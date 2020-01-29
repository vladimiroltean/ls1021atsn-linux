// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019, Vladimir Oltean <olteanv@gmail.com>
 */
#include "sja1105.h"

/* The adjfine API clamps ppb between [-32,768,000, 32,768,000], and
 * therefore scaled_ppm between [-2,147,483,648, 2,147,483,647].
 * Set the maximum supported ppb to a round value smaller than the maximum.
 *
 * Percentually speaking, this is a +/- 0.032x adjustment of the
 * free-running counter (0.968x to 1.032x).
 */
#define SJA1105_MAX_ADJ_PPB		32000000
#define SJA1105_SIZE_PTP_CMD		4

/* Timestamps are in units of 8 ns clock ticks (equivalent to a fixed
 * 125 MHz clock) so the scale factor (MULT / SHIFT) needs to be 8.
 * Furthermore, wisely pick SHIFT as 28 bits, which translates
 * MULT into 2^31 (0x80000000).  This is the same value around which
 * the hardware PTPCLKRATE is centered, so the same ppb conversion
 * arithmetic can be reused.
 */
#define SJA1105_CC_SHIFT		28
#define SJA1105_CC_MULT			(8 << SJA1105_CC_SHIFT)

/* Having 33 bits of cycle counter left until a 64-bit overflow during delta
 * conversion, we multiply this by the 8 ns counter resolution and arrive at
 * a comfortable 68.71 second refresh interval until the delta would cause
 * an integer overflow, in absence of any other readout.
 * Approximate to 1 minute.
 */
#define SJA1105_REFRESH_INTERVAL	(HZ * 60)

/*            This range is actually +/- SJA1105_MAX_ADJ_PPB
 *            divided by 1000 (ppb -> ppm) and with a 16-bit
 *            "fractional" part (actually fixed point).
 *                                    |
 *                                    v
 * Convert scaled_ppm from the +/- ((10^6) << 16) range
 * into the +/- (1 << 31) range.
 *
 * This forgoes a "ppb" numeric representation (up to NSEC_PER_SEC)
 * and defines the scaling factor between scaled_ppm and the actual
 * frequency adjustments (both cycle counter and hardware).
 *
 *   ptpclkrate = scaled_ppm * 2^31 / (10^6 * 2^16)
 *   simplifies to
 *   ptpclkrate = scaled_ppm * 2^9 / 5^6
 */
#define SJA1105_CC_MULT_NUM		(1 << 9)
#define SJA1105_CC_MULT_DEM		15625

#define ptp_caps_to_data(d) \
		container_of((d), struct sja1105_ptp_data, caps)
#define cc_to_ptp_data(d) \
		container_of((d), struct sja1105_ptp_data, tstamp_cc)
#define dw_to_ptp_data(d) \
		container_of((d), struct sja1105_ptp_data, refresh_work)
#define ptp_data_to_sja1105(d) \
		container_of((d), struct sja1105_private, ptp_data)

static int sja1105_init_avb_params(struct sja1105_private *priv,
				   bool on)
{
	struct sja1105_avb_params_entry *avb;
	struct sja1105_table *table;

	table = &priv->static_config.tables[BLK_IDX_AVB_PARAMS];

	/* Discard previous AVB Parameters Table */
	if (table->entry_count) {
		kfree(table->entries);
		table->entry_count = 0;
	}

	/* Configure the reception of meta frames only if requested */
	if (!on)
		return 0;

	table->entries = kcalloc(SJA1105_MAX_AVB_PARAMS_COUNT,
				 table->ops->unpacked_entry_size, GFP_KERNEL);
	if (!table->entries)
		return -ENOMEM;

	table->entry_count = SJA1105_MAX_AVB_PARAMS_COUNT;

	avb = table->entries;

	avb->destmeta = SJA1105_META_DMAC;
	avb->srcmeta  = SJA1105_META_SMAC;

	return 0;
}

/* Must be called only with priv->tagger_data.state bit
 * SJA1105_HWTS_RX_EN cleared
 */
