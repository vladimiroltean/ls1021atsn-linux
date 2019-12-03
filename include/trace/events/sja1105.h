/* SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2019, Vladimir Oltean <olteanv@gmail.com>
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM	sja1105

#if !defined(_NET_DSA_SJA1105_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _NET_DSA_SJA1105_TRACE_H

#include <linux/dsa/sja1105.h>
#include <linux/tracepoint.h>

TRACE_EVENT(sja1105_stampable_skb,

	TP_PROTO(const struct sk_buff *skb),

	TP_ARGS(skb),

	TP_STRUCT__entry(
		__string(	name,			skb->dev->name	)
		__field(	const void *,		skbaddr		)
	),

	TP_fast_assign(
		__assign_str(name, skb->dev->name);
		__entry->skbaddr = skb;
	),

	TP_printk("dev=%s skbaddr=%p",
		  __get_str(name), __entry->skbaddr)
);

TRACE_EVENT(sja1105_meta,

	TP_PROTO(const struct sk_buff *skb, const struct sja1105_meta *meta),

	TP_ARGS(skb, meta),

	TP_STRUCT__entry(
		__string(	name,			skb->dev->name	)
		__field(	const void *,		skbaddr		)
		__field(	u32,			tstamp		)
	),

	TP_fast_assign(
		__assign_str(name, skb->dev->name);
		__entry->skbaddr = skb;
		__entry->tstamp = meta->tstamp;
	),

	TP_printk("dev=%s skbaddr=%p tstamp=0x%x",
		  __get_str(name), __entry->skbaddr, __entry->tstamp)
);

TRACE_EVENT(sja1105_txtstamp_start,

	TP_PROTO(const struct sk_buff *skb, int tsreg),

	TP_ARGS(skb, tsreg),

	TP_STRUCT__entry(
		__string(	name,			skb->dev->name	)
		__field(	const void *,		skbaddr		)
		__field(	int,			tsreg		)
	),

	TP_fast_assign(
		__assign_str(name, skb->dev->name);
		__entry->skbaddr = skb;
		__entry->tsreg = tsreg;
	),

	TP_printk("dev=%s skbaddr=%p tsreg=%d",
		  __get_str(name), __entry->skbaddr, __entry->tsreg)
);

TRACE_EVENT(sja1105_rxtstamp_start,

	TP_PROTO(const struct sk_buff *skb),

	TP_ARGS(skb),

	TP_STRUCT__entry(
		__string(	name,			skb->dev->name	)
		__field(	const void *,		skbaddr		)
	),

	TP_fast_assign(
		__assign_str(name, skb->dev->name);
		__entry->skbaddr = skb;
	),

	TP_printk("dev=%s skbaddr=%p",
		  __get_str(name), __entry->skbaddr)
);

TRACE_EVENT(sja1105_txtstamp_end,

	TP_PROTO(const struct sk_buff *skb, u64 tstamp),

	TP_ARGS(skb, tstamp),

	TP_STRUCT__entry(
		__string(	name,			skb->dev->name	)
		__field(	const void *,		skbaddr		)
		__field(	u64,			tstamp		)
	),

	TP_fast_assign(
		__assign_str(name, skb->dev->name);
		__entry->skbaddr = skb;
		__entry->tstamp = tstamp;
	),

	TP_printk("dev=%s skbaddr=%p tstamp=0x%llx",
		  __get_str(name), __entry->skbaddr, __entry->tstamp)
);

TRACE_EVENT(sja1105_rxtstamp_end,

	TP_PROTO(const struct sk_buff *skb, u64 tstamp),

	TP_ARGS(skb, tstamp),

	TP_STRUCT__entry(
		__string(	name,			skb->dev->name	)
		__field(	const void *,		skbaddr		)
		__field(	u64,			tstamp		)
	),

	TP_fast_assign(
		__assign_str(name, skb->dev->name);
		__entry->skbaddr = skb;
		__entry->tstamp = tstamp;
	),

	TP_printk("dev=%s skbaddr=%p tstamp=0x%llx",
		  __get_str(name), __entry->skbaddr, __entry->tstamp)
);

TRACE_EVENT(sja1105_mgmt_route,

	TP_PROTO(const struct sk_buff *skb, int slot),

	TP_ARGS(skb, slot),

	TP_STRUCT__entry(
		__string(	name,			skb->dev->name	)
		__field(	const void *,		skbaddr		)
		__field(	int,			slot		)
	),

	TP_fast_assign(
		__assign_str(name, skb->dev->name);
		__entry->skbaddr = skb;
		__entry->slot = slot;
	),

	TP_printk("dev=%s skbaddr=%p slot=%d",
		  __get_str(name), __entry->skbaddr, __entry->slot)
);

TRACE_EVENT(sja1105_poll_mgmt_route_start,

	TP_PROTO(int slot),

	TP_ARGS(slot),

	TP_STRUCT__entry(
		__field(	int,			slot		)
	),

	TP_fast_assign(
		__entry->slot = slot;
	),

	TP_printk("slot=%d", __entry->slot)
);

TRACE_EVENT(sja1105_poll_mgmt_route_end,

	TP_PROTO(int slot),

	TP_ARGS(slot),

	TP_STRUCT__entry(
		__field(	int,			slot		)
	),

	TP_fast_assign(
		__entry->slot = slot;
	),

	TP_printk("slot=%d", __entry->slot)
);

#endif /* _NET_DSA_SJA1105_TRACE_H */

#include <trace/define_trace.h>
