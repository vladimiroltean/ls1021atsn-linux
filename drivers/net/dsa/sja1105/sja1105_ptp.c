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
 * hardware supports one full billion parts per billion frequency adjustments,
 * i.e. recover 1 whole second of offset (or NSEC_PER_SEC as the ppb unit
 * expresses it) during 1 second.
 */
#define SJA1105_MAX_ADJ_PPB		NSEC_PER_SEC

struct sja1105_ptp_cmd {
	u64 ptpstrtsch;   /* start schedule */
	u64 ptpstopsch;   /* stop schedule */
	u64 startptpcp;   /* start pin toggle  */
	u64 stopptpcp;    /* stop pin toggle */
	u64 cassync;      /* if cascaded master, trigger a toggle of the
			   * PTP_CLK pin, and store the timestamp of its
			   * 1588 clock (ptpclk or ptptsclk, depending on
			   * corrclk4ts), in ptpsyncts.
			   * only for P/Q/R/S series
			   */
	u64 resptp;       /* reset */
	u64 corrclk4ts;   /* if (1) timestamps are based on ptpclk,
			   * if (0) timestamps are based on ptptsclk
			   */
	u64 ptpclksub;    /* only for P/Q/R/S series */
	u64 ptpclkadd;    /* enum sja1105_ptp_clk_mode */
};

static void
sja1105et_ptp_cmd_pack(void *buf, const struct sja1105_ptp_cmd *cmd)
{
	const int size = 4;
	/* No need to keep this as part of the structure */
	u64 valid = 1;

	sja1105_pack(buf, &valid,           31, 31, size);
	sja1105_pack(buf, &cmd->ptpstrtsch, 30, 30, size);
	sja1105_pack(buf, &cmd->ptpstopsch, 29, 29, size);
	sja1105_pack(buf, &cmd->startptpcp, 28, 28, size);
	sja1105_pack(buf, &cmd->stopptpcp,  27, 27, size);
	sja1105_pack(buf, &cmd->resptp,      3,  3, size);
	sja1105_pack(buf, &cmd->corrclk4ts,  2,  2, size);
	sja1105_pack(buf, &cmd->ptpclksub,   1,  1, size);
	sja1105_pack(buf, &cmd->ptpclkadd,   0,  0, size);
}

static void
sja1105pqrs_ptp_cmd_pack(void *buf, const struct sja1105_ptp_cmd *cmd)
{
	const int size = 4;
	/* No need to keep this as part of the structure */
	u64 valid = 1;

	sja1105_pack(buf, &valid,           31, 31, size);
	sja1105_pack(buf, &cmd->ptpstrtsch, 30, 30, size);
	sja1105_pack(buf, &cmd->ptpstopsch, 29, 29, size);
	sja1105_pack(buf, &cmd->startptpcp, 28, 28, size);
	sja1105_pack(buf, &cmd->stopptpcp,  27, 27, size);
	sja1105_pack(buf, &cmd->cassync,    25, 25, size);
	sja1105_pack(buf, &cmd->resptp,      2,  2, size);
	sja1105_pack(buf, &cmd->corrclk4ts,  1,  1, size);
	sja1105_pack(buf, &cmd->ptpclkadd,   0,  0, size);
}

/* Wrapper around sja1105_spi_send_packed_buf() */
int sja1105et_ptp_cmd(const void *ctx, const void *data)
{
	const struct sja1105_ptp_cmd *cmd = data;
	const struct sja1105_private *priv = ctx;
	const struct sja1105_regs *regs = priv->info->regs;
	u8 packed_buf[4] = { 0 };

	sja1105et_ptp_cmd_pack(packed_buf, cmd);

	return sja1105_spi_send_packed_buf(priv, SPI_WRITE,
					   regs->ptp_control,
					   packed_buf, 4);
}

int sja1105pqrs_ptp_cmd(const void *ctx, const void *data)
{
	const struct sja1105_ptp_cmd *cmd = data;
	const struct sja1105_private *priv = ctx;
	const struct sja1105_regs *regs = priv->info->regs;
	u8 packed_buf[4] = { 0 };

	sja1105pqrs_ptp_cmd_pack(packed_buf, cmd);

	return sja1105_spi_send_packed_buf(priv, SPI_WRITE,
					   regs->ptp_control,
					   packed_buf, 4);
}

