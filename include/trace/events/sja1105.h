/* SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2019, Vladimir Oltean <olteanv@gmail.com>
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM	sja1105

#if !defined(_NET_DSA_SJA1105_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _NET_DSA_SJA1105_TRACE_H

#include <linux/ptp_clock_kernel.h>
#include <linux/ptp_classify.h>
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
		__field(	u16,			seqid		)
		__field(	u8,			msgtype		)
	),

	TP_fast_assign(
		__assign_str(name, skb->dev->name);
		__entry->skbaddr = DSA_SKB_CB(skb)->clone;
		__entry->tsreg = tsreg;
		__entry->seqid = ntohs(*(__be16 *)(skb_ptp_header(skb) + OFF_PTP_SEQUENCE_ID));
		__entry->msgtype = skb_ptp_header(skb)[0] & 0xf;
	),

	TP_printk("dev=%s skbaddr=%p tsreg=%d msgtype=%u seqid=%u",
		  __get_str(name), __entry->skbaddr, __entry->tsreg,
		  __entry->msgtype, __entry->seqid)
);

TRACE_EVENT(sja1105_rxtstamp_start,

	TP_PROTO(const struct sk_buff *skb),

	TP_ARGS(skb),

	TP_STRUCT__entry(
		__string(	name,			skb->dev->name	)
		__field(	const void *,		skbaddr		)
		__field(	u16,			seqid		)
		__field(	u8,			msgtype		)
	),

	TP_fast_assign(
		__assign_str(name, skb->dev->name);
		__entry->skbaddr = skb;
		__entry->seqid = ntohs(*(__be16 *)(skb_ptp_header(skb) + OFF_PTP_SEQUENCE_ID));
		__entry->msgtype = skb_ptp_header(skb)[0] & 0xf;
	),

	TP_printk("dev=%s skbaddr=%p msgtype=%u seqid=%u",
		  __get_str(name), __entry->skbaddr,
		  __entry->msgtype, __entry->seqid)
);

TRACE_EVENT(sja1105_txtstamp_end,

	TP_PROTO(const struct sk_buff *skb, u64 tstamp),

	TP_ARGS(skb, tstamp),

	TP_STRUCT__entry(
		__string(	name,			skb->dev->name	)
		__field(	const void *,		skbaddr		)
		__field(	u64,			tstamp		)
		__field(	u16,			seqid		)
		__field(	u8,			msgtype		)
	),

	TP_fast_assign(
		__assign_str(name, skb->dev->name);
		__entry->skbaddr = DSA_SKB_CB(skb)->clone;
		__entry->tstamp = tstamp;
		__entry->seqid = ntohs(*(__be16 *)(skb_ptp_header(skb) + OFF_PTP_SEQUENCE_ID));
		__entry->msgtype = skb_ptp_header(skb)[0] & 0xf;
	),

	TP_printk("dev=%s skbaddr=%p tstamp=0x%llx msgtype=%u seqid=%u",
		  __get_str(name), __entry->skbaddr, __entry->tstamp,
		  __entry->msgtype, __entry->seqid)
);

TRACE_EVENT(sja1105_rxtstamp_end,

	TP_PROTO(const struct sk_buff *skb, u64 tstamp),

	TP_ARGS(skb, tstamp),

	TP_STRUCT__entry(
		__string(	name,			skb->dev->name	)
		__field(	const void *,		skbaddr		)
		__field(	u64,			tstamp		)
		__field(	u16,			seqid		)
		__field(	u8,			msgtype		)
	),

	TP_fast_assign(
		__assign_str(name, skb->dev->name);
		__entry->skbaddr = skb;
		__entry->tstamp = tstamp;
		__entry->seqid = ntohs(*(__be16 *)(skb_ptp_header(skb) + OFF_PTP_SEQUENCE_ID));
		__entry->msgtype = skb_ptp_header(skb)[0] & 0xf;
	),

	TP_printk("dev=%s skbaddr=%p tstamp=0x%llx msgtype=%u seqid=%u",
		  __get_str(name), __entry->skbaddr, __entry->tstamp,
		  __entry->msgtype, __entry->seqid)
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

TRACE_EVENT(sja1105_poll_mgmt_route_end,

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

TRACE_EVENT(sja1105_ptpclkval_read,

	TP_PROTO(s64 ticks, struct ptp_system_timestamp *ptp_sts),

	TP_ARGS(ticks, ptp_sts),

	TP_STRUCT__entry(
		__field(	s64,			ticks		)
		__field(	s64,			pre_ts		)
		__field(	s64,			post_ts		)
	),

	TP_fast_assign(
		__entry->ticks = ticks;
		__entry->pre_ts = ptp_sts ? timespec64_to_ns(&ptp_sts->pre_ts) : 0;
		__entry->post_ts = ptp_sts ? timespec64_to_ns(&ptp_sts->post_ts) : 0;
	),

	TP_printk("ticks=0x%016llx pre_ts=%09lld post_ts=%09lld",
		  __entry->ticks, __entry->pre_ts, __entry->post_ts)
);

TRACE_EVENT(sja1105_ptpclkval_write,

	TP_PROTO(s64 ticks, int mode, struct ptp_system_timestamp *ptp_sts),

	TP_ARGS(ticks, mode, ptp_sts),

	TP_STRUCT__entry(
		__field(	s64,			ticks		)
		__field(	int,			mode		)
		__field(	s64,			pre_ts		)
		__field(	s64,			post_ts		)
	),

	TP_fast_assign(
		__entry->ticks = ticks;
		__entry->mode = mode;
		__entry->pre_ts = ptp_sts ? timespec64_to_ns(&ptp_sts->pre_ts) : 0;
		__entry->post_ts = ptp_sts ? timespec64_to_ns(&ptp_sts->post_ts) : 0;
	),

	TP_printk("ticks=0x%016llx mode=%d pre_ts=%09lld post_ts=%09lld",
		  __entry->ticks, __entry->mode,
		  __entry->pre_ts, __entry->post_ts)
);

TRACE_EVENT(sja1105_ptp_mode_set,

	TP_PROTO(int mode),

	TP_ARGS(mode),

	TP_STRUCT__entry(
		__field(	int,			mode		)
	),

	TP_fast_assign(
		__entry->mode = mode;
	),

	TP_printk("mode=%d", __entry->mode)
);

TRACE_EVENT(sja1105_ptpclkrate,

	TP_PROTO(u32 ptpclkrate),

	TP_ARGS(ptpclkrate),

	TP_STRUCT__entry(
		__field(	u32,			ptpclkrate	)
	),

	TP_fast_assign(
		__entry->ptpclkrate = ptpclkrate;
	),

	TP_printk("ptpclkrate=0x%08x", __entry->ptpclkrate)
);

TRACE_EVENT(sja1105_select_poll,

	TP_PROTO(struct file *file, __poll_t mask, short revents,
		 const char *func),

	TP_ARGS(file, mask, revents, func),

	TP_STRUCT__entry(
		__field(	void *,		file		)
		__field(	__poll_t,	mask		)
		__field(	short,		revents		)
		__string(	func,		func		)
	),

	TP_fast_assign(
		__entry->file = file;
		__entry->mask = mask;
		__entry->revents = revents;
		__assign_str(func, func);
	),

	TP_printk("file=%p mask=0x%x revents=0x%x func=%s",
		__entry->file, __entry->mask, __entry->revents, __get_str(func))
);

TRACE_EVENT(sja1105_poll_put_mask,

	TP_PROTO(struct pollfd *pfd, int fd, short revents),

	TP_ARGS(pfd, fd, revents),

	TP_STRUCT__entry(
		__field(	void *,		pfd		)
		__field(	int,		fd		)
		__field(	short,		revents		)
	),

	TP_fast_assign(
		__entry->pfd = pfd;
		__entry->fd = fd;
		__entry->revents = revents;
	),

	TP_printk("pfd=%p fd=%d revents=0x%x",
		  __entry->pfd, __entry->fd, __entry->revents)
);

TRACE_EVENT(sja1105_sock_poll,

	TP_PROTO(struct sock *sk, struct file *file, __poll_t mask,
		 bool readable, bool writable, bool exceptional,
		 const char *func),

	TP_ARGS(sk, file, mask, readable, writable, exceptional, func),

	TP_STRUCT__entry(
		__field(	void *,		sk		)
		__field(	void *,		file		)
		__field(	__poll_t,	mask		)
		__field(	bool,		readable	)
		__field(	bool,		writable	)
		__field(	bool,		exceptional	)
		__string(	func,		func		)
	),

	TP_fast_assign(
		__entry->sk = sk;
		__entry->file = file;
		__entry->mask = mask;
		__entry->readable = readable;
		__entry->writable = writable;
		__entry->exceptional = exceptional;
		__assign_str(func, func);
	),

	TP_printk("sk=%p file=%p mask=0x%x readable=%d writable=%d exceptional=%d func=%s",
		__entry->sk, __entry->file, __entry->mask,
		__entry->readable, __entry->writable, __entry->exceptional,
		__get_str(func))
);

TRACE_EVENT(sja1105_stack_ptp,

	TP_PROTO(struct sk_buff *skb, struct sock *sk, const char *func),

	TP_ARGS(skb, sk, func),

	TP_STRUCT__entry(
		__field(	void *,		skbaddr		)
		__string(	name,		skb->dev->name	)
		__field(	u16,		seqid		)
		__field(	u8,		msgtype		)
		__field(	void *,		sk		)
		__string(	func,		func		)
	),

	TP_fast_assign(
		__assign_str(name, skb->dev->name);
		__assign_str(func, func);
		__entry->skbaddr = skb;
		__entry->sk = sk;
		__entry->seqid = ntohs(*(__be16 *)(skb_mac_header(skb) + ETH_HLEN + OFF_PTP_SEQUENCE_ID));
		__entry->msgtype = skb_mac_header(skb)[ETH_HLEN] & 0xf;
	),

	TP_printk("skbaddr=%p dev=%s msgtype=%u seqid=%u sk=%p func=%s",
		__entry->skbaddr, __get_str(name), __entry->msgtype,
		__entry->seqid, __entry->sk, __get_str(func))
);

#endif /* _NET_DSA_SJA1105_TRACE_H */

#include <trace/define_trace.h>
