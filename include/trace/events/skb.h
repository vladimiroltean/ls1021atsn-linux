/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM skb

#if !defined(_TRACE_SKB_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SKB_H

#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/tracepoint.h>
#include <linux/ptp_classify.h>

/*
 * Tracepoint for free an sk_buff:
 */
TRACE_EVENT(skb_queue,

	TP_PROTO(struct sk_buff_head *queue, const char *func, int line),

	TP_ARGS(queue, func, line),

	TP_STRUCT__entry(
		__field(	void *,		queue		)
		__field(	void *,		next		)
		__field(	void *,		prev		)
		__field(	int,		qlen		)
		__string(	func,		func		)
		__field(	int,		line		)
	),

	TP_fast_assign(
		__entry->queue = queue;
		__entry->next = queue->next;
		__entry->prev = queue->prev;
		__entry->qlen = queue->qlen;
		__assign_str(func, func);
		__entry->line = line;
	),

	TP_printk("queue=%p next=%p prev=%p qlen=%u func=%s line=%d",
		__entry->queue, __entry->next, __entry->prev,
		__entry->qlen, __get_str(func), __entry->line)
);

/*
 * Tracepoint for free an sk_buff:
 */
TRACE_EVENT(dequeue_skb,

	TP_PROTO(struct sk_buff *skb, struct sk_buff *last, const char *func, int line),

	TP_ARGS(skb, last, func, line),

	TP_STRUCT__entry(
		__field(	void *,		skbaddr		)
		__field(	void *,		last		)
		__string(	func,		func		)
		__field(	int,		line		)
	),

	TP_fast_assign(
		__assign_str(func, func);
		__entry->line = line;
		__entry->skbaddr = skb;
		__entry->last = last;
	),

	TP_printk("skbaddr=%p last=%p func=%s line=%d",
		__entry->skbaddr, __entry->last,
		__get_str(func), __entry->line)
);

/*
 * Tracepoint for free an sk_buff:
 */
TRACE_EVENT(ptp_skb,

	TP_PROTO(struct sk_buff *skb, const char *func),

	TP_ARGS(skb, func),

	TP_STRUCT__entry(
		__field(	void *,		skbaddr		)
		__string(	name,		skb->dev->name	)
		__string(	func,		func		)
		__field(	u16,		seqid		)
		__field(	u8,		msgtype		)
	),

	TP_fast_assign(
		__assign_str(name, skb->dev->name);
		__assign_str(func, func);
		__entry->skbaddr = skb;
		__entry->seqid = ntohs(*(__be16 *)(skb_mac_header(skb) + ETH_HLEN + OFF_PTP_SEQUENCE_ID));
		__entry->msgtype = skb_mac_header(skb)[ETH_HLEN] & 0xf;
	),

	TP_printk("skbaddr=%p dev=%s msgtype=%u seqid=%u func=%s",
		__entry->skbaddr, __get_str(name), __entry->msgtype,
		__entry->seqid, __get_str(func))
);

/*
 * Tracepoint for free an sk_buff:
 */
TRACE_EVENT(kfree_skb,

	TP_PROTO(struct sk_buff *skb, void *location),

	TP_ARGS(skb, location),

	TP_STRUCT__entry(
		__field(	void *,		skbaddr		)
		__field(	void *,		location	)
		__field(	unsigned short,	protocol	)
	),

	TP_fast_assign(
		__entry->skbaddr = skb;
		__entry->location = location;
		__entry->protocol = ntohs(skb->protocol);
	),

	TP_printk("skbaddr=%p protocol=%u location=%p",
		__entry->skbaddr, __entry->protocol, __entry->location)
);

TRACE_EVENT(consume_skb,

	TP_PROTO(struct sk_buff *skb),

	TP_ARGS(skb),

	TP_STRUCT__entry(
		__field(	void *,	skbaddr	)
	),

	TP_fast_assign(
		__entry->skbaddr = skb;
	),

	TP_printk("skbaddr=%p", __entry->skbaddr)
);

TRACE_EVENT(skb_copy_datagram_iovec,

	TP_PROTO(const struct sk_buff *skb, int len),

	TP_ARGS(skb, len),

	TP_STRUCT__entry(
		__field(	const void *,		skbaddr		)
		__field(	int,			len		)
	),

	TP_fast_assign(
		__entry->skbaddr = skb;
		__entry->len = len;
	),

	TP_printk("skbaddr=%p len=%d", __entry->skbaddr, __entry->len)
);

#endif /* _TRACE_SKB_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