static int sja1105_change_rxtstamping(struct sja1105_private *priv,
				      bool on)
{
	struct sja1105_general_params_entry *general_params;
	struct sja1105_table *table;
	int rc;

	table = &priv->static_config.tables[BLK_IDX_GENERAL_PARAMS];
	general_params = table->entries;
	general_params->send_meta1 = on;
	general_params->send_meta0 = on;

	rc = sja1105_init_avb_params(priv, on);
	if (rc < 0)
		return rc;

	/* Initialize the meta state machine to a known state */
	if (priv->tagger_data.stampable_skb) {
		kfree_skb(priv->tagger_data.stampable_skb);
		priv->tagger_data.stampable_skb = NULL;
	}

	return sja1105_static_config_reload(priv);
}

int sja1105_hwtstamp_set(struct dsa_switch *ds, int port, struct ifreq *ifr)
{
	struct sja1105_private *priv = ds->priv;
	struct hwtstamp_config config;
	bool rx_on;
	int rc;

	if (copy_from_user(&config, ifr->ifr_data, sizeof(config)))
		return -EFAULT;

	switch (config.tx_type) {
	case HWTSTAMP_TX_OFF:
		priv->ports[port].hwts_tx_en = false;
		break;
	case HWTSTAMP_TX_ON:
		priv->ports[port].hwts_tx_en = true;
		break;
	default:
		return -ERANGE;
	}

	switch (config.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		rx_on = false;
		break;
	default:
		rx_on = true;
		break;
	}

	if (rx_on != test_bit(SJA1105_HWTS_RX_EN, &priv->tagger_data.state)) {
		clear_bit(SJA1105_HWTS_RX_EN, &priv->tagger_data.state);

		rc = sja1105_change_rxtstamping(priv, rx_on);
		if (rc < 0) {
			dev_err(ds->dev,
				"Failed to change RX timestamping: %d\n", rc);
			return rc;
		}
		if (rx_on)
			set_bit(SJA1105_HWTS_RX_EN, &priv->tagger_data.state);
	}

	if (copy_to_user(ifr->ifr_data, &config, sizeof(config)))
		return -EFAULT;
	return 0;
}

int sja1105_hwtstamp_get(struct dsa_switch *ds, int port, struct ifreq *ifr)
{
	struct sja1105_private *priv = ds->priv;
	struct hwtstamp_config config;

	config.flags = 0;
	if (priv->ports[port].hwts_tx_en)
		config.tx_type = HWTSTAMP_TX_ON;
	else
		config.tx_type = HWTSTAMP_TX_OFF;
	if (test_bit(SJA1105_HWTS_RX_EN, &priv->tagger_data.state))
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_EVENT;
	else
		config.rx_filter = HWTSTAMP_FILTER_NONE;

	return copy_to_user(ifr->ifr_data, &config, sizeof(config)) ?
		-EFAULT : 0;
}

int sja1105_get_ts_info(struct dsa_switch *ds, int port,
			struct ethtool_ts_info *info)
{
	struct sja1105_private *priv = ds->priv;
	struct sja1105_ptp_data *ptp_data = &priv->ptp_data;

	/* Called during cleanup */
	if (!ptp_data->clock)
		return -ENODEV;

	info->so_timestamping = SOF_TIMESTAMPING_TX_HARDWARE |
				SOF_TIMESTAMPING_RX_HARDWARE |
				SOF_TIMESTAMPING_RAW_HARDWARE;
	info->tx_types = (1 << HWTSTAMP_TX_OFF) |
			 (1 << HWTSTAMP_TX_ON);
	info->rx_filters = (1 << HWTSTAMP_FILTER_NONE) |
			   (1 << HWTSTAMP_FILTER_PTP_V2_L2_EVENT);
	info->phc_index = ptp_clock_index(ptp_data->clock);
	return 0;
}

