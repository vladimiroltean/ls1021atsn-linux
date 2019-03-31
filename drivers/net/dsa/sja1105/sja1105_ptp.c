// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/* Copyright (c) 2018, NXP Semiconductors
 * Copyright (c) 2018-2019, Vladimir Oltean <olteanv@gmail.com>
 */

#include <linux/version.h>
#include <linux/time64.h>
#include "sja1105.h"

/* At full swing, the PTPCLKVAL can either speed up to 2x PTPTSCLK (when
 * PTPCLKRATE = 0xffffffff), or slow down to 1/2x PTPTSCLK (when PTPCLKRATE =
 * 0x0). PTPCLKRATE is centered around 0x80000000.  This means that the
 * hardware supports one full billion parts per billion frequency adjustments
 * in each direction, i.e. recover 1 whole second of offset (or NSEC_PER_SEC
 * as the ppb unit expresses it) during 1 second.
 *
 * The schedule engine and the PTP clock are driven by the same oscillator, and
 * they run in parallel. But whilst the PTP clock can keep an absolute
 * time-of-day, the schedule engine is only running in 'ticks' (25 ticks make
 * up a delta, which is 200ns), and wrapping around at the end of each cycle.
 * The schedule engine is started when the PTP clock reaches the PTPSCHTM time
 * (in PTP domain).  Because the PTP clock can be rate-corrected (accelerated
 * or slowed down) by a software servo, and the schedule engine clock runs in
 * parallel to the PTP clock, there is logic internal to the switch that
 * periodically keeps the schedule engine from drifting away. The frequency
 * with which this internal syntonization happens is the PTP clock correction
 * period (PTPCLKCORP). It is a value also in the PTP clock domain, and is also
 * rate-corrected.  To be precise, during a correction period, there is logic
 * to determine by how many scheduler clock ticks has the PTP clock drifted. At
 * the end of each correction period/beginning of new one, the length of a
 * delta is shrunk or expanded with an integer number of ticks, compared with
 * the typical 25.  So a delta lasts for 200ns (or 25 ticks) only on average.
 * Sometimes it is longer, sometimes it is shorter. The internal syntonization
 * logic can adjust for at most 5 ticks each 20 ticks.  The first implication
 * is that you should choose your schedule correction period to be an integer
 * multiple of the schedule length. Preferably one. Hence the choice in the
 * patched linuxptp.  This way, the updates are always synchronous to the
 * transmission cycle, and therefore predictable.  The second implication is
 * that at the beginning of a correction period, the first few deltas will be
 * modulated in time, until the schedule engine is properly phase-aligned with
 * the PTP clock. For this reason, you should place your best-effort traffic at
 * the beginning of a cycle, and your time-triggered traffic afterwards.  The
 * third implication is that once the schedule engine is started, it can only
 * adjust for so much drift within a correction period. In the servo you can
 * only change the PTPCLKRATE, but not step the clock (PTPCLKADD). If you want
 * to do the latter, you need to stop and restart the schedule engine.
 */
#define SJA1105_MAX_ADJ_PPB		32000000
#define SJA1105_SIZE_PTP_CMD		4

struct sja1105_ptp_cmd {
	u64 ptpstrtsch;   /* start schedule */
	u64 ptpstopsch;   /* stop schedule */
	u64 startptpcp;   /* start pin toggle  */
	u64 stopptpcp;    /* stop pin toggle */
	u64 resptp;       /* reset */
	u64 corrclk4ts;   /* if (1) timestamps are based on ptpclk,
			   * if (0) timestamps are based on ptptsclk
			   */
	u64 ptpclkadd;    /* enum sja1105_ptp_clk_mode */
};

int sja1105_get_ts_info(struct dsa_switch *ds, int port,
			struct ethtool_ts_info *info)
{
	struct sja1105_private *priv = ds->priv;

	/* Called during cleanup */
	if (!priv->clock)
		return -ENODEV;

	info->so_timestamping = SOF_TIMESTAMPING_TX_HARDWARE |
				SOF_TIMESTAMPING_RX_HARDWARE |
				SOF_TIMESTAMPING_RAW_HARDWARE;
	info->tx_types = (1 << HWTSTAMP_TX_OFF) |
			 (1 << HWTSTAMP_TX_ON);
	info->rx_filters = (1 << HWTSTAMP_FILTER_NONE) |
			   (1 << HWTSTAMP_FILTER_SOME);
	info->phc_index = ptp_clock_index(priv->clock);
	return 0;
}

