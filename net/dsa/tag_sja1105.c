// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019, Vladimir Oltean <olteanv@gmail.com>
 */
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/dsa/sja1105.h>
#include "../../drivers/net/dsa/sja1105/sja1105.h"

#include "dsa_priv.h"

static struct sk_buff *dsa_8021q_xmit(struct sk_buff *skb,
				      struct net_device *netdev,
				      u16 tpid, u16 tci)
{
	/* skb->data points at skb_mac_header, which
	 * is fine for vlan_insert_tag.
	 */
	return vlan_insert_tag(skb, tpid, tci);
}

static struct sk_buff *dsa_8021q_rcv(struct sk_buff *skb, struct net_device *netdev,
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

static struct sk_buff *sja1105_xmit(struct sk_buff *skb,
				    struct net_device *netdev)
{
	struct dsa_port *dp = dsa_slave_to_port(netdev);
	struct dsa_switch *ds = dp->ds;
	u16 pvid = ds->ops->tagging_vid_from_port(ds, dp->index);
	struct sja1105_private *priv = ds->priv;
	struct sja1105_port *sp = &priv->ports[dp->index];
	u8 pcp = skb->priority;
	struct ethhdr *hdr = eth_hdr(skb);
	u64 dmac = ether_addr_to_u64(hdr->h_dest);
	struct sja1105_general_params_entry *gp;
	struct sk_buff *clone;
	u16 tpid;

	gp = priv->static_config.tables[BLK_IDX_GENERAL_PARAMS].entries;

	if (dp->vlan_filtering)
		tpid = ETH_P_8021Q;
	else
		tpid = ETH_P_EDSA;

	skb = dsa_8021q_xmit(skb, netdev, tpid,
			     ((pcp << VLAN_PRIO_SHIFT) | pvid));
	if (!skb) {
		dev_err(ds->dev, "xmit: vlan_insert_tag failed\n");
		return NULL;
	}

	/* Similar to !is_link_local_ether_addr(hdr->h_dest)
	 * but also covers PTP
	 */
	if (likely((dmac & gp->mac_flt0) != gp->mac_fltres0) &&
		  ((dmac & gp->mac_flt1) != gp->mac_fltres1))
		return skb;

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

	if (sp->xmit_ring.count == SJA1105_SKB_RING_SIZE) {
		dev_err(ds->dev, "xmit: reached maximum skb ring size\n");
#if 0
		/* TODO setup a dedicated netdev queue for management traffic so that
		 * we can selectively apply backpressure and not be required to stop
		 * the entire traffic when the software skb ring is full. This requires
		 * hooking the ndo_select_queue from DSA and matching on mac_fltres.
		 */
		netif_stop_queue(netdev);
#endif
	}

	schedule_work(&sp->xmit_work);
	/* Let DSA free its reference to the skb and we will free
	 * the clone in the deferred worker
	 */
	return NULL;
}

static struct sk_buff *sja1105_rcv(struct sk_buff *skb, struct net_device *netdev,
				   struct packet_type *pt)
{
	/* TODO nicer way to get a hold of switch handle? */
	struct dsa_port *dp = dsa_slave_to_port(dsa_master_find_slave(netdev, 0, 0));
	unsigned int source_port;
	u16 tpid;
	u16 vid;
	u16 tci;

	skb = dsa_8021q_rcv(skb, netdev, pt, &tpid, &tci);
	if (!skb)
		return NULL;

	if (tpid == ETH_P_EDSA) {
		/* Delete/overwrite fake VLAN header, DSA expects to not find
		 * it there, see dsa_switch_rcv: skb_push(skb, ETH_HLEN).
		 */
		memmove(skb->data - ETH_HLEN, skb->data - ETH_HLEN - VLAN_HLEN,
			ETH_HLEN - VLAN_HLEN);
	} else if (unlikely(tpid != ETH_P_8021Q && tpid != ETH_P_8021AD)) {
		netdev_warn(netdev, "TPID 0x%04x not for tagging\n", tpid);
		return NULL;
	}
	skb->priority = (tci & VLAN_PRIO_MASK) >> VLAN_PRIO_SHIFT;
	vid = tci & VLAN_VID_MASK;
	source_port = dp->ds->ops->tagging_vid_to_port(dp->ds, vid);

	skb->dev = dsa_master_find_slave(netdev, 0, source_port);
	if (!skb->dev) {
		netdev_warn(netdev, "Packet with invalid source port %u\n",
			    source_port);
		return NULL;
	}

	skb->offload_fwd_mark = 1;

	return skb;
}

const struct dsa_device_ops sja1105_netdev_ops = {
	.xmit = sja1105_xmit,
	.rcv = sja1105_rcv,
	.overhead = VLAN_HLEN,
};