int sja1105et_ptp_cmd(const struct dsa_switch *ds,
		      const struct sja1105_ptp_cmd *cmd)
{
	const struct sja1105_private *priv = ds->priv;
	const struct sja1105_regs *regs = priv->info->regs;
	const int size = SJA1105_SIZE_PTP_CMD;
	u8 buf[SJA1105_SIZE_PTP_CMD] = {0};
	/* No need to keep this as part of the structure */
	u64 valid = 1;

	sja1105_pack(buf, &valid,           31, 31, size);
	sja1105_pack(buf, &cmd->resptp,      2,  2, size);

	return sja1105_xfer_buf(priv, SPI_WRITE, regs->ptp_control, buf,
				SJA1105_SIZE_PTP_CMD);
}

int sja1105pqrs_ptp_cmd(const struct dsa_switch *ds,
			const struct sja1105_ptp_cmd *cmd)
{
	const struct sja1105_private *priv = ds->priv;
	const struct sja1105_regs *regs = priv->info->regs;
	const int size = SJA1105_SIZE_PTP_CMD;
	u8 buf[SJA1105_SIZE_PTP_CMD] = {0};
	/* No need to keep this as part of the structure */
	u64 valid = 1;

	sja1105_pack(buf, &valid,           31, 31, size);
	sja1105_pack(buf, &cmd->resptp,      3,  3, size);

	return sja1105_xfer_buf(priv, SPI_WRITE, regs->ptp_control, buf,
				SJA1105_SIZE_PTP_CMD);
}

/* The switch returns partial timestamps (24 bits for SJA1105 E/T, which wrap
 * around in 0.135 seconds, and 32 bits for P/Q/R/S, wrapping around in 34.35
 * seconds).
 *
 * This receives the RX or TX MAC timestamps, provided by hardware as
 * the lower bits of the cycle counter, sampled at the time the timestamp was
 * collected.
 *
 * To reconstruct into a full 64-bit-wide timestamp, the cycle counter is
 * read and the high-order bits are filled in.
 *
 * Must be called within one wraparound period of the partial timestamp since
 * it was generated by the MAC.
 */
static u64 sja1105_tstamp_reconstruct(struct dsa_switch *ds, u64 now,
				      u64 ts_partial)
{
	struct sja1105_private *priv = ds->priv;
	u64 partial_tstamp_mask = CYCLECOUNTER_MASK(priv->info->ptp_ts_bits);
	u64 ts_reconstructed;

	ts_reconstructed = (now & ~partial_tstamp_mask) | ts_partial;

	/* Check lower bits of current cycle counter against the timestamp.
	 * If the current cycle counter is lower than the partial timestamp,
	 * then wraparound surely occurred and must be accounted for.
	 */
	if ((now & partial_tstamp_mask) <= ts_partial)
		ts_reconstructed -= (partial_tstamp_mask + 1);

	return ts_reconstructed;
}

/* Reads the SPI interface for an egress timestamp generated by the switch
 * for frames sent using management routes.
 *
 * SJA1105 E/T layout of the 4-byte SPI payload:
 *
 * 31    23    15    7     0
 * |     |     |     |     |
 * +-----+-----+-----+     ^
 *          ^              |
 *          |              |
 *  24-bit timestamp   Update bit
 *
 *
 * SJA1105 P/Q/R/S layout of the 8-byte SPI payload:
 *
 * 31    23    15    7     0     63    55    47    39    32
 * |     |     |     |     |     |     |     |     |     |
 *                         ^     +-----+-----+-----+-----+
 *                         |                 ^
 *                         |                 |
 *                    Update bit    32-bit timestamp
 *
 * Notice that the update bit is in the same place.
 * To have common code for E/T and P/Q/R/S for reading the timestamp,
 * we need to juggle with the offset and the bit indices.
 */
