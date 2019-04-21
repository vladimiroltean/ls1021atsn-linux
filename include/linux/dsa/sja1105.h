/* SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2019, Vladimir Oltean <olteanv@gmail.com>
 */

/* Included by drivers/net/dsa/sja1105/sja1105.h and net/dsa/tag_sja1105.c */

#ifndef _NET_DSA_SJA1105_H
#define _NET_DSA_SJA1105_H

#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <net/dsa.h>

/* IEEE 802.3 Annex 57A: Slow Protocols PDUs (01:80:C2:xx:xx:xx) */
#define SJA1105_LINKLOCAL_FILTER_A	0x0180C2000000ull
#define SJA1105_LINKLOCAL_FILTER_A_MASK	0xFFFFFF000000ull
/* IEEE 1588 Annex F: Transport of PTP over Ethernet (01:1B:19:xx:xx:xx) */
#define SJA1105_LINKLOCAL_FILTER_B	0x011B19000000ull
#define SJA1105_LINKLOCAL_FILTER_B_MASK	0xFFFFFF000000ull

/* Source and Destination MAC of follow-up meta frames */
#define SJA1105_META_SMAC		0x222222222222ull
#define SJA1105_META_DMAC		0x0180C2000000ull

#define SJA1105_STATE_META_ARRIVED	0

#define SJA1105_FRAME_TYPE_LINK_LOCAL	BIT(0)
#define SJA1105_FRAME_TYPE_META		BIT(1)

struct sja1105_skb_cb {
	unsigned long type;
	unsigned long state;
	ktime_t orig_time;
	u32 meta_tstamp;
};

#define SJA1105_SKB_CB(skb) \
	((struct sja1105_skb_cb *)DSA_SKB_CB_PRIV(skb))

struct sja1105_port {
	struct dsa_port *dp;
	bool rgmii_rx_delay;
	bool rgmii_tx_delay;
	int mgmt_slot;
	bool hwts_tx_en;
	bool hwts_rx_en;
	bool expect_meta;
	struct work_struct rxtstamp_work;
	struct sk_buff *last_stampable_skb;
	struct sk_buff_head skb_rxtstamp_queue;
};

/* Similar to is_link_local_ether_addr(hdr->h_dest) but also covers PTP */
static inline bool sja1105_is_link_local(const struct sk_buff *skb)
{
	const struct ethhdr *hdr = eth_hdr(skb);
	u64 dmac = ether_addr_to_u64(hdr->h_dest);

	if ((dmac & SJA1105_LINKLOCAL_FILTER_A_MASK) ==
		    SJA1105_LINKLOCAL_FILTER_A)
		return true;
	if ((dmac & SJA1105_LINKLOCAL_FILTER_B_MASK) ==
		    SJA1105_LINKLOCAL_FILTER_B)
		return true;
	return false;
}

static inline bool sja1105_is_meta_frame(const struct sk_buff *skb)
{
	const struct ethhdr *hdr = eth_hdr(skb);
	u64 smac = ether_addr_to_u64(hdr->h_source);
	u64 dmac = ether_addr_to_u64(hdr->h_dest);

	if (smac != SJA1105_META_SMAC)
		return false;
	if (dmac != SJA1105_META_DMAC)
		return false;
	if (hdr->h_proto != ETH_P_IP)
		return false;
	return true;
}

#endif /* _NET_DSA_SJA1105_H */
