// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2016-2018, NXP Semiconductors
 * Copyright (c) 2018, Sensor-Technik Wiedemann GmbH
 * Copyright (c) 2018-2019, Vladimir Oltean <olteanv@gmail.com>
 */
#include <linux/spi/spi.h>
#include <linux/packing.h>
#include "sja1105.h"

#define SPI_TRANSFER_SIZE_MAX  (SIZE_SPI_MSG_HEADER + SIZE_SPI_MSG_MAXLEN)

static int sja1105_spi_transfer(const struct sja1105_private *priv,
				const void *tx, void *rx, int size)
{
	struct spi_device *spi = priv->spidev;
	struct spi_transfer transfer = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len = size,
	};
	struct spi_message msg;
	int rc;

	if (size > SPI_TRANSFER_SIZE_MAX) {
		dev_err(&spi->dev, "SPI message (%d) longer than max of %d\n",
			size, SPI_TRANSFER_SIZE_MAX);
		return -EMSGSIZE;
	}

	spi_message_init(&msg);
	spi_message_add_tail(&transfer, &msg);

	rc = spi_sync(spi, &msg);
	if (rc < 0) {
		dev_err(&spi->dev, "SPI transfer failed: %d\n", rc);
		return rc;
	}

	return rc;
}

static void
sja1105_spi_message_pack(void *buf, const struct sja1105_spi_message *msg)
{
	const int size = SIZE_SPI_MSG_HEADER;

	memset(buf, 0, size);

	sja1105_pack(buf, &msg->access,     31, 31, size);
	sja1105_pack(buf, &msg->read_count, 30, 25, size);
	sja1105_pack(buf, &msg->address,    24,  4, size);
}

/* If read_or_write is:
 *     * SPI_WRITE: creates and sends an SPI write message at absolute
 *                  address reg_addr, taking size_bytes from *packed_buf
 *     * SPI_READ: creates and sends an SPI read message from absolute
 *                 address reg_addr, writing size_bytes into *packed_buf
 *
 * This function should only be called if it is priorly known that
 * size_bytes is smaller than SIZE_SPI_MSG_MAXLEN. Larger packed buffers
 * are chunked in smaller pieces by sja1105_spi_send_long_packed_buf below.
 */
int
sja1105_spi_send_packed_buf(const struct sja1105_private *priv,
			    enum sja1105_spi_access_mode read_or_write,
			    u64 reg_addr, void *packed_buf, size_t size_bytes)
{
	const int msg_len = size_bytes + SIZE_SPI_MSG_HEADER;
	struct sja1105_spi_message msg;
	u8 tx_buf[SIZE_SPI_MSG_HEADER + SIZE_SPI_MSG_MAXLEN];
	u8 rx_buf[SIZE_SPI_MSG_HEADER + SIZE_SPI_MSG_MAXLEN];
	int rc;

	if (msg_len > SIZE_SPI_MSG_HEADER + SIZE_SPI_MSG_MAXLEN)
		return -ERANGE;

	memset(rx_buf, 0, msg_len);

	msg.access     = read_or_write;
	msg.read_count = (read_or_write == SPI_READ) ? (size_bytes / 4) : 0;
	msg.address    = reg_addr;
	sja1105_spi_message_pack(tx_buf, &msg);

	if (read_or_write == SPI_READ)
		memset(tx_buf + SIZE_SPI_MSG_HEADER, 0, size_bytes);
	else if (read_or_write == SPI_WRITE)
		memcpy(tx_buf + SIZE_SPI_MSG_HEADER, packed_buf, size_bytes);
	else
		return -EINVAL;

	rc = sja1105_spi_transfer(priv, tx_buf, rx_buf, msg_len);
	if (rc < 0)
		return rc;

	if (read_or_write == SPI_READ)
		memcpy(packed_buf, rx_buf + SIZE_SPI_MSG_HEADER, size_bytes);

	return 0;
}

/* If read_or_write is:
 *     * SPI_WRITE: creates and sends an SPI write message at absolute
 *                  address reg_addr, taking size_bytes from *value
 *     * SPI_READ: creates and sends an SPI read message from absolute
 *                 address reg_addr, writing size_bytes into *value
 *
 * The u64 *value is unpacked, meaning that it's stored in the native
 * CPU endianness and directly usable by software running on the core.
 *
 * This is a wrapper around sja1105_spi_send_packed_buf().
 */
