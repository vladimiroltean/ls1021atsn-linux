// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019, Vladimir Oltean <olteanv@gmail.com>
 */
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>

#include "dsa_priv.h"

#define DSA_TAGGING_VID_RANGE    (DSA_MAX_SWITCHES * DSA_MAX_PORTS)
#define DSA_TAGGING_VID_BASE     (VLAN_N_VID - 2 * DSA_TAGGING_VID_RANGE - 1)
#define DSA_TAGGING_RX_VID_BASE  (DSA_TAGGING_VID_BASE)
#define DSA_TAGGING_TX_VID_BASE  (DSA_TAGGING_VID_BASE + DSA_TAGGING_VID_RANGE)

u16 dsa_tagging_tx_vid(struct dsa_switch *ds, int port)
{
	return DSA_TAGGING_TX_VID_BASE + (DSA_MAX_PORTS * ds->index) + port;
}

u16 dsa_tagging_rx_vid(struct dsa_switch *ds, int port)
{
	return DSA_TAGGING_RX_VID_BASE + (DSA_MAX_PORTS * ds->index) + port;
}

int dsa_tagging_rx_switch_id(u16 vid)
{
	return ((vid - DSA_TAGGING_RX_VID_BASE) / DSA_MAX_PORTS);
}

int dsa_tagging_rx_source_port(u16 vid)
{
	return ((vid - DSA_TAGGING_RX_VID_BASE) % DSA_MAX_PORTS);
}

/* Rx VLAN tagging (left) and Tx VLAN tagging (right) setup shown for a single
 * front-panel switch port (here swp0).
 *
 * Port identification through VLAN (802.1Q) tags has different requirements
 * for it to work effectively:
 *  - On Rx (ingress from network): each front-panel port must have a pvid
 *    that uniquely identifies it, and the egress of this pvid must be tagged
 *    towards the CPU port, so that software can recover the source port based
 *    on the VID in the frame. But this would only work for standalone ports;
 *    if bridged, this VLAN setup would break autonomous forwarding and would
 *    force all switched traffic to pass through the CPU. So we must also make
 *    the other front-panel ports members of this VID we're adding, albeit
 *    we're not making it their PVID (they'll still have their own).
 *    By the way - just because we're installing the same VID in multiple
 *    switch ports doesn't mean that they'll start to talk to one another, even
 *    while not bridged: the final forwarding decision is still an AND between
 *    the L2 forwarding information (which is limiting forwarding in this case)
 *    and the VLAN-based restrictions (of which there are none in this case,
 *    since all ports are members).
 *  - On Tx (ingress from CPU and towards network) we are faced with a problem.
 *    If we were to tag traffic (from within DSA) with the port's pvid, all
 *    would be well, assuming the switch ports were standalone. Frames would
 *    have no choice but to be directed towards the correct front-panel port.
 *    But because we also want the Rx VLAN to not break bridging, then
 *    inevitably that means that we have to give them a choice (of what
 *    front-panel port to go out on), and therefore we cannot steer traffic
 *    based on the Rx VID. So what we do is simply install one more VID on the
 *    front-panel and CPU ports, and profit off of the fact that steering will
 *    work just by virtue of the fact that there is only one other port that's
 *    a member of the VID we're tagging the traffic with - the desired one.
 *
 * So at the end, each front-panel port will have one Rx VID (also the PVID),
 * the Rx VID of all other front-panel ports, and one Tx VID. Whereas the CPU
 * port will have the Rx and Tx VIDs of all front-panel ports, and on top of
 * that, is also tagged-input and tagged-output (VLAN trunk).
 *
 *               CPU port                               CPU port
 * +-------------+-----+-------------+    +-------------+-----+-------------+
 * |  Rx VID     |     |             |    |  Tx VID     |     |             |
 * |  of swp0    |     |             |    |  of swp0    |     |             |
 * |             +-----+             |    |             +-----+             |
 * |                ^ T              |    |                | Tagged         |
 * |                |                |    |                | ingress        |
 * |    +-------+---+---+-------+    |    |    +-----------+                |
 * |    |       |       |       |    |    |    | Untagged                   |
 * |    |     U v     U v     U v    |    |    v egress                     |
 * | +-----+ +-----+ +-----+ +-----+ |    | +-----+ +-----+ +-----+ +-----+ |
 * | |     | |     | |     | |     | |    | |     | |     | |     | |     | |
 * | |PVID | |     | |     | |     | |    | |     | |     | |     | |     | |
 * +-+-----+-+-----+-+-----+-+-----+-+    +-+-----+-+-----+-+-----+-+-----+-+
 *   swp0    swp1    swp2    swp3           swp0    swp1    swp2    swp3
 */
