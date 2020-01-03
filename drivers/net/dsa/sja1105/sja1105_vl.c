// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019, Vladimir Oltean <olteanv@gmail.com>
 */
#include <linux/dsa/8021q.h>
#include "sja1105.h"

/* The switch flow classification core implements ARINC 664 part 7 (AFDX) and
 * 'thinks' in terms of Virtual Links (VL). However it also has one other
 * operating mode (VLLUPFORMAT=0) where it acts closer to a pre-standard
 * implementation of IEEE 802.1Qci (Per-Stream Filtering and Policing), which is
 * what the driver is going to be implementing.
 *
 *                                 VL Lookup
 *        Key = {DMAC && VLANID   +---------+  Key = { (DMAC[47:16] & VLMASK ==
 *               && VLAN PCP      |         |                         VLMARKER)
 *               && INGRESS PORT} +---------+                      (both fixed)
 *            (exact match,            |             && DMAC[15:0] == VLID
 *         all specified in rule)      |                    (specified in rule)
 *                                     v             && INGRESS PORT }
 *                               ------------
 *                    0 (PSFP)  /            \  1 (ARINC664)
 *                 +-----------/  VLLUPFORMAT \----------+
 *                 |           \    (fixed)   /          |
 *                 |            \            /           |
 *  0 (forwarding) v             ------------            |
 *           ------------                                |
 *          /            \  1 (QoS classification)       |
 *     +---/  ISCRITICAL  \-----------+                  |
 *     |   \  (per rule)  /           |                  |
 *     |    \            /   VLID taken from      VLID taken from
 *     v     ------------     index of rule       contents of rule
 *  select                     that matched         that matched
 * DESTPORTS                          |                  |
 *  |                                 +---------+--------+
 *  |                                           |
 *  |                                           v
 *  |                                     VL Forwarding
 *  |                                   (indexed by VLID)
 *  |                                      +---------+
 *  |                                      |         |
 *  |                                      +---------+
 *  |                                           |
 *  |                                select TYPE, PRIORITY,
 *  |                                 PARTITION, DESTPORTS
 *  |                                           |
 *  |                       +-------------------+
 *  |                       |
 *  |                       v
 *  |   0 (rate      ------------    1 (time
 *  |  constrained) /            \   triggered)
 *  |       +------/     TYPE     \------------+
 *  |       |      \  (per VLID)  /            |
 *  |       v       \            /             v
 *  |  VL Policing   ------------         VL Policing
 *  |  +---------+                        +---------+
 *  |  |         |                        |         |
 *  |  +---------+                        +---------+
 *  |  select SHARINDX                 select SHARINDX to
 *  |  to rate-limit                 re-enter VL Forwarding
 *  |  groups of VL's               with new VLID for egress
 *  |  to same quota                           |
 *  |       |                                  v
 *  |       v                            select MAXLEN
 *  |  select MAXLEN,                          |
 *  |   BAG, JITTER                            v
 *  |       |             ----------------------------------------------
 *  |       v            /    Reception Window is open for this VL      \
 *  |  exceed => drop   /    (the Schedule Table executes an entry i     \
 *  |       |          /   M <= i < N, for which these conditions hold):  \ no
 *  |       |    +----/                                                    \-+
 *  |       |    |yes \       WINST[M] == 1 && WINSTINDEX[M] == VLID       / |
 *  |       |    |     \     WINEND[N] == 1 && WINSTINDEX[N] == VLID      /  |
 *  |       |    |      \                                                /   |
 *  |       |    |       \ (the VL window has opened and not yet closed)/    |
 *  |       |    |        ----------------------------------------------     |
 *  |       |    v                                                           v
 *  |       |  dispatch to DESTPORTS when the Schedule Table               drop
 *  |       |  executes an entry i with TXEN == 1 && VLINDEX == i
 *  v       v
 * dispatch immediately to DESTPORTS
 *
 * The per-port classification key is always composed of {DMAC, VID, PCP} and
 * is non-maskable. This 'looks like' the NULL stream identification function
 * from IEEE 802.1CB clause 6, except for the extra VLAN PCP (which we could
 * allow the user to not specify, and then the port-based default will be
 * used).
 *
 * The action is always a full triplet of:
 * a. 'police': (rate-based or time-based) and size-based
 * b. 'flow steering': select the traffic class on the egress port
 * c. 'redirect': select the egress port list
 *
 * For c, using Virtual Links to forward traffic based on {DMAC, VID} is quite
 * pointless, since that's what the FDB is for. The DESTPORTS feature would
 * have had sense if we had used VLLUPFORMAT=1 (ARINC 664), which I already
 * said we aren't going to.
 * But nonetheless, specifying DESTPORTS in the action is mandatory. So what we
 * can do is to only allow the user to specify a VL rule for a {DMAC, VLAN}
 * pair for which a static FDB entry has already been input. Then we can take
 * the matching ports from the driver's list of static FDB entries and use
 * those to populate DESTPORTS for the action. The end result is that the
 * static FDB entry will be shadowed by a VL rule, but the forwarding decision
 * will still be as per user request.
 *
 * For a and b, it would be nice to be able to do flow steering and policing as
 * independent actions. So we can do:
 * - Policing without flow steering: configure the PRIORITY of the VL to be the
 *   port-based default priority.
 * - Flow steering without policing: configure the VL as ISCRITICAL=1 and
 *   TYPE=0 (rate constrained), but set BAG=0 and JITTER=0 to disable the
 *   bandwidth checks.
 */