int sja1105_spi_send_int(const struct sja1105_private *priv,
			 enum sja1105_spi_access_mode read_or_write,
			 u64 reg_addr, u64 *value, u64 size_bytes)
{
	u8 packed_buf[SIZE_SPI_MSG_MAXLEN];
	int rc;

	if (size_bytes > SIZE_SPI_MSG_MAXLEN)
		return -ERANGE;

	if (read_or_write == SPI_WRITE)
		sja1105_pack(packed_buf, value, 8 * size_bytes - 1, 0,
			     size_bytes);

	rc = sja1105_spi_send_packed_buf(priv, read_or_write, reg_addr,
					 packed_buf, size_bytes);

	if (read_or_write == SPI_READ)
		sja1105_unpack(packed_buf, value, 8 * size_bytes - 1, 0,
			       size_bytes);

	return rc;
}

/* Should be used if a packed_buf larger than SIZE_SPI_MSG_MAXLEN must be
 * sent/received. Splitting the buffer into chunks and assembling those
 * into SPI messages is done automatically by this function.
 */
int
sja1105_spi_send_long_packed_buf(const struct sja1105_private *priv,
				 enum sja1105_spi_access_mode read_or_write,
				 u64 base_addr, void *packed_buf, u64 buf_len)
{
	struct chunk {
		void *buf_ptr;
		int len;
		u64 spi_address;
	} chunk;
	int distance_to_end;
	int rc = 0;

	/* Initialize chunk */
	chunk.buf_ptr = packed_buf;
	chunk.spi_address = base_addr;
	chunk.len = min_t(int, buf_len, SIZE_SPI_MSG_MAXLEN);

	while (chunk.len) {
		rc = sja1105_spi_send_packed_buf(priv, read_or_write,
						 chunk.spi_address,
						 chunk.buf_ptr, chunk.len);
		if (rc < 0)
			return rc;

		chunk.buf_ptr += chunk.len;
		chunk.spi_address += chunk.len / 4;
		distance_to_end = (uintptr_t)((packed_buf + buf_len) -
					chunk.buf_ptr);
		chunk.len = min(distance_to_end, SIZE_SPI_MSG_MAXLEN);
	}

	return 0;
}

/* Back-ported structure from UM11040 Table 112.
 * Reset control register (addr. 100440h)
 * In the SJA1105 E/T, only warm_rst and cold_rst are
 * supported (exposed in UM10944 as rst_ctrl), but the bit
 * offsets of warm_rst and cold_rst are actually reversed.
 */
struct sja1105_reset_cmd {
	u64 switch_rst;
	u64 cfg_rst;
	u64 car_rst;
	u64 otp_rst;
	u64 warm_rst;
	u64 cold_rst;
	u64 por_rst;
};

static void
sja1105et_reset_cmd_pack(void *buf, const struct sja1105_reset_cmd *reset)
{
	const int size = 4;

	memset(buf, 0, size);

	sja1105_pack(buf, &reset->cold_rst, 3, 3, size);
	sja1105_pack(buf, &reset->warm_rst, 2, 2, size);
}

static void
sja1105pqrs_reset_cmd_pack(void *buf, const struct sja1105_reset_cmd *reset)
{
	const int size = 4;

	memset(buf, 0, size);

	sja1105_pack(buf, &reset->switch_rst, 8, 8, size);
	sja1105_pack(buf, &reset->cfg_rst,    7, 7, size);
	sja1105_pack(buf, &reset->car_rst,    5, 5, size);
	sja1105_pack(buf, &reset->otp_rst,    4, 4, size);
	sja1105_pack(buf, &reset->warm_rst,   3, 3, size);
	sja1105_pack(buf, &reset->cold_rst,   2, 2, size);
	sja1105_pack(buf, &reset->por_rst,    1, 1, size);
}