static int sja1105_ptpegr_ts_poll(struct dsa_switch *ds, int port, u64 *ts)
{
	struct sja1105_private *priv = ds->priv;
	const struct sja1105_regs *regs = priv->info->regs;
	int tstamp_bit_start, tstamp_bit_end;
	int timeout = 10;
	u8 packed_buf[8];
	u64 update;
	int rc;

	do {
		rc = sja1105_xfer_buf(priv, SPI_READ, regs->ptpegr_ts[port],
				      packed_buf, priv->info->ptpegr_ts_bytes);
		if (rc < 0)
			return rc;

		sja1105_unpack(packed_buf, &update, 0, 0,
			       priv->info->ptpegr_ts_bytes);
		if (update)
			break;

		usleep_range(10, 50);
	} while (--timeout);

	if (!timeout)
		return -ETIMEDOUT;

	/* Point the end bit to the second 32-bit word on P/Q/R/S,
	 * no-op on E/T.
	 */
	tstamp_bit_end = (priv->info->ptpegr_ts_bytes - 4) * 8;
	/* Shift the 24-bit timestamp on E/T to be collected from 31:8.
	 * No-op on P/Q/R/S.
	 */
	tstamp_bit_end += 32 - priv->info->ptp_ts_bits;
	tstamp_bit_start = tstamp_bit_end + priv->info->ptp_ts_bits - 1;

	*ts = 0;

	sja1105_unpack(packed_buf, ts, tstamp_bit_start, tstamp_bit_end,
		       priv->info->ptpegr_ts_bytes);

	return 0;
}

#define rxtstamp_to_tagger(d) \
	container_of((d), struct sja1105_tagger_data, rxtstamp_work)
#define tagger_to_sja1105(d) \
	container_of((d), struct sja1105_private, tagger_data)

static void sja1105_rxtstamp_work(struct work_struct *work)
{
	struct sja1105_tagger_data *tagger_data = rxtstamp_to_tagger(work);
	struct sja1105_private *priv = tagger_to_sja1105(tagger_data);
	struct sja1105_ptp_data *ptp_data = &priv->ptp_data;
	struct dsa_switch *ds = priv->ds;
	struct sk_buff *skb;

	mutex_lock(&ptp_data->lock);

	while ((skb = skb_dequeue(&tagger_data->skb_rxtstamp_queue)) != NULL) {
		struct skb_shared_hwtstamps *shwt = skb_hwtstamps(skb);
		u64 now, ts;

		now = ptp_data->tstamp_cc.read(&ptp_data->tstamp_cc);

		*shwt = (struct skb_shared_hwtstamps) {0};

		ts = SJA1105_SKB_CB(skb)->meta_tstamp;
		ts = sja1105_tstamp_reconstruct(ds, now, ts);
		ts = timecounter_cyc2time(&ptp_data->tstamp_tc, ts);

		shwt->hwtstamp = ns_to_ktime(ts);
		netif_rx_ni(skb);
	}

	mutex_unlock(&ptp_data->lock);
}

/* Called from dsa_skb_defer_rx_timestamp */
bool sja1105_port_rxtstamp(struct dsa_switch *ds, int port,
			   struct sk_buff *skb, unsigned int type)
{
	struct sja1105_private *priv = ds->priv;
	struct sja1105_tagger_data *tagger_data = &priv->tagger_data;

	if (!test_bit(SJA1105_HWTS_RX_EN, &tagger_data->state))
		return false;

	/* We need to read the full PTP clock to reconstruct the Rx
	 * timestamp. For that we need a sleepable context.
	 */
	skb_queue_tail(&tagger_data->skb_rxtstamp_queue, skb);
	schedule_work(&tagger_data->rxtstamp_work);
	return true;
}

/* Called from dsa_skb_tx_timestamp. This callback is just to make DSA clone
 * the skb and have it available in DSA_SKB_CB in the .port_deferred_xmit
 * callback, where we will timestamp it synchronously.
 */
bool sja1105_port_txtstamp(struct dsa_switch *ds, int port,
			   struct sk_buff *skb, unsigned int type)
{
	struct sja1105_private *priv = ds->priv;
	struct sja1105_port *sp = &priv->ports[port];

	if (!sp->hwts_tx_en)
		return false;

	return true;
}