static void
sja1105_timespec_to_ptp_time(const struct timespec64 *ts, u64 *ptp_time)
{
	*ptp_time = (ts->tv_sec * NSEC_PER_SEC + ts->tv_nsec) / 8;
}

#define u64_to_timespec64(ts, val) \
	{ \
		(ts)->tv_sec = div_u64((val), NSEC_PER_SEC); \
		(ts)->tv_nsec = (val) - (ts)->tv_sec * NSEC_PER_SEC; \
	};

static void
sja1105_ptp_time_to_timespec(struct timespec64 *ts, u64 ptp_time)
{
	/* Check whether we can actually multiply by 8ns
	 * (the hw resolution) without overflow
	 */
	if (ptp_time >= 0x1FFFFFFFFFFFFFFFull)
		pr_err("Integer overflow during timespec conversion!\n");
	u64_to_timespec64(ts, ptp_time * 8);
}

/* Read PTPTSCLK */
int sja1105_ptp_ts_clk_get(struct sja1105_private *priv,
			   struct timespec64 *ts)
{
	const struct sja1105_regs *regs = priv->info->regs;
	struct dsa_switch *ds = priv->ds;
	u64 ptptsclk;
	int rc;

	rc = sja1105_spi_send_int(priv, SPI_READ, regs->ptptsclk,
				  &ptptsclk, 8);
	if (rc < 0) {
		dev_err(ds->dev, "Failed to read ptptsclk\n");
		return rc;
	}
	sja1105_ptp_time_to_timespec(ts, ptptsclk);

	return 0;
}

static int sja1105_ptp_reset(struct sja1105_private *priv)
{
	struct dsa_switch *ds = priv->ds;
	struct sja1105_ptp_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.resptp = 1;
	dev_dbg(ds->dev, "Resetting PTP clock\n");
	return priv->info->ptp_cmd(priv, &cmd);
}

static inline int sja1105_ptp_mode_set(struct sja1105_private *priv,
				       enum sja1105_ptp_clk_mode mode)
{
	struct sja1105_ptp_cmd cmd = { 0 };
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
	struct sja1105_private *priv = container_of(ptp, struct
				sja1105_private, ptp_caps);
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
	struct sja1105_private *priv = container_of(ptp, struct
				sja1105_private, ptp_caps);
	struct dsa_switch *ds = priv->ds;
	int rc;

	rc = sja1105_ptp_mode_set(priv, PTP_SET_MODE);
	if (rc < 0) {
		dev_err(ds->dev, "Failed to put PTPCLK in set mode\n");
		return rc;
	}
	return sja1105_ptp_clk_write(priv, ts);
}

/* Write to PTPCLKRATE */
static int sja1105_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct sja1105_private *priv = container_of(ptp, struct
				sja1105_private, ptp_caps);
	const struct sja1105_regs *regs = priv->info->regs;
	u64 ptpclkrate;
	int rc;

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
	ptpclkrate = (u64) scaled_ppm << 9;
	ptpclkrate = div_s64(ptpclkrate, 15625);
	/* Take a +/- value and re-center it around 2^31. */
	ptpclkrate += 0x80000000ull;

	rc = sja1105_spi_send_int(priv, SPI_WRITE, regs->ptpclkrate,
				  &ptpclkrate, 4);
	return rc;
}

/* Write to PTPCLKVAL while PTPCLKADD is 1 */
static int sja1105_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct sja1105_private *priv = container_of(ptp, struct
				sja1105_private, ptp_caps);
	struct timespec64 ts = ns_to_timespec64(delta);
	struct dsa_switch *ds = priv->ds;
	int rc;

	rc = sja1105_ptp_mode_set(priv, PTP_ADD_MODE);
	if (rc < 0) {
		dev_err(ds->dev, "Failed to put PTPCLK in add mode\n");
		return rc;
	}
	return sja1105_ptp_clk_write(priv, &ts);
}

static const struct ptp_clock_info sja1105_ptp_caps = {
	.owner     = THIS_MODULE,
	.name      = "SJA1105 PHC",
	.max_adj   = SJA1105_MAX_ADJ_PPB, /* has real physical significance */
	.adjfine   = sja1105_ptp_adjfine,
	.adjtime   = sja1105_ptp_adjtime,
	.gettime64 = sja1105_ptp_gettime,
	.settime64 = sja1105_ptp_settime,
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