/* Test environment:
 *       +----------------------------+
 *       |           Switch           |
 *       |                            |
 *       | swp5       swp3       eth1 |
 *       | swp4       swp2       eth0 |
 *       +--|----------|--------------+
 *          |          |
 *      +---+          +--------------+
 *      |                             |
 * +---------+                   +---------+
 * |   Host  |                   |   Host  |
 * |    A    |                   |    B    |
 * +---------+                   +---------+
 *
 * Host A runs:
 * arp -s 10.0.0.200 01:02:03:04:05:06 dev eth0
 * ping -f 10.0.0.200
 *
 * The switch runs:
 * ./trapping.sh
 * tcpdump -i eth2
 * and sees all packets
 *
 * Host B runs:
 * tcpdump -i eth0
 * and sees no packet
 */
int sja1105_init_virtual_links(struct sja1105_private *priv)
{
	struct sja1105_vl_lookup_entry *vl_lookup;
	struct sja1105_table *table;
	/*const int swp2 = 1;*/
	/*const int swp3 = 2;*/
	const int swp4 = 3;
	/*const int swp5 = 0;*/
	const int cpu = 4;

	table = &priv->static_config.tables[BLK_IDX_VL_LOOKUP];

	if (table->entry_count) {
		kfree(table->entries);
		table->entry_count = 0;
	}

	table->entries = kcalloc(1, table->ops->unpacked_entry_size,
				 GFP_KERNEL);
	if (!table->entries)
		return -ENOMEM;

	table->entry_count = 1;
	vl_lookup = table->entries;

	/* On swp4, trap all incoming frames with a DMAC of 01:02:03:04:05:06
	 * to the CPU. Use of dsa_8021q_rx_vid requires vlan_filtering=0 on the
	 * bridge. Alternatively, any VLAN ID can be used, but the packet will
	 * have to be consumed by the DSA master somehow, or custom code added
	 * to tag_sja1105.c.
	 */
	vl_lookup[0].destports = BIT(cpu);
	vl_lookup[0].iscritical = false;
	vl_lookup[0].macaddr = 0x010203040506;
	vl_lookup[0].vlanid = dsa_8021q_rx_vid(priv->ds, swp4);
	vl_lookup[0].vlanprior = 0;
	vl_lookup[0].port = swp4;

	return 0;
}
