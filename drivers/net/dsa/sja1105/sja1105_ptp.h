/* SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2019, Vladimir Oltean <olteanv@gmail.com>
 */
#ifndef _SJA1105_PTP_H
#define _SJA1105_PTP_H

/* Timestamps are in units of 8 ns clock ticks (equivalent to
 * a fixed 125 MHz clock).
 */
#define SJA1105_TICK_NS			8

static inline s64 ns_to_sja1105_ticks(s64 ns)
{
	return ns / SJA1105_TICK_NS;
}

static inline s64 sja1105_ticks_to_ns(s64 ticks)
{
	return ticks * SJA1105_TICK_NS;
}

struct sja1105_private;

#if IS_ENABLED(CONFIG_NET_DSA_SJA1105_PTP)

enum sja1105_ptp_clk_mode {
	PTP_ADD_MODE = 1,
	PTP_SET_MODE = 0,
};

struct sja1105_ptp_cmd {
	u64 resptp;		/* reset */
	u64 corrclk4ts;		/* use the corrected clock for timestamps */
	u64 ptpclkadd;		/* enum sja1105_ptp_clk_mode */
};

struct sja1105_ptp_data {
	struct sja1105_ptp_cmd cmd;
	struct ptp_clock_info caps;
	struct ptp_clock *clock;
	/* Serializes all operations on the PTP hardware clock */
	struct mutex lock;
};

int sja1105_ptp_clock_register(struct sja1105_private *priv);

void sja1105_ptp_clock_unregister(struct sja1105_private *priv);

int sja1105_ptpegr_ts_poll(struct sja1105_private *priv, int port, u64 *ts);

void sja1105et_ptp_cmd_packing(u8 *buf, struct sja1105_ptp_cmd *cmd,
			       enum packing_op op);

void sja1105pqrs_ptp_cmd_packing(u8 *buf, struct sja1105_ptp_cmd *cmd,
				 enum packing_op op);

int sja1105_get_ts_info(struct dsa_switch *ds, int port,
			struct ethtool_ts_info *ts);

u64 sja1105_tstamp_reconstruct(struct sja1105_private *priv, u64 now,
			       u64 ts_partial);

int sja1105_ptp_reset(struct sja1105_private *priv);

u64 sja1105_ptpclkval_read(struct sja1105_private *priv,
			   struct ptp_system_timestamp *sts);

u64 __sja1105_ptp_gettimex(struct sja1105_private *priv,
			   struct ptp_system_timestamp *sts);

int __sja1105_ptp_settime(struct sja1105_private *priv, u64 ns,
			  struct ptp_system_timestamp *ptp_sts);

int __sja1105_ptp_adjtime(struct sja1105_private *priv, s64 delta);

#else

struct sja1105_ptp_cmd;

/* Structures cannot be empty in C. Bah!
 * Keep the mutex as the only element, which is a bit more difficult to
 * refactor out of sja1105_main.c anyway.
 */
struct sja1105_ptp_data {
	struct mutex lock;
};

static inline int sja1105_ptp_clock_register(struct sja1105_private *priv)
{
	return 0;
}

static inline void sja1105_ptp_clock_unregister(struct sja1105_private *priv)
{
	return;
}

static inline int
sja1105_ptpegr_ts_poll(struct sja1105_private *priv, int port, u64 *ts)
{
	return 0;
}

static inline u64 sja1105_tstamp_reconstruct(struct sja1105_private *priv,
					     u64 now, u64 ts_partial)
{
	return 0;
}

static inline int sja1105_ptp_reset(struct sja1105_private *priv)
{
	return 0;
}

static inline u64 sja1105_ptpclkval_read(struct sja1105_private *priv,
					 struct ptp_system_timestamp *sts)
{
	return 0;
}

static inline u64 __sja1105_ptp_gettimex(struct sja1105_private *priv,
					 struct ptp_system_timestamp *sts)
{
	return 0;
}

static inline int __sja1105_ptp_settime(struct sja1105_private *priv, u64 ns,
					struct ptp_system_timestamp *ptp_sts)
{
	return 0;
}

static inline int __sja1105_ptp_adjtime(struct sja1105_private *priv, s64 delta)
{
	return 0;
}

#define sja1105et_ptp_cmd_packing NULL

#define sja1105pqrs_ptp_cmd_packing NULL

#define sja1105_get_ts_info NULL

#endif /* IS_ENABLED(CONFIG_NET_DSA_SJA1105_PTP) */

#endif /* _SJA1105_PTP_H */