static int sja1105_reset_cmd_commit(const struct sja1105_private *priv,
				    const struct sja1105_reset_cmd *reset)
{
#define BUF_LEN 4
	struct device *dev = priv->ds->dev;
	u8 packed_buf[BUF_LEN];

	if (reset->switch_rst)
		dev_dbg(dev, "Main reset for all functional modules requested\n");
	if (reset->cfg_rst)
		dev_dbg(dev, "Chip configuration reset requested\n");
	if (reset->car_rst)
		dev_dbg(dev, "Clock and reset control logic reset requested\n");
	if (reset->otp_rst)
		dev_dbg(dev, "OTP read cycle for reading product "
			"config settings requested\n");
	if (reset->warm_rst)
		dev_dbg(dev, "Warm reset requested\n");
	if (reset->cold_rst)
		dev_dbg(dev, "Cold reset requested\n");
	if (reset->por_rst)
		dev_dbg(dev, "Power-on reset requested\n");

	if ((reset->switch_rst || reset->cfg_rst || reset->car_rst ||
	     reset->otp_rst || reset->por_rst) && IS_ET(priv->device_id)) {
		dev_err(dev, "Only warm and cold reset is supported "
			"for SJA1105 E/T!\n");
		return -EINVAL;
	}
	if (IS_ET(priv->device_id))
		sja1105et_reset_cmd_pack(packed_buf, reset);
	else
		sja1105pqrs_reset_cmd_pack(packed_buf, reset);

	return sja1105_spi_send_packed_buf(priv, SPI_WRITE, priv->regs->rgu,
					   packed_buf, BUF_LEN);
#undef BUF_LEN
}

static int sja1105_cold_reset(const struct sja1105_private *priv)
{
	struct sja1105_reset_cmd reset = {0};

	reset.cold_rst = 1;
	return sja1105_reset_cmd_commit(priv, &reset);
}

static const char *SJA1105E_DEVICE_ID_STR   = "SJA1105E";
static const char *SJA1105T_DEVICE_ID_STR   = "SJA1105T";
static const char *SJA1105P_DEVICE_ID_STR   = "SJA1105P";
static const char *SJA1105Q_DEVICE_ID_STR   = "SJA1105Q";
static const char *SJA1105R_DEVICE_ID_STR   = "SJA1105R";
static const char *SJA1105S_DEVICE_ID_STR   = "SJA1105S";
static const char *SJA1105PR_DEVICE_ID_STR  = "SJA1105P or SJA1105R";
static const char *SJA1105QS_DEVICE_ID_STR  = "SJA1105Q or SJA1105S";
static const char *SJA1105_NO_DEVICE_ID_STR = "None";

const char *sja1105_device_id_string_get(u64 device_id, u64 part_nr)
{
	if (device_id == SJA1105E_DEVICE_ID)
		return SJA1105E_DEVICE_ID_STR;
	if (device_id == SJA1105T_DEVICE_ID)
		return SJA1105T_DEVICE_ID_STR;
	/* P and R have same Device ID, and differ by Part Number.
	 * Same do Q and S.
	 */
	if (IS_P(device_id, part_nr))
		return SJA1105P_DEVICE_ID_STR;
	if (IS_Q(device_id, part_nr))
		return SJA1105Q_DEVICE_ID_STR;
	if (IS_R(device_id, part_nr))
		return SJA1105R_DEVICE_ID_STR;
	if (IS_S(device_id, part_nr))
		return SJA1105S_DEVICE_ID_STR;
	/* Fallback: if we don't know/care what the part_nr is, and we
	 * have a P/R, we can simply pass -1 to part_nr and have this
	 * function say it's either P or R, instead of reporting it
	 * as invalid.
	 */
	if (device_id == SJA1105PR_DEVICE_ID)
		return SJA1105PR_DEVICE_ID_STR;
	if (device_id == SJA1105QS_DEVICE_ID)
		return SJA1105QS_DEVICE_ID_STR;

	return SJA1105_NO_DEVICE_ID_STR;
}

