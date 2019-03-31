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
#define SJA1105_MAX_ADJ_PPB        NSEC_PER_SEC

enum sja1105_ptpegr_ts_source {
	TS_PTPTSCLK = 0,
	TS_PTPCLK = 1,
};

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
	u64 ptpclkadd;    /* enum sja1105_ptp_clk_add_mode */
};

static void sja1105_ptp_cmd_packing(void *buf, struct sja1105_ptp_cmd *cmd,
				    enum packing_op op, u64 device_id)
{
	const int size = 4;
	/* No need to keep this as part of the structure */
	u64 valid = 1;

	if (op == UNPACK)
		memset(cmd, 0, sizeof(*cmd));
	else
		memset(buf, 0, size);

	sja1105_packing(buf, &valid,           31, 31, size, op);
	sja1105_packing(buf, &cmd->ptpstrtsch, 30, 30, size, op);
	sja1105_packing(buf, &cmd->ptpstopsch, 29, 29, size, op);
	sja1105_packing(buf, &cmd->startptpcp, 28, 28, size, op);
	sja1105_packing(buf, &cmd->stopptpcp,  27, 27, size, op);
	if (IS_ET(device_id)) {
		sja1105_packing(buf, &cmd->resptp,      3,  3, size, op);
		sja1105_packing(buf, &cmd->corrclk4ts,  2,  2, size, op);
		sja1105_packing(buf, &cmd->ptpclksub,   1,  1, size, op);
		sja1105_packing(buf, &cmd->ptpclkadd,   0,  0, size, op);
	} else {
		sja1105_packing(buf, &cmd->cassync,    25, 25, size, op);
		sja1105_packing(buf, &cmd->resptp,      2,  2, size, op);
		sja1105_packing(buf, &cmd->corrclk4ts,  1,  1, size, op);
		sja1105_packing(buf, &cmd->ptpclkadd,   0,  0, size, op);
	}
}

/* Wrapper around sja1105_spi_send_packed_buf() */
static int sja1105_ptp_cmd_commit(struct sja1105_private *priv,
				  struct sja1105_ptp_cmd *cmd)
{
#define BUF_LEN 4
	u8 packed_buf[BUF_LEN];

	sja1105_ptp_cmd_packing(packed_buf, cmd, PACK, priv->device_id);
	return sja1105_spi_send_packed_buf(priv, SPI_WRITE,
					   priv->regs->ptp_control,
					   packed_buf, BUF_LEN);
#undef BUF_LEN
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

int sja1105_ptpegr_ts_poll(struct sja1105_private *priv,
			   enum sja1105_ptpegr_ts_source source,
			   int port, int ts_regid, struct timespec64 *ts)
{
#define SIZE_PTPEGR_TS 4
	const int ts_reg_index = 2 * port + ts_regid;
	u8  packed_buf[SIZE_PTPEGR_TS];
	u64 ptpegr_ts_reconstructed;
	u64 ptpegr_ts_partial;
	u64 full_current_ts;
	u64 ptpclk_addr;
	u64 update;
	int rc;

	if (source == TS_PTPCLK)
		/* Use the rate-corrected PTPCLK */
		ptpclk_addr = priv->regs->ptpclk;
	else if (source == TS_PTPTSCLK)
		/* Use the uncorrected PTPTSCLK */
		ptpclk_addr = priv->regs->ptptsclk;
	else
		return -EINVAL;

	rc = sja1105_spi_send_packed_buf(priv, SPI_READ,
					 priv->regs->ptpegr_ts + ts_reg_index,
					 packed_buf, SIZE_PTPEGR_TS);
	if (rc < 0)
		return rc;

	sja1105_unpack(packed_buf, &ptpegr_ts_partial, 31, 8, SIZE_PTPEGR_TS);
	sja1105_unpack(packed_buf, &update,             0, 0, SIZE_PTPEGR_TS);

	if (!update)
		/* No update. Keep trying, you'll make it someday. */
		return -EAGAIN;
	rc = sja1105_spi_send_int(priv, SPI_READ, ptpclk_addr,
				 &full_current_ts, 8);
	if (rc < 0)
		return rc;

	ptpegr_ts_reconstructed = (full_current_ts &
				  ~priv->regs->ptpegr_ts_mask) |
				   ptpegr_ts_partial;
	/* Check if wraparound occurred between moment when the partial
	 * ptpegr timestamp was generated, and the moment when that
	 * timestamp is being read out (now, ptpclkval/ptptsclk).
	 * If last 24 bits (32 for P/Q/R/S) of current ptpclkval/ptptsclk
	 * time are lower than the partial timestamp, then wraparound surely
	 * occurred, as ptpclkval is 64-bit.
	 * What is up to anyone's guess is how many times has the wraparound
	 * occurred. The code assumes (perhaps foolishly?) that if wraparound
	 * is present, it has only occurred once, and thus corrects for it.
	 */
	if ((full_current_ts & priv->regs->ptpegr_ts_mask) <= ptpegr_ts_partial)
		ptpegr_ts_reconstructed -= (priv->regs->ptpegr_ts_mask + 1ull);

	sja1105_ptp_time_to_timespec(ts, ptpegr_ts_reconstructed);

	return 0;
#undef SIZE_PTPEGR_TS
}

/* Read PTPTSCLK */
int sja1105_ptp_ts_clk_get(struct sja1105_private *priv,
			   struct timespec64 *ts)
{
	struct dsa_switch *ds = priv->ds;
	u64 ptptsclk;
	int rc;

	rc = sja1105_spi_send_int(priv, SPI_READ, priv->regs->ptptsclk, &ptptsclk, 8);
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
	return sja1105_ptp_cmd_commit(priv, &cmd);
}

static inline int sja1105_ptp_add_mode_set(struct sja1105_private *priv,
					   enum sja1105_ptp_clk_add_mode mode)
{
	struct sja1105_ptp_cmd cmd;
	int rc;

	if (priv->ptp_add_mode == mode)
		return 0;

	memset(&cmd, 0, sizeof(cmd));
	cmd.ptpclkadd = mode;
	rc = sja1105_ptp_cmd_commit(priv, &cmd);
	if (rc < 0)
		return rc;

	priv->ptp_add_mode = mode;
	return 0;
}

static inline int sja1105_ptp_clk_write(struct sja1105_private *priv,
					const struct timespec64 *ts)
{
	u64 ptpclkval;

	sja1105_timespec_to_ptp_time(ts, &ptpclkval);
	return sja1105_spi_send_int(priv, SPI_WRITE, priv->regs->ptpclk,
				   &ptpclkval, 8);
}

static int sja1105_ptp_gettime(struct ptp_clock_info *ptp,
			       struct timespec64 *ts)
{
	struct sja1105_private *priv = container_of(ptp, struct
				sja1105_private, ptp_caps);
	struct dsa_switch *ds = priv->ds;
	u64 ptpclkval;
	int rc;

	rc = sja1105_spi_send_int(priv, SPI_READ, priv->regs->ptpclk,
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

	rc = sja1105_ptp_add_mode_set(priv, PTP_SET_MODE);
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

	rc = sja1105_spi_send_int(priv, SPI_WRITE, priv->regs->ptpclkrate,
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

	rc = sja1105_ptp_add_mode_set(priv, PTP_ADD_MODE);
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

