/* Included by drivers/net/dsa/sja1105/sja1105.h and net/dsa/tag_sja1105.c */

#ifndef _NET_DSA_SJA1105_H
#define _NET_DSA_SJA1105_H

#include <linux/skbuff.h>
#include <net/dsa.h>

#define SJA1105_SKB_RING_SIZE    20

struct sja1105_skb_ring {
	struct sk_buff *skb[SJA1105_SKB_RING_SIZE];
	int count;
	int pi; /* Producer index */
	int ci; /* Consumer index */
};

static inline int sja1105_skb_ring_add(struct sja1105_skb_ring *ring,
				       struct sk_buff *skb)
{
	int index;

	if (ring->count == SJA1105_SKB_RING_SIZE)
		return -1;

	index = ring->pi;
	ring->skb[index] = skb;
	ring->pi = (index + 1) % SJA1105_SKB_RING_SIZE;
	ring->count++;
	return index;
}

static inline int sja1105_skb_ring_get(struct sja1105_skb_ring *ring,
				       struct sk_buff **skb)
{
	int index;

	if (ring->count == 0)
		return -1;

	index = ring->ci;
	*skb = ring->skb[index];
	ring->ci = (index + 1) % SJA1105_SKB_RING_SIZE;
	ring->count--;
	return index;
}

#endif /* _NET_DSA_SJA1105_Hk*/