struct sja1105_regs sja1105et_regs = {
	.rgu = 0x100440,
	.config = 0x020000,
	.pad_mii_tx = {0x100800, 0x100802, 0x100804, 0x100806, 0x100808},
	.ptpegr_ts = 0xC0,
	.rmii_pll1 = 0x10000A,
	.cgu_idiv = {0x10000B, 0x10000C, 0x10000D, 0x10000E, 0x10000F},
	/* UM10944.pdf, Table 86, ACU Register overview */
	.rgmii_pad_mii_tx = {0x100800, 0x100802, 0x100804, 0x100806, 0x100808},
	/* The base address is off-by-1 compared to UM10944,
	 * because we are skipping device_id from the readout.
	 */
	.general_status = 0x1,
	.mac = {0x200, 0x202, 0x204, 0x206, 0x208},
	.mac_hl1 = {0x400, 0x410, 0x420, 0x430, 0x440},
	.mac_hl2 = {0x600, 0x610, 0x620, 0x630, 0x640},
	.ptpegr_ts_mask = GENMASK_ULL(23, 0),
	.ptp_control = 0x17,
	.ptpclk = 0x18,
	.ptpclkrate = 0x1A,
	.ptptsclk = 0x1B,
	/* UM10944.pdf, Table 78, CGU Register overview */
	.mii_tx_clk = {0x100013, 0x10001A, 0x100021, 0x100028, 0x10002F},
	.mii_rx_clk = {0x100014, 0x10001B, 0x100022, 0x100029, 0x100030},
	.mii_ext_tx_clk = {0x100018, 0x10001F, 0x100026, 0x10002D, 0x100034},
	.mii_ext_rx_clk = {0x100019, 0x100020, 0x100027, 0x10002E, 0x100035},
	.rgmii_txc = {0x100016, 0x10001D, 0x100024, 0x10002B, 0x100032},
	.rmii_ref_clk = {0x100015, 0x10001C, 0x100023, 0x10002A, 0x100031},
	.rmii_ext_tx_clk = {0x100018, 0x10001F, 0x100026, 0x10002D, 0x100034},
};

struct sja1105_regs sja1105pqrs_regs = {
	.rgu = 0x100440,
	.config = 0x020000,
	.pad_mii_tx = {0x100800, 0x100802, 0x100804, 0x100806, 0x100808},
	.ptpegr_ts = 0xC0,
	.rmii_pll1 = 0x10000A,
	.cgu_idiv = {0x10000B, 0x10000C, 0x10000D, 0x10000E, 0x10000F},
	/* UM10944.pdf, Table 86, ACU Register overview */
	.rgmii_pad_mii_tx = {0x100800, 0x100802, 0x100804, 0x100806, 0x100808},
	/* The base address is off-by-1 compared to UM10944,
	 * because we are skipping device_id from the readout.
	 */
	.general_status = 0x1,
	.mac = {0x200, 0x202, 0x204, 0x206, 0x208},
	.mac_hl1 = {0x400, 0x410, 0x420, 0x430, 0x440},
	.mac_hl2 = {0x600, 0x610, 0x620, 0x630, 0x640},
	.ptpegr_ts_mask = GENMASK_ULL(31, 0),
	.ptp_control = 0x18,
	.ptpclk = 0x19,
	.ptpclkrate = 0x1B,
	.ptptsclk = 0x1C,
	/* UM11040.pdf, Table 114 */
	.mii_tx_clk = {0x100013, 0x100019, 0x10001F, 0x100025, 0x10002B},
	.mii_rx_clk = {0x100014, 0x10001A, 0x100020, 0x100026, 0x10002C},
	.mii_ext_tx_clk = {0x100017, 0x10001D, 0x100023, 0x100029, 0x10002F},
	.mii_ext_rx_clk = {0x100018, 0x10001E, 0x100024, 0x10002A, 0x100030},
	.rgmii_txc = {0x100016, 0x10001C, 0x100022, 0x100028, 0x10002E},
	.rmii_ref_clk = {0x100015, 0x10001B, 0x100021, 0x100027, 0x10002D},
	.rmii_ext_tx_clk = {0x100017, 0x10001D, 0x100023, 0x100029, 0x10002F},
	.qlevel = {0x604, 0x614, 0x624, 0x634, 0x644},
};