int sja1105_ptp_reset(struct dsa_switch *ds)
{
	struct sja1105_private *priv = ds->priv;
	struct sja1105_ptp_data *ptp_data = &priv->ptp_data;
	struct sja1105_ptp_cmd cmd = ptp_data->cmd;
	int rc;

	mutex_lock(&ptp_data->lock);

	cmd.resptp = 1;
	dev_dbg(ds->dev, "Resetting PTP clock\n");
	rc = priv->info->ptp_cmd(ds, &cmd);

	timecounter_init(&ptp_data->tstamp_tc, &ptp_data->tstamp_cc,
			 ktime_to_ns(ktime_get_real()));

	mutex_unlock(&ptp_data->lock);

	return rc;
}

static int sja1105_ptp_gettime(struct ptp_clock_info *ptp,
			       struct timespec64 *ts)
{
	struct sja1105_ptp_data *ptp_data = ptp_caps_to_data(ptp);
	u64 ns;

	mutex_lock(&ptp_data->lock);
	ns = timecounter_read(&ptp_data->tstamp_tc);
	mutex_unlock(&ptp_data->lock);

	*ts = ns_to_timespec64(ns);

	return 0;
}

static int sja1105_ptp_settime(struct ptp_clock_info *ptp,
			       const struct timespec64 *ts)
{
	struct sja1105_ptp_data *ptp_data = ptp_caps_to_data(ptp);
	u64 ns = timespec64_to_ns(ts);

	mutex_lock(&ptp_data->lock);
	timecounter_init(&ptp_data->tstamp_tc, &ptp_data->tstamp_cc, ns);
	mutex_unlock(&ptp_data->lock);

	return 0;
}

static int sja1105_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct sja1105_ptp_data *ptp_data = ptp_caps_to_data(ptp);
	s64 clkrate;

	clkrate = (s64)scaled_ppm * SJA1105_CC_MULT_NUM;
	clkrate = div_s64(clkrate, SJA1105_CC_MULT_DEM);

	mutex_lock(&ptp_data->lock);

	/* Force a readout to update the timer *before* changing its frequency.
	 *
	 * This way, its corrected time curve can at all times be modeled
	 * as a linear "A * x + B" function, where:
	 *
	 * - B are past frequency adjustments and offset shifts, all
	 *   accumulated into the cycle_last variable.
	 *
	 * - A is the new frequency adjustments we're just about to set.
	 *
	 * Reading now makes B accumulate the correct amount of time,
	 * corrected at the old rate, before changing it.
	 *
	 * Hardware timestamps then become simple points on the curve and
	 * are approximated using the above function.  This is still better
	 * than letting the switch take the timestamps using the hardware
	 * rate-corrected clock (PTPCLKVAL) - the comparison in this case would
	 * be that we're shifting the ruler at the same time as we're taking
	 * measurements with it.
	 *
	 * The disadvantage is that it's possible to receive timestamps when
	 * a frequency adjustment took place in the near past.
	 * In this case they will be approximated using the new ppb value
	 * instead of a compound function made of two segments (one at the old
	 * and the other at the new rate) - introducing some inaccuracy.
	 */
	timecounter_read(&ptp_data->tstamp_tc);

	ptp_data->tstamp_cc.mult = SJA1105_CC_MULT + clkrate;

	mutex_unlock(&ptp_data->lock);

	return 0;
}

static int sja1105_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct sja1105_ptp_data *ptp_data = ptp_caps_to_data(ptp);

	mutex_lock(&ptp_data->lock);
	timecounter_adjtime(&ptp_data->tstamp_tc, delta);
	mutex_unlock(&ptp_data->lock);

	return 0;
}

static u64 sja1105_ptptsclk_read(const struct cyclecounter *cc)
{
	struct sja1105_ptp_data *ptp_data = cc_to_ptp_data(cc);
	struct sja1105_private *priv = ptp_data_to_sja1105(ptp_data);
	const struct sja1105_regs *regs = priv->info->regs;
	u64 ptptsclk = 0;
	int rc;

	rc = sja1105_xfer_u64(priv, SPI_READ, regs->ptptsclk, &ptptsclk);
	if (rc < 0)
		dev_err_ratelimited(priv->ds->dev,
				    "failed to read ptp cycle counter: %d\n",
				    rc);
	return ptptsclk;
}

