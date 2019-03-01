// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019, Vladimir Oltean <olteanv@gmail.com>
 */
#include <linux/etherdevice.h>
/*#include <linux/list.h>*/
/*#include <linux/slab.h>*/
#include <linux/if_vlan.h>

#include "dsa_priv.h"

#define dsa_port_from_pseudo_pvid(pvid) \
	(pvid - 4000)

static struct sk_buff *pseudo_8021q_xmit(struct sk_buff *skb,
					 struct net_device *netdev)
{
	struct dsa_port *dp = dsa_slave_to_port(netdev);
	u16 pvid = dsa_get_pseudo_pvid(dp->ds, dp->index);
	u8 pcp = skb->priority;

	/* skb->data points at skb_mac_header, which
	 * is fine for vlan_insert_tag.
	 */
	return vlan_insert_tag(skb, ETH_P_EDSA,
			     ((pcp << VLAN_PRIO_SHIFT) | pvid));
}

static struct sk_buff *pseudo_8021q_rcv(struct sk_buff *skb, struct net_device *netdev,
				   struct packet_type *pt)
{
	unsigned int source_port;
	struct vlan_ethhdr *tag;
	u16 tpid;
	u16 vid;
	u16 tci;

	tag = vlan_eth_hdr(skb);
	tpid = ntohs(tag->h_vlan_proto);
	if (tpid != ETH_P_EDSA) {
		netdev_warn(netdev, "Invalid pseudo-VLAN marker 0x%x\n", tpid);
		return NULL;
	}
	tci = ntohs(tag->h_vlan_TCI);
	skb->priority = (tci & VLAN_PRIO_MASK) >> VLAN_PRIO_SHIFT;
	vid = tci & VLAN_VID_MASK;
	source_port = dsa_port_from_pseudo_pvid(vid);

	skb->dev = dsa_master_find_slave(netdev, 0, source_port);
	if (!skb->dev) {
		netdev_warn(netdev, "Packet with invalid source port %u\n",
			    source_port);
		return NULL;
	}

	/* skb->data points in the middle of the VLAN tag,
	 * after tpid and before tci. This is because so far,
	 * ETH_HLEN (DMAC, SMAC, EtherType) bytes were pulled.
	 * There are 2 bytes of VLAN tag left in skb->data, and upper
	 * layers expect the 'real' EtherType to be consumed as well.
	 * Coincidentally, a VLAN header is also of the same size as
	 * the number of bytes that need to be pulled.
	 */
	skb_pull_rcsum(skb, VLAN_HLEN);
	/* Delete/overwrite VLAN header, DSA expects to not find it there,
	 * see dsa_switch_rcv: skb_push(skb, ETH_HLEN).
	 */
	memmove(skb->data - ETH_HLEN, skb->data - ETH_HLEN - VLAN_HLEN,
		2 * ETH_ALEN);
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	skb_reset_transport_header(skb);
	skb->ip_summed = CHECKSUM_NONE;
	return skb;
}

const struct dsa_device_ops pseudo_8021q_netdev_ops = {
	.xmit = pseudo_8021q_xmit,
	.rcv = pseudo_8021q_rcv,
	.overhead = VLAN_HLEN,
};