static void
sja1105et_ptp_cmd_pack(void *buf, const struct sja1105_ptp_cmd *cmd)
{
	const int size = SJA1105_SIZE_PTP_CMD;
	/* No need to keep this as part of the structure */
	u64 valid = 1;

	sja1105_pack(buf, &valid,           31, 31, size);
	sja1105_pack(buf, &cmd->ptpstrtsch, 30, 30, size);
	sja1105_pack(buf, &cmd->ptpstopsch, 29, 29, size);
	sja1105_pack(buf, &cmd->startptpcp, 28, 28, size);
	sja1105_pack(buf, &cmd->stopptpcp,  27, 27, size);
	sja1105_pack(buf, &cmd->resptp,      2,  2, size);
	sja1105_pack(buf, &cmd->corrclk4ts,  1,  1, size);
	sja1105_pack(buf, &cmd->ptpclkadd,   0,  0, size);
}

static void
sja1105pqrs_ptp_cmd_pack(void *buf, const struct sja1105_ptp_cmd *cmd)
{
	const int size = SJA1105_SIZE_PTP_CMD;
	/* No need to keep this as part of the structure */
	u64 valid = 1;

	sja1105_pack(buf, &valid,           31, 31, size);
	sja1105_pack(buf, &cmd->ptpstrtsch, 30, 30, size);
	sja1105_pack(buf, &cmd->ptpstopsch, 29, 29, size);
	sja1105_pack(buf, &cmd->startptpcp, 28, 28, size);
	sja1105_pack(buf, &cmd->stopptpcp,  27, 27, size);
	sja1105_pack(buf, &cmd->resptp,      3,  3, size);
	sja1105_pack(buf, &cmd->corrclk4ts,  2,  2, size);
	sja1105_pack(buf, &cmd->ptpclkadd,   0,  0, size);
}

/* Wrapper around sja1105_spi_send_packed_buf() */
int sja1105et_ptp_cmd(const void *ctx, const void *data)
{
	const struct sja1105_ptp_cmd *cmd = data;
	const struct sja1105_private *priv = ctx;
	const struct sja1105_regs *regs = priv->info->regs;
	u8 packed_buf[SJA1105_SIZE_PTP_CMD] = {0};

	sja1105et_ptp_cmd_pack(packed_buf, cmd);

	return sja1105_spi_send_packed_buf(priv, SPI_WRITE, regs->ptp_control,
					   packed_buf, SJA1105_SIZE_PTP_CMD);
}

int sja1105pqrs_ptp_cmd(const void *ctx, const void *data)
{
	const struct sja1105_ptp_cmd *cmd = data;
	const struct sja1105_private *priv = ctx;
	const struct sja1105_regs *regs = priv->info->regs;
	u8 packed_buf[SJA1105_SIZE_PTP_CMD] = {0};

	sja1105pqrs_ptp_cmd_pack(packed_buf, cmd);

	return sja1105_spi_send_packed_buf(priv, SPI_WRITE, regs->ptp_control,
					   packed_buf, SJA1105_SIZE_PTP_CMD);
}

static void
sja1105_timespec_to_ptp_time(const struct timespec64 *ts, u64 *ptp_time)
{
#if 0
	/* We have to fit (ts->tv_sec * NSEC_PER_SEC + ts->tv_nsec) / 8
	 * into ptp_time. But although the final result may fit into 64 bits,
	 * the intermediary result prior to the division by 8 may not.
	 */

	*ptp_time = (u64)ts->tv_sec * (NSEC_PER_SEC / 8);
	*ptp_time += (ts->tv_nsec >> 3);
#else
	*ptp_time = ((u64)ts->tv_sec * NSEC_PER_SEC + ts->tv_nsec) / 8;
#endif
}

