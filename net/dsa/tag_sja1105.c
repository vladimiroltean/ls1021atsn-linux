// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019, Vladimir Oltean <olteanv@gmail.com>
 */
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/dsa/sja1105.h>
#include "../../drivers/net/dsa/sja1105/sja1105.h"

#include "dsa_priv.h"

/* Similar to is_link_local_ether_addr(hdr->h_dest) but also covers PTP */
static inline bool sja1105_is_link_local(struct sk_buff *skb)
{
	struct ethhdr *hdr = eth_hdr(skb);
	u64 dmac = ether_addr_to_u64(hdr->h_dest);

	if ((dmac & SJA1105_LINKLOCAL_FILTER_A_MASK) ==
		    SJA1105_LINKLOCAL_FILTER_A)
		return true;
	if ((dmac & SJA1105_LINKLOCAL_FILTER_B_MASK) ==
		    SJA1105_LINKLOCAL_FILTER_B)
		return true;
	return false;
}

static struct sk_buff *sja1105_xmit(struct sk_buff *skb,
				    struct net_device *netdev)
{
	struct dsa_port *dp = dsa_slave_to_port(netdev);
	struct dsa_switch *ds = dp->ds;
	struct sja1105_private *priv = ds->priv;
	struct sja1105_port *sp = &priv->ports[dp->index];
	struct sk_buff *clone;

	if (likely(!sja1105_is_link_local(skb))) {
		/* Normal traffic path. */
		u16 tx_vid = dsa_tagging_tx_vid(ds, dp->index);
		u8 pcp = skb->priority;

		/* If we are under a vlan_filtering bridge, IP termination on
		 * switch ports based on 802.1Q tags is simply too brittle to
		 * be passable. So just defer to the dsa_slave_notag_xmit
		 * implementation.
		 */
		if (dp->vlan_filtering)
			return skb;

		return dsa_8021q_xmit(skb, netdev, ETH_P_EDSA,
				     ((pcp << VLAN_PRIO_SHIFT) | tx_vid));
	}

	/* Code path for transmitting management traffic. This does not rely
	 * upon switch tagging, but instead SPI-installed management routes.
	 */
	clone = skb_clone(skb, GFP_ATOMIC);
	if (!clone) {
		dev_err(ds->dev, "xmit: failed to clone skb\n");
		return NULL;
	}

	if (sja1105_skb_ring_add(&sp->xmit_ring, clone) < 0) {
		dev_err(ds->dev, "xmit: skb ring full\n");
		kfree_skb(clone);
		return NULL;
	}

	if (sp->xmit_ring.count == SJA1105_SKB_RING_SIZE)
		/* TODO setup a dedicated netdev queue for management traffic
		 * so that we can selectively apply backpressure and not be
		 * required to stop the entire traffic when the software skb
		 * ring is full. This requires hooking the ndo_select_queue
		 * from DSA and matching on mac_fltres.
		 */
		dev_err(ds->dev, "xmit: reached maximum skb ring size\n");

	schedule_work(&sp->xmit_work);
	/* Let DSA free its reference to the skb and we will free
	 * the clone in the deferred worker
	 */
	return NULL;
}

static struct sk_buff *sja1105_rcv(struct sk_buff *skb,
				   struct net_device *netdev,
				   struct packet_type *pt)
{
	unsigned int source_port, switch_id;
	struct ethhdr *hdr = eth_hdr(skb);
	u16 tpid, vid, tci;

	skb = dsa_8021q_rcv(skb, netdev, pt, &tpid, &tci);
	if (!skb)
		return NULL;

	if (tpid != ETH_P_EDSA) {
		netdev_warn(netdev, "TPID 0x%04x not for tagging\n", tpid);
		return NULL;
	}

	skb->priority = (tci & VLAN_PRIO_MASK) >> VLAN_PRIO_SHIFT;
	vid = tci & VLAN_VID_MASK;

	skb->offload_fwd_mark = 1;

	if (likely(!sja1105_is_link_local(skb))) {
		/* Normal traffic path. */
		source_port = dsa_tagging_rx_source_port(vid);
		switch_id = dsa_tagging_rx_switch_id(vid);
	} else {
		/* Management traffic path. Switch embeds the switch ID and
		 * port ID into bytes of the destination MAC, courtesy of
		 * the incl_srcpt options.
		 */
		source_port = hdr->h_dest[3];
		switch_id = hdr->h_dest[4];
		/* Clear the DMAC bytes that were mangled by the switch */
		hdr->h_dest[3] = 0;
		hdr->h_dest[4] = 0;
	}

	skb->dev = dsa_master_find_slave(netdev, switch_id, source_port);
	if (!skb->dev) {
		netdev_warn(netdev, "Packet with invalid switch id %u and source port %u\n",
			    switch_id, source_port);
		return NULL;
	}

	/* Delete/overwrite fake VLAN header, DSA expects to not find
	 * it there, see dsa_switch_rcv: skb_push(skb, ETH_HLEN).
	 */
	memmove(skb->data - ETH_HLEN, skb->data - ETH_HLEN - VLAN_HLEN,
		ETH_HLEN - VLAN_HLEN);

	return skb;
}

const struct dsa_device_ops sja1105_netdev_ops = {
	.xmit = sja1105_xmit,
	.rcv = sja1105_rcv,
	.overhead = VLAN_HLEN,
};