static void sja1105_ptp_overflow_check(struct work_struct *work)
{
	struct delayed_work *dw = to_delayed_work(work);
	struct sja1105_ptp_data *ptp_data = dw_to_ptp_data(dw);
	struct timespec64 ts;

	sja1105_ptp_gettime(&ptp_data->caps, &ts);

	schedule_delayed_work(&ptp_data->refresh_work,
			      SJA1105_REFRESH_INTERVAL);
}

int sja1105_ptp_clock_register(struct dsa_switch *ds)
{
	struct sja1105_private *priv = ds->priv;
	struct sja1105_tagger_data *tagger_data = &priv->tagger_data;
	struct sja1105_ptp_data *ptp_data = &priv->ptp_data;

	/* Set up the cycle counter */
	ptp_data->tstamp_cc = (struct cyclecounter) {
		.read		= sja1105_ptptsclk_read,
		.mask		= CYCLECOUNTER_MASK(64),
		.shift		= SJA1105_CC_SHIFT,
		.mult		= SJA1105_CC_MULT,
	};
	ptp_data->caps = (struct ptp_clock_info) {
		.owner		= THIS_MODULE,
		.name		= "SJA1105 PHC",
		.adjfine	= sja1105_ptp_adjfine,
		.adjtime	= sja1105_ptp_adjtime,
		.gettime64	= sja1105_ptp_gettime,
		.settime64	= sja1105_ptp_settime,
		.max_adj	= SJA1105_MAX_ADJ_PPB,
	};

	skb_queue_head_init(&tagger_data->skb_rxtstamp_queue);
	INIT_WORK(&tagger_data->rxtstamp_work, sja1105_rxtstamp_work);
	spin_lock_init(&tagger_data->meta_lock);

	ptp_data->clock = ptp_clock_register(&ptp_data->caps, ds->dev);
	if (IS_ERR_OR_NULL(ptp_data->clock))
		return PTR_ERR(ptp_data->clock);

	INIT_DELAYED_WORK(&ptp_data->refresh_work, sja1105_ptp_overflow_check);
	schedule_delayed_work(&ptp_data->refresh_work, SJA1105_REFRESH_INTERVAL);

	return sja1105_ptp_reset(ds);
}

void sja1105_ptp_clock_unregister(struct dsa_switch *ds)
{
	struct sja1105_private *priv = ds->priv;
	struct sja1105_ptp_data *ptp_data = &priv->ptp_data;

	if (IS_ERR_OR_NULL(ptp_data->clock))
		return;

	cancel_work_sync(&priv->tagger_data.rxtstamp_work);
	skb_queue_purge(&priv->tagger_data.skb_rxtstamp_queue);
	cancel_delayed_work_sync(&ptp_data->refresh_work);
	ptp_clock_unregister(ptp_data->clock);
	ptp_data->clock = NULL;
}

void sja1105_ptp_txtstamp_skb(struct dsa_switch *ds, int slot,
			      struct sk_buff *skb)
{
	struct sja1105_private *priv = ds->priv;
	struct sja1105_ptp_data *ptp_data = &priv->ptp_data;
	struct skb_shared_hwtstamps shwt = {0};
	u64 now, ts;
	int rc;

	skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;

	mutex_lock(&ptp_data->lock);

	now = ptp_data->tstamp_cc.read(&ptp_data->tstamp_cc);

	rc = sja1105_ptpegr_ts_poll(ds, slot, &ts);
	if (rc < 0) {
		dev_err(ds->dev, "timed out polling for tstamp\n");
		kfree_skb(skb);
		goto out;
	}

	ts = sja1105_tstamp_reconstruct(ds, now, ts);
	ts = timecounter_cyc2time(&ptp_data->tstamp_tc, ts);

	shwt.hwtstamp = ns_to_ktime(ts);
	skb_complete_tx_timestamp(skb, &shwt);

out:
	mutex_unlock(&ptp_data->lock);
}