/* Populates priv structures device_id, part_nr and regs */
int sja1105_device_id_get(struct sja1105_private *priv)
{
#define DEVICE_ID_ADDR	0x0
#define PROD_ID_ADDR	0x100BC3
	/* These can't be part of regs, because otherwise we'd have
	 * a chicken and egg problem
	 */
	u64 compatible_device_ids[] = {
		SJA1105E_DEVICE_ID,
		SJA1105T_DEVICE_ID,
		SJA1105PR_DEVICE_ID,
		SJA1105QS_DEVICE_ID,
	};
	struct device *dev = priv->ds->dev;
	u64 tmp_device_id;
	u64 tmp_part_nr;
	unsigned int i;
	int rc;

	rc = sja1105_spi_send_int(priv, SPI_READ, DEVICE_ID_ADDR,
				  &tmp_device_id, SIZE_SJA1105_DEVICE_ID);
	if (rc < 0)
		return rc;

	priv->device_id = SJA1105_NO_DEVICE_ID;
	for (i = 0; i < ARRAY_SIZE(compatible_device_ids); i++) {
		if (tmp_device_id == compatible_device_ids[i]) {
			priv->device_id = compatible_device_ids[i];
			break;
		}
	}
	if (priv->device_id == SJA1105_NO_DEVICE_ID) {
		dev_err(dev, "Unrecognized Device ID 0x%llx\n", tmp_device_id);
		return -EINVAL;
	}
	if (IS_PQRS(priv->device_id)) {
		rc = sja1105_spi_send_int(priv, SPI_READ, PROD_ID_ADDR,
					  &tmp_part_nr, 4);
		if (rc < 0)
			return -EINVAL;

		sja1105_unpack(&tmp_part_nr, &priv->part_nr, 19, 4, 4);
	}

	if (IS_ET(priv->device_id))
		priv->regs = &sja1105et_regs;
	else if (IS_PQRS(priv->device_id))
		priv->regs = &sja1105pqrs_regs;

	return 0;
#undef PROD_ID_ADDR
#undef DEVICE_ID_ADDR
}

struct sja1105_general_status {
	u64 configs;
	u64 crcchkl;
	u64 ids;
	u64 crcchkg;
	u64 nslot;
	u64 vlind;
	u64 vlparind;
	u64 vlroutes;
	u64 vlparts;
	u64 macaddl;
	u64 portenf;
	u64 fwds_03h;
	u64 macfds;
	u64 enffds;
	u64 l2busyfds;
	u64 l2busys;
	u64 macaddu;
	u64 macaddhcl;
	u64 vlanidhc;
	u64 hashconfs;
	u64 macaddhcu;
	u64 wpvlanid;
	u64 port_07h;
	u64 vlanbusys;
	u64 wrongports;
	u64 vnotfounds;
	u64 vlid;
	u64 portvl;
	u64 vlnotfound;
	u64 emptys;
	u64 buffers;
	u64 buflwmark; /* Only on P/Q/R/S */
	u64 port_0ah;
	u64 fwds_0ah;
	u64 parts;
	u64 ramparerrl;
	u64 ramparerru;
};

