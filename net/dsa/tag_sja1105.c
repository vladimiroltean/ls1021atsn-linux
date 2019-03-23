// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019, Vladimir Oltean <olteanv@gmail.com>
 */
#include <linux/if_vlan.h>
#include <linux/dsa/sja1105.h>
#include <linux/packing.h>
#include "dsa_priv.h"

static bool sja1105_filter(const struct sk_buff *skb, struct net_device *dev)
{
	if (sja1105_is_link_local(skb)) {
		SJA1105_SKB_CB(skb)->type = SJA1105_FRAME_TYPE_LINK_LOCAL;
		return true;
	}
	if (!dev->dsa_ptr->vlan_filtering)
		return true;
	return false;
}

static struct sk_buff *sja1105_xmit(struct sk_buff *skb,
				    struct net_device *netdev)
{
	struct dsa_port *dp = dsa_slave_to_port(netdev);
	struct dsa_switch *ds = dp->ds;
	u16 tx_vid = dsa_tagging_tx_vid(ds, dp->index);
	u8 pcp = skb->priority;

	/* Transmitting management traffic does not rely upon switch tagging,
	 * but instead SPI-installed management routes.
	 */
	if (unlikely(sja1105_is_link_local(skb))) {
		dsa_defer_xmit(skb, netdev);
		/* Let DSA free its reference to the skb and we will free
		 * the clone in the deferred worker
		 */
		return NULL;
	}

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

static struct sk_buff *sja1105_rcv(struct sk_buff *skb,
				   struct net_device *netdev,
				   struct packet_type *pt)
{
	struct ethhdr *hdr = eth_hdr(skb);
	u64 source_port, switch_id;
	struct sk_buff *nskb;
	u16 tpid, vid, tci;
	bool is_tagged;

	nskb = dsa_8021q_rcv(skb, netdev, pt, &tpid, &tci);
	is_tagged = (nskb && tpid == ETH_P_EDSA);

	skb->priority = (tci & VLAN_PRIO_MASK) >> VLAN_PRIO_SHIFT;
	vid = tci & VLAN_VID_MASK;

	skb->offload_fwd_mark = 1;

	if (SJA1105_SKB_CB(skb)->type & SJA1105_FRAME_TYPE_LINK_LOCAL) {
		/* Management traffic path. Switch embeds the switch ID and
		 * port ID into bytes of the destination MAC, courtesy of
		 * the incl_srcpt options.
		 */
		source_port = hdr->h_dest[3];
		switch_id = hdr->h_dest[4];
		/* Clear the DMAC bytes that were mangled by the switch */
		hdr->h_dest[3] = 0;
		hdr->h_dest[4] = 0;
	} else {
		/* Normal traffic path. */
		source_port = dsa_tagging_rx_source_port(vid);
		switch_id = dsa_tagging_rx_switch_id(vid);
	}

	skb->dev = dsa_master_find_slave(netdev, switch_id, source_port);
	if (!skb->dev) {
		netdev_warn(netdev, "Couldn't decode source port\n");
		return NULL;
	}

	/* Delete/overwrite fake VLAN header, DSA expects to not find
	 * it there, see dsa_switch_rcv: skb_push(skb, ETH_HLEN).
	 */
	if (is_tagged)
		memmove(skb->data - ETH_HLEN, skb->data - ETH_HLEN - VLAN_HLEN,
			ETH_HLEN - VLAN_HLEN);

	return skb;
}

const struct dsa_device_ops sja1105_netdev_ops = {
	.xmit = sja1105_xmit,
	.rcv = sja1105_rcv,
	.filter = sja1105_filter,
	.overhead = VLAN_HLEN,
};