static void
sja1105_ptp_time_to_timespec(struct timespec64 *ts, u64 ptp_time)
{
	u32 nsec;

#if 0
	/* Since the 64-bit hardware time has a resolution of 8 ns,
	 * we can't simply multiply by 8 as it could overflow.
	 * So let x be (ptp_time * 8).
	 * The number of seconds in x is
	 *    (x / NSEC_PER_SEC), therefore
	 *    (ptp_time * 8 / 1,000,000,000), or
	 *    (ptp_time / 125000000), or
	 *    (ptp_time / (5^9 * 2^6)).
	 */
	ts->tv_sec = div_u64_rem(ptp_time >> 6, 1953125, &nsec);
	/* The remainder of the simplified division is a value in the range of
	 * [0, 5^9]. To get a nanosecond value it has to be upscaled to the
	 * [0, NSEC_PER_SEC] interval (multiplied by 512).
	 * Also since we performed the division by 5^9 and by 2^6 separately,
	 * we lost 6 bits of precision in the nanoseconds. So patch those
	 * back from the original ptp_time. The other 3 bits of precision
	 * are lost due to hardware resolution.
	 */
	ts->tv_nsec = (nsec << 9) | ((ptp_time & GENMASK_ULL(5, 0)) << 3);
#else
	ts->tv_sec = div_u64_rem(ptp_time * 8, NSEC_PER_SEC, &nsec);
	ts->tv_nsec = nsec;
#endif
}

static int sja1105_ptp_reset(struct sja1105_private *priv)
{
	struct dsa_switch *ds = priv->ds;
	struct sja1105_ptp_cmd cmd = {0};

	cmd.resptp = 1;
	dev_dbg(ds->dev, "Resetting PTP clock\n");
	return priv->info->ptp_cmd(priv, &cmd);
}

static inline int sja1105_ptp_mode_set(struct sja1105_private *priv,
				       enum sja1105_ptp_clk_mode mode)
{
	struct sja1105_ptp_cmd cmd = {0};
	int rc;

	if (priv->ptp_mode == mode)
		return 0;

	cmd.ptpclkadd = mode;
	rc = priv->info->ptp_cmd(priv, &cmd);
	if (rc < 0)
		return rc;

	priv->ptp_mode = mode;
	return 0;
}

static inline int sja1105_ptp_clk_write(struct sja1105_private *priv,
					const struct timespec64 *ts)
{
	const struct sja1105_regs *regs = priv->info->regs;
	u64 ptpclkval;

	sja1105_timespec_to_ptp_time(ts, &ptpclkval);
	return sja1105_spi_send_int(priv, SPI_WRITE, regs->ptpclk,
				    &ptpclkval, 8);
}

static int sja1105_ptp_gettime(struct ptp_clock_info *ptp,
			       struct timespec64 *ts)
{
	struct sja1105_private *priv = container_of(ptp,
						    struct sja1105_private,
						    ptp_caps);
	const struct sja1105_regs *regs = priv->info->regs;
	struct dsa_switch *ds = priv->ds;
	u64 ptpclkval;
	int rc;

	rc = sja1105_spi_send_int(priv, SPI_READ, regs->ptpclk,
				  &ptpclkval, 8);
	if (rc < 0) {
		dev_err(ds->dev, "failed to read ptpclkval\n");
		return rc;
	}
	sja1105_ptp_time_to_timespec(ts, ptpclkval);

	return 0;
}

/* Write to PTPCLKVAL while PTPCLKADD is 0 */
static int sja1105_ptp_settime(struct ptp_clock_info *ptp,
			       const struct timespec64 *ts)
{
	struct sja1105_private *priv = container_of(ptp,
						    struct sja1105_private,
						    ptp_caps);
	struct dsa_switch *ds = priv->ds;
	int rc;

	rc = sja1105_ptp_mode_set(priv, PTP_SET_MODE);
	if (rc < 0) {
		dev_err(ds->dev, "Failed to put PTPCLK in set mode\n");
		return rc;
	}
	return sja1105_ptp_clk_write(priv, ts);
}