static void
sja1105_general_status_unpack(void *buf, struct sja1105_general_status *status,
			      u64 device_id)
{
	/* So that addition translates to 4 bytes */
	u32 *p = (u32 *)buf;

	memset(status, 0, sizeof(*status));
	/* device_id is missing from the buffer, but we don't
	 * want to diverge from the manual definition of the
	 * register addresses, so we'll back off one step with
	 * the register pointer, and never access p[0].
	 */
	p--;
	sja1105_unpack(p + 0x1, &status->configs,   31, 31, 4);
	sja1105_unpack(p + 0x1, &status->crcchkl,   30, 30, 4);
	sja1105_unpack(p + 0x1, &status->ids,       29, 29, 4);
	sja1105_unpack(p + 0x1, &status->crcchkg,   28, 28, 4);
	sja1105_unpack(p + 0x1, &status->nslot,      3,  0, 4);
	sja1105_unpack(p + 0x2, &status->vlind,     31, 16, 4);
	sja1105_unpack(p + 0x2, &status->vlparind,  15,  8, 4);
	sja1105_unpack(p + 0x2, &status->vlroutes,   1,  1, 4);
	sja1105_unpack(p + 0x2, &status->vlparts,    0,  0, 4);
	sja1105_unpack(p + 0x3, &status->macaddl,   31, 16, 4);
	sja1105_unpack(p + 0x3, &status->portenf,   15,  8, 4);
	sja1105_unpack(p + 0x3, &status->fwds_03h,   4,  4, 4);
	sja1105_unpack(p + 0x3, &status->macfds,     3,  3, 4);
	sja1105_unpack(p + 0x3, &status->enffds,     2,  2, 4);
	sja1105_unpack(p + 0x3, &status->l2busyfds,  1,  1, 4);
	sja1105_unpack(p + 0x3, &status->l2busys,    0,  0, 4);
	sja1105_unpack(p + 0x4, &status->macaddu,   31,  0, 4);
	sja1105_unpack(p + 0x5, &status->macaddhcl, 31, 16, 4);
	sja1105_unpack(p + 0x5, &status->vlanidhc,  15,  4, 4);
	sja1105_unpack(p + 0x5, &status->hashconfs,  0,  0, 4);
	sja1105_unpack(p + 0x6, &status->macaddhcu, 31,  0, 4);
	sja1105_unpack(p + 0x7, &status->wpvlanid,  31, 16, 4);
	sja1105_unpack(p + 0x7, &status->port_07h,  15,  8, 4);
	sja1105_unpack(p + 0x7, &status->vlanbusys,  4,  4, 4);
	sja1105_unpack(p + 0x7, &status->wrongports, 3,  3, 4);
	sja1105_unpack(p + 0x7, &status->vnotfounds, 2,  2, 4);
	sja1105_unpack(p + 0x8, &status->vlid,      31, 16, 4);
	sja1105_unpack(p + 0x8, &status->portvl,    15,  8, 4);
	sja1105_unpack(p + 0x8, &status->vlnotfound, 0,  0, 4);
	sja1105_unpack(p + 0x9, &status->emptys,    31, 31, 4);
	sja1105_unpack(p + 0x9, &status->buffers,   30,  0, 4);
	if (IS_ET(device_id)) {
		sja1105_unpack(p + 0xA, &status->port_0ah,   15,  8, 4);
		sja1105_unpack(p + 0xA, &status->fwds_0ah,    1,  1, 4);
		sja1105_unpack(p + 0xA, &status->parts,       0,  0, 4);
		sja1105_unpack(p + 0xB, &status->ramparerrl, 20,  0, 4);
		sja1105_unpack(p + 0xC, &status->ramparerru,  4,  0, 4);
	} else {
		sja1105_unpack(p + 0xA, &status->buflwmark,  30,  0, 4);
		sja1105_unpack(p + 0xB, &status->port_0ah,   15,  8, 4);
		sja1105_unpack(p + 0xB, &status->fwds_0ah,    1,  1, 4);
		sja1105_unpack(p + 0xB, &status->parts,       0,  0, 4);
		sja1105_unpack(p + 0xC, &status->ramparerrl, 22,  0, 4);
		sja1105_unpack(p + 0xD, &status->ramparerru,  4,  0, 4);
	}
}

static int sja1105_general_status_get(struct sja1105_private *priv,
				      struct sja1105_general_status *status)
{
#define SIZE_ET   (0x0C * 4) /* 0x01 to 0x0C */
#define SIZE_PQRS (0x0D * 4) /* 0x01 to 0x0D */
#define MAX_SIZE (max(SIZE_ET, SIZE_PQRS))
	u8 packed_buf[MAX_SIZE];
	const int size = IS_ET(priv->device_id) ? SIZE_ET : SIZE_PQRS;
	int rc;

	rc = sja1105_spi_send_packed_buf(priv, SPI_READ,
					 priv->regs->general_status,
					 packed_buf, size);
	if (rc < 0)
		return rc;

	sja1105_general_status_unpack(packed_buf, status, priv->device_id);

	return 0;
#undef MAX_SIZE
#undef SIZE_PQRS
#undef SIZE_ET
}

/* Not const because unpacking priv->static_config into buffers and preparing
 * for upload requires the recalculation of table CRCs and updating the
 * structures with these.
 */