int dsa_port_setup_8021q_tagging(struct dsa_switch *ds, int port, bool enabled)
{
	int upstream = dsa_upstream_port(ds, port);
	struct dsa_port *dp = &ds->ports[port];
	struct dsa_port *upstream_dp = &ds->ports[upstream];
	u16 rx_vid = dsa_tagging_rx_vid(ds, port);
	u16 tx_vid = dsa_tagging_tx_vid(ds, port);
	int i, err;

	/* The CPU port is implicitly configured by
	 * configuring the front-panel ports
	 */
	if (!dsa_is_user_port(ds, port))
		return 0;

	/* Add this user port's Rx VID to the membership list of all others
	 * (including itself). This is so that bridging will not be hindered.
	 * L2 forwarding rules still take precedence when there are no VLAN
	 * restrictions, so there are no concerns about leaking traffic.
	 */
	for (i = 0; i < ds->num_ports; i++) {
		struct dsa_port *other_dp = &ds->ports[i];
		u16 flags;

		if (i == upstream)
			/* CPU port needs to see this port's Rx VID
			 * as tagged egress.
			 */
			flags = 0;
		else if (i == port)
			/* The Rx VID is pvid on this port */
			flags = BRIDGE_VLAN_INFO_UNTAGGED |
				BRIDGE_VLAN_INFO_PVID;
		else
			/* The Rx VID is a regular VLAN on all others */
			flags = BRIDGE_VLAN_INFO_UNTAGGED;

		if (enabled)
			err = __dsa_port_vlan_add(other_dp, rx_vid, flags);
		else
			err = __dsa_port_vlan_del(other_dp, rx_vid);
		if (err) {
			dev_err(ds->dev, "Failed to apply Rx VID %d to port %d: %d\n",
				rx_vid, port, err);
			return err;
		}
	}
	/* Finally apply the Tx VID on this port and on the CPU port */
	if (enabled)
		err = __dsa_port_vlan_add(dp, tx_vid,
					  BRIDGE_VLAN_INFO_UNTAGGED);
	else
		err = __dsa_port_vlan_del(dp, tx_vid);
	if (err) {
		dev_err(ds->dev, "Failed to apply Tx VID %d on port %d: %d\n",
			tx_vid, port, err);
		return err;
	}
	if (enabled)
		err = __dsa_port_vlan_add(upstream_dp, tx_vid, 0);
	else
		err = __dsa_port_vlan_del(upstream_dp, tx_vid);
	if (err) {
		dev_err(ds->dev, "Failed to apply Tx VID %d on port %d: %d\n",
			tx_vid, upstream, err);
		return err;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(dsa_port_setup_8021q_tagging);

struct sk_buff *dsa_8021q_xmit(struct sk_buff *skb, struct net_device *netdev,
			       u16 tpid, u16 tci)
{
	/* skb->data points at skb_mac_header, which
	 * is fine for vlan_insert_tag.
	 */
	return vlan_insert_tag(skb, tpid, tci);
}
EXPORT_SYMBOL_GPL(dsa_8021q_xmit);

struct sk_buff *dsa_8021q_rcv(struct sk_buff *skb, struct net_device *netdev,
			      struct packet_type *pt, u16 *tpid, u16 *tci)
{
	struct vlan_ethhdr *tag;

	if (unlikely(!pskb_may_pull(skb, VLAN_HLEN)))
		return NULL;

	tag = vlan_eth_hdr(skb);
	*tpid = ntohs(tag->h_vlan_proto);
	*tci = ntohs(tag->h_vlan_TCI);

	/* skb->data points in the middle of the VLAN tag,
	 * after tpid and before tci. This is because so far,
	 * ETH_HLEN (DMAC, SMAC, EtherType) bytes were pulled.
	 * There are 2 bytes of VLAN tag left in skb->data, and upper
	 * layers expect the 'real' EtherType to be consumed as well.
	 * Coincidentally, a VLAN header is also of the same size as
	 * the number of bytes that need to be pulled.
	 */
	skb_pull_rcsum(skb, VLAN_HLEN);

	return skb;
}
EXPORT_SYMBOL_GPL(dsa_8021q_rcv);