/* Write to PTPCLKRATE.
 *
 * PTPCLKVAL   PTPCLKRATE
 * --------- = ----------
 * PTPTSCLK    0x80000000
 *
 * We replace the ratio with its actual "parts per billion" interpretation.
 *
 *          ppb        PTPCLKRATE
 * 1 + ------------- = ----------
 *     1,000,000,000   0x80000000
 *
 * We know 1 scaled_ppm = 65.536 ppb.
 * We also know that ppb is clamped between [-32,768,000, 32,768,000], and
 * therefore scaled_ppm between [-2,147,483,648, 2,147,483,647], but that
 * doesn't change the arithmetic.
 *
 *      scaled_ppm      PTPCLKRATE
 * 1 + -------------- = ----------
 *     65,536,000,000   0x80000000
 *
 *                           0x80000000 * scaled_ppm
 * PTPCLKRATE = 0x80000000 + -----------------------
 *                                65,536,000,000
 *
 *                           32,768 * scaled_ppm
 * PTPCLKRATE = 0x80000000 + -------------------
 *                                   10^6
 */
static int sja1105_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct sja1105_private *priv = container_of(ptp,
						    struct sja1105_private,
						    ptp_caps);
	const struct sja1105_regs *regs = priv->info->regs;
	s64 ptpclkrate;
	s64 rate;

	/*            This range is actually +/- SJA1105_MAX_ADJ_PPB
	 *            divided by 1000 (ppb -> ppm) and with a 16-bit
	 *            "fractional" part (actually fixed point).
	 *                                    |
	 *                                    v
	 * Convert scaled_ppm from the +/- ((10^6) << 16) range
	 * into the +/- (1 << 31) range (which the hw supports).
	 *
	 *   ptpclkrate = scaled_ppm * 2^31 / (10^6 * 2^16)
	 *   simplifies to
	 *   ptpclkrate = scaled_ppm * 2^9 / 5^6
	 */
	rate = div_s64((s64)scaled_ppm << 9, 15625);
	/* Take a +/- value and re-center it around 2^31. */
	ptpclkrate = rate + 0x80000000ll;

	dev_err(priv->ds->dev, "%s: scaled_ppm %ld ptpclkrate %llx\n", __func__, scaled_ppm, ptpclkrate);
	return sja1105_spi_send_int(priv, SPI_WRITE, regs->ptpclkrate,
				    &ptpclkrate, 4);
}

/* Write to PTPCLKVAL while PTPCLKADD is 1 */
static int sja1105_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
#if 0
	struct sja1105_private *priv = container_of(ptp,
						    struct sja1105_private,
						    ptp_caps);
	struct timespec64 ts = ns_to_timespec64(delta);
	struct dsa_switch *ds = priv->ds;
	int rc;

	rc = sja1105_ptp_mode_set(priv, PTP_ADD_MODE);
	if (rc < 0) {
		dev_err(ds->dev, "Failed to put PTPCLK in add mode\n");
		return rc;
	}
	return sja1105_ptp_clk_write(priv, &ts);
#else
	struct timespec64 ts;

	sja1105_ptp_gettime(ptp, &ts);
	set_normalized_timespec64(&ts, ts.tv_sec, (s64)ts.tv_nsec + delta);
	return sja1105_ptp_settime(ptp, &ts);
#endif
}

static const struct ptp_clock_info sja1105_ptp_caps = {
	.owner		= THIS_MODULE,
	.name		= "SJA1105 PHC",
	.adjfine	= sja1105_ptp_adjfine,
	.adjtime	= sja1105_ptp_adjtime,
	.gettime64	= sja1105_ptp_gettime,
	.settime64	= sja1105_ptp_settime,
	.max_adj	= SJA1105_MAX_ADJ_PPB,
};

int sja1105_ptp_clock_register(struct sja1105_private *priv)
{
	struct dsa_switch *ds = priv->ds;
	struct timespec64 now;
	int rc;

	priv->ptp_caps = sja1105_ptp_caps;

	priv->clock = ptp_clock_register(&priv->ptp_caps, ds->dev);
	if (IS_ERR_OR_NULL(priv->clock))
		return PTR_ERR(priv->clock);

	rc = sja1105_ptp_reset(priv);
	if (rc < 0)
		return -EIO;
	ktime_get_real_ts64(&now);
	sja1105_ptp_settime(&priv->ptp_caps, &now);

	return 0;
}

void sja1105_ptp_clock_unregister(struct sja1105_private *priv)
{
	if (IS_ERR_OR_NULL(priv->clock))
		return;

	ptp_clock_unregister(priv->clock);
	priv->clock = NULL;
}