static int
static_config_buf_prepare_for_upload(struct sja1105_private *priv,
				     void *config_buf, int buf_len)
{
	struct sja1105_static_config *config = &priv->static_config;
	enum sja1105_static_config_validity valid;
	struct sja1105_table_header final_header;
	char *final_header_ptr;
	int crc_len;

	valid = sja1105_static_config_check_valid(config);
	if (valid != SJA1105_CONFIG_OK) {
		dev_err(&priv->spidev->dev,
			sja1105_static_config_error_msg[valid]);
		return -EINVAL;
	}

	if (config->device_id != priv->device_id) {
		dev_err(&priv->spidev->dev,
			"The static config is for device id %llx "
			"but the chip is %s (%llx)\n", config->device_id,
			sja1105_device_id_string_get(priv->device_id,
						     priv->part_nr),
			priv->device_id);
		return -EINVAL;
	}

	/* Write Device ID and config tables to config_buf */
	sja1105_static_config_pack(config_buf, config);
	/* Recalculate CRC of the last header (right now 0xDEADBEEF).
	 * Don't include the CRC field itself.
	 */
	crc_len = buf_len - 4;
	/* Read the whole table header */
	final_header_ptr = config_buf + buf_len - SIZE_TABLE_HEADER;
	sja1105_table_header_packing(final_header_ptr, &final_header, UNPACK);
	/* Modify */
	final_header.crc = sja1105_crc32(config_buf, crc_len);
	/* Rewrite */
	sja1105_table_header_packing(final_header_ptr, &final_header, PACK);

	return 0;
}

int sja1105_static_config_upload(struct sja1105_private *priv)
{
#define RETRIES 10
	struct sja1105_static_config *config = &priv->static_config;
	struct device *dev = &priv->spidev->dev;
	struct sja1105_general_status status;
	int rc, retries = RETRIES;
	u8 *config_buf;
	int buf_len;

	buf_len = sja1105_static_config_get_length(config);
	config_buf = kcalloc(buf_len, sizeof(char), GFP_KERNEL);
	if (!config_buf)
		return -ENOMEM;

	rc = static_config_buf_prepare_for_upload(priv, config_buf, buf_len);
	if (rc < 0) {
		dev_err(dev, "Invalid config, cannot upload\n");
		return -EINVAL;
	}
	do {
		/* Put the SJA1105 in programming mode */
		rc = sja1105_cold_reset(priv);
		if (rc < 0) {
			dev_err(dev, "Failed to reset switch, retrying...\n");
			continue;
		}
		/* Wait for the switch to come out of reset */
		usleep_range(1000, 5000);
		/* Upload the static config to the device */
		rc = sja1105_spi_send_long_packed_buf(priv, SPI_WRITE,
						      priv->regs->config,
						      config_buf, buf_len);
		if (rc < 0) {
			dev_err(dev, "Failed to upload config, retrying...\n");
			continue;
		}
		/* Check that SJA1105 responded well to the config upload */
		rc = sja1105_general_status_get(priv, &status);
		if (rc < 0)
			continue;

		if (status.ids == 1) {
			dev_err(dev, "Mismatch between hardware and staging area "
				"device id. Wrote 0x%llx, wants 0x%llx\n",
				config->device_id, priv->device_id);
			continue;
		}
		if (status.crcchkl == 1) {
			dev_err(dev, "Switch reported invalid local CRC on "
				"the uploaded config, retrying...\n");
			continue;
		}
		if (status.crcchkg == 1) {
			dev_err(dev, "Switch reported invalid global CRC on "
				"the uploaded config, retrying...\n");
			continue;
		}
		if (status.configs == 0) {
			dev_err(dev, "Switch reported that configuration is "
				"invalid, retrying...\n");
			continue;
		}
	} while (--retries && (status.crcchkl == 1 || status.crcchkg == 1 ||
		 status.configs == 0 || status.ids == 1));

	if (!retries) {
		rc = -EIO;
		dev_err(dev, "Failed to upload config to device, giving up\n");
		goto out;
	} else if (retries != RETRIES - 1) {
		dev_info(dev, "Succeeded after %d tried\n", RETRIES - retries);
	}

	dev_info(dev, "Reset switch and programmed static config\n");
out:
	kfree(config_buf);
	return rc;
#undef RETRIES
}

