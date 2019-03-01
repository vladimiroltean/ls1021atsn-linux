// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Copyright (c) 2016-2018, NXP Semiconductors
 * Copyright (c) 2018-2019, Vladimir Oltean <olteanv@gmail.com>
 */
#include <linux/packing.h>
#include "sja1105.h"

struct sja1105_cfg_pad_mii_tx {
	u64 d32_os;
	u64 d32_ipud;
	u64 d10_os;
	u64 d10_ipud;
	u64 ctrl_os;
	u64 ctrl_ipud;
	u64 clk_os;
	u64 clk_ih;
	u64 clk_ipud;
};

/* UM10944 Table 82.
 * IDIV_0_C to IDIV_4_C control registers
 * (addr. 10000Bh to 10000Fh)
 */
struct sja1105_cgu_idiv {
	u64 clksrc;
	u64 autoblock;
	u64 idiv;
	u64 pd;
};

/* UM10944 Table 80.
 * PLL_x_S clock status registers 0 and 1
 * (address 100007h and 100009h)
 */
struct sja1105_cgu_pll_status {
	u64 lock;
};

/* PLL_1_C control register
 *
 * SJA1105 E/T: UM10944 Table 81 (address 10000Ah)
 * SJA1105 P/Q/R/S: UM11040 Table 116 (address 10000Ah)
 */
struct sja1105_cgu_pll_control {
	u64 pllclksrc;
	u64 msel;
	u64 nsel; /* Only for P/Q/R/S series */
	u64 autoblock;
	u64 psel;
	u64 direct;
	u64 fbsel;
	u64 p23en; /* Only for P/Q/R/S series */
	u64 bypass;
	u64 pd;
};

#define CLKSRC_MII0_TX_CLK 0x00
#define CLKSRC_MII0_RX_CLK 0x01
#define CLKSRC_MII1_TX_CLK 0x02
#define CLKSRC_MII1_RX_CLK 0x03
#define CLKSRC_MII2_TX_CLK 0x04
#define CLKSRC_MII2_RX_CLK 0x05
#define CLKSRC_MII3_TX_CLK 0x06
#define CLKSRC_MII3_RX_CLK 0x07
#define CLKSRC_MII4_TX_CLK 0x08
#define CLKSRC_MII4_RX_CLK 0x09
#define CLKSRC_PLL0        0x0B
#define CLKSRC_PLL1        0x0E
#define CLKSRC_IDIV0       0x11
#define CLKSRC_IDIV1       0x12
#define CLKSRC_IDIV2       0x13
#define CLKSRC_IDIV3       0x14
#define CLKSRC_IDIV4       0x15

/* UM10944 Table 83.
 * MIIx clock control registers 1 to 30
 * (addresses 100013h to 100035h)
 */
struct sja1105_cgu_mii_control {
	u64 clksrc;
	u64 autoblock;
	u64 pd;
};

static void sja1105_cgu_idiv_packing(void *buf, struct sja1105_cgu_idiv *idiv,
				     enum packing_op op)
{
	const int size = 4;

	if (op == UNPACK)
		memset(idiv, 0, sizeof(*idiv));
	else
		memset(buf, 0, size);

	sja1105_packing(buf, &idiv->clksrc,    28, 24, size, op);
	sja1105_packing(buf, &idiv->autoblock, 11, 11, size, op);
	sja1105_packing(buf, &idiv->idiv,       5,  2, size, op);
	sja1105_packing(buf, &idiv->pd,         0,  0, size, op);
}

static int sja1105_cgu_idiv_config(struct sja1105_private *priv, int port,
				   bool enabled, int factor)
{
#define BUF_LEN 4
	/* UM10944.pdf, Table 78, CGU Register overview */
	const int idiv_offsets[] = {0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
	struct device *dev = priv->ds->dev;
	struct sja1105_cgu_idiv idiv;
	u8 packed_buf[BUF_LEN];

	if (enabled && factor != 1 && factor != 10) {
		dev_err(dev, "idiv factor must be 1 or 10\n");
		return -ERANGE;
	}

	/* Payload for packed_buf */
	idiv.clksrc    = 0x0A;            /* 25MHz */
	idiv.autoblock = 1;               /* Block clk automatically */
	idiv.idiv      = factor - 1;      /* Divide by 1 or 10 */
	idiv.pd        = enabled ? 0 : 1; /* Power down? */
	sja1105_cgu_idiv_packing(packed_buf, &idiv, PACK);

	return sja1105_spi_send_packed_buf(priv, SPI_WRITE, CGU_ADDR +
			idiv_offsets[port], packed_buf, BUF_LEN);
#undef BUF_LEN
}

static void sja1105_cgu_mii_control_packing(void *buf,
		struct sja1105_cgu_mii_control *mii_control, enum packing_op op)
{
	const int size = 4;

	if (op == UNPACK)
		memset(mii_control, 0, sizeof(*mii_control));
	else
		memset(buf, 0, size);

	sja1105_packing(buf, &mii_control->clksrc,    28, 24, 4, op);
	sja1105_packing(buf, &mii_control->autoblock, 11, 11, 4, op);
	sja1105_packing(buf, &mii_control->pd,         0,  0, 4, op);
}

static int
sja1105_cgu_mii_tx_clk_config(struct sja1105_private *priv, int port, int mii_mode)
{
#define BUF_LEN 4
	u8 packed_buf[BUF_LEN];
	struct  sja1105_cgu_mii_control mii_tx_clk;
	/* UM10944.pdf, Table 78, CGU Register overview */
	const int  mii_tx_clk_offsets_et[]   = {0x13, 0x1A, 0x21, 0x28, 0x2F};
	/* UM11040.pdf, Table 114 */
	const int  mii_tx_clk_offsets_pqrs[] = {0x13, 0x19, 0x1F, 0x25, 0x2B};
	const int *mii_tx_clk_offsets;
	const int mac_clk_sources[] = {
		CLKSRC_MII0_TX_CLK,
		CLKSRC_MII1_TX_CLK,
		CLKSRC_MII2_TX_CLK,
		CLKSRC_MII3_TX_CLK,
		CLKSRC_MII4_TX_CLK,
	};
	const int phy_clk_sources[] = {
		CLKSRC_IDIV0,
		CLKSRC_IDIV1,
		CLKSRC_IDIV2,
		CLKSRC_IDIV3,
		CLKSRC_IDIV4,
	};
	int clksrc;

	/* E/T and P/Q/R/S compatibility */
	if (IS_ET(priv->device_id))
		mii_tx_clk_offsets = mii_tx_clk_offsets_et;
	else
		mii_tx_clk_offsets = mii_tx_clk_offsets_pqrs;

	if (mii_mode == XMII_MAC)
		clksrc = mac_clk_sources[port];
	else
		clksrc = phy_clk_sources[port];

	/* Payload for packed_buf */
	mii_tx_clk.clksrc    = clksrc;
	mii_tx_clk.autoblock = 1;  /* Autoblock clk while changing clksrc */
	mii_tx_clk.pd        = 0;  /* Power Down off => enabled */
	sja1105_cgu_mii_control_packing(packed_buf, &mii_tx_clk, PACK);

	return sja1105_spi_send_packed_buf(priv, SPI_WRITE, CGU_ADDR +
			mii_tx_clk_offsets[port], packed_buf, BUF_LEN);
#undef BUF_LEN
}

static int
sja1105_cgu_mii_rx_clk_config(struct sja1105_private *priv, int port)
{
#define BUF_LEN 4
	u8 packed_buf[BUF_LEN];
	struct  sja1105_cgu_mii_control mii_rx_clk;
	/* UM10944.pdf, Table 78, CGU Register overview */
	const int  mii_rx_clk_offsets_et[]   = {0x14, 0x1B, 0x22, 0x29, 0x30};
	/* UM11040.pdf, Table 114 */
	const int  mii_rx_clk_offsets_pqrs[] = {0x14, 0x1A, 0x20, 0x26, 0x2C};
	const int *mii_rx_clk_offsets;
	const int clk_sources[] = {
		CLKSRC_MII0_RX_CLK,
		CLKSRC_MII1_RX_CLK,
		CLKSRC_MII2_RX_CLK,
		CLKSRC_MII3_RX_CLK,
		CLKSRC_MII4_RX_CLK,
	};

	/* E/T and P/Q/R/S compatibility */
	if (IS_ET(priv->device_id))
		mii_rx_clk_offsets = mii_rx_clk_offsets_et;
	else
		mii_rx_clk_offsets = mii_rx_clk_offsets_pqrs;

	/* Payload for packed_buf */
	mii_rx_clk.clksrc    = clk_sources[port];
	mii_rx_clk.autoblock = 1;  /* Autoblock clk while changing clksrc */
	mii_rx_clk.pd        = 0;  /* Power Down off => enabled */
	sja1105_cgu_mii_control_packing(packed_buf, &mii_rx_clk, PACK);

	return sja1105_spi_send_packed_buf(priv, SPI_WRITE, CGU_ADDR +
			mii_rx_clk_offsets[port], packed_buf, BUF_LEN);
#undef BUF_LEN
}

static int
sja1105_cgu_mii_ext_tx_clk_config(struct sja1105_private *priv, int port)
{
#define BUF_LEN 4
	u8 packed_buf[BUF_LEN];
	struct  sja1105_cgu_mii_control mii_ext_tx_clk;
	/* UM10944.pdf, Table 78, CGU Register overview */
	const int  mii_ext_tx_clk_offsets_et[]   = {0x18, 0x1F, 0x26, 0x2D, 0x34};
	/* UM11040.pdf, Table 114 */
	const int  mii_ext_tx_clk_offsets_pqrs[] = {0x17, 0x1D, 0x23, 0x29, 0x2F};
	const int *mii_ext_tx_clk_offsets;
	const int clk_sources[] = {
		CLKSRC_IDIV0,
		CLKSRC_IDIV1,
		CLKSRC_IDIV2,
		CLKSRC_IDIV3,
		CLKSRC_IDIV4,
	};

	/* E/T and P/Q/R/S compatibility */
	if (IS_ET(priv->device_id))
		mii_ext_tx_clk_offsets = mii_ext_tx_clk_offsets_et;
	else
		mii_ext_tx_clk_offsets = mii_ext_tx_clk_offsets_pqrs;

	/* Payload for packed_buf */
	mii_ext_tx_clk.clksrc    = clk_sources[port];
	mii_ext_tx_clk.autoblock = 1; /* Autoblock clk while changing clksrc */
	mii_ext_tx_clk.pd        = 0; /* Power Down off => enabled */
	sja1105_cgu_mii_control_packing(packed_buf, &mii_ext_tx_clk, PACK);

	return sja1105_spi_send_packed_buf(priv, SPI_WRITE, CGU_ADDR +
			mii_ext_tx_clk_offsets[port], packed_buf, BUF_LEN);
#undef BUF_LEN
}

static int
sja1105_cgu_mii_ext_rx_clk_config(struct sja1105_private *priv, int port)
{
#define BUF_LEN 4
	u8 packed_buf[BUF_LEN];
	struct  sja1105_cgu_mii_control mii_ext_rx_clk;
	/* UM10944.pdf, Table 78, CGU Register overview */
	const int  mii_ext_rx_clk_offsets_et[]   = {0x19, 0x20, 0x27, 0x2E, 0x35};
	/* UM11040.pdf, Table 114 */
	const int  mii_ext_rx_clk_offsets_pqrs[] = {0x18, 0x1E, 0x24, 0x2A, 0x30};
	const int *mii_ext_rx_clk_offsets;
	const int clk_sources[] = {
		CLKSRC_IDIV0,
		CLKSRC_IDIV1,
		CLKSRC_IDIV2,
		CLKSRC_IDIV3,
		CLKSRC_IDIV4,
	};

	/* E/T and P/Q/R/S compatibility */
	if (IS_ET(priv->device_id))
		mii_ext_rx_clk_offsets = mii_ext_rx_clk_offsets_et;
	else
		mii_ext_rx_clk_offsets = mii_ext_rx_clk_offsets_pqrs;

	/* Payload for packed_buf */
	mii_ext_rx_clk.clksrc    = clk_sources[port];
	mii_ext_rx_clk.autoblock = 1; /* Autoblock clk while changing clksrc */
	mii_ext_rx_clk.pd        = 0; /* Power Down off => enabled */
	sja1105_cgu_mii_control_packing(packed_buf, &mii_ext_rx_clk, PACK);

	return sja1105_spi_send_packed_buf(priv, SPI_WRITE, CGU_ADDR +
			mii_ext_rx_clk_offsets[port], packed_buf, BUF_LEN);
#undef BUF_LEN
}

static int mii_clocking_setup(struct sja1105_private *priv, int port, int mii_mode)
{
	struct device *dev = priv->ds->dev;
	int rc;

	if (mii_mode != XMII_MAC && mii_mode != XMII_PHY)
		goto error;

	dev_dbg(dev, "Configuring MII-%s clocking\n",
		  (mii_mode == XMII_MAC) ? "MAC" : "PHY");
	/*   * If mii_mode is MAC, disable IDIV
	 *   * If mii_mode is PHY, enable IDIV and configure for 1/1 divider
	 */
	rc = sja1105_cgu_idiv_config(priv, port, (mii_mode == XMII_PHY), 1);
	if (rc < 0)
		goto error;

	/* Configure CLKSRC of MII_TX_CLK_n
	 *   * If mii_mode is MAC, select TX_CLK_n
	 *   * If mii_mode is PHY, select IDIV_n
	 */
	rc = sja1105_cgu_mii_tx_clk_config(priv, port, mii_mode);
	if (rc < 0)
		goto error;

	/* Configure CLKSRC of MII_RX_CLK_n
	 * Select RX_CLK_n
	 */
	rc = sja1105_cgu_mii_rx_clk_config(priv, port);
	if (rc < 0)
		goto error;

	if (mii_mode == XMII_PHY) {
		/* In MII mode the PHY (which is us) drives the TX_CLK pin */

		/* Configure CLKSRC of EXT_TX_CLK_n
		 * Select IDIV_n
		 */
		rc = sja1105_cgu_mii_ext_tx_clk_config(priv, port);
		if (rc < 0)
			goto error;

		/* Configure CLKSRC of EXT_RX_CLK_n
		 * Select IDIV_n
		 */
		rc = sja1105_cgu_mii_ext_rx_clk_config(priv, port);
		if (rc < 0)
			goto error;
	}
	return 0;
error:
	return -1;
}

static void sja1105_cgu_pll_control_packing(void *buf,
			struct sja1105_cgu_pll_control *pll_control,
			enum packing_op op, u64 device_id)
{
	const int size = 4;

	if (op == UNPACK)
		memset(pll_control, 0, sizeof(*pll_control));
	else
		memset(buf, 0, size);

	sja1105_packing(buf, &pll_control->pllclksrc, 28, 24, size, op);
	sja1105_packing(buf, &pll_control->msel,      23, 16, size, op);
	sja1105_packing(buf, &pll_control->autoblock, 11, 11, size, op);
	sja1105_packing(buf, &pll_control->psel,       9,  8, size, op);
	sja1105_packing(buf, &pll_control->direct,     7,  7, size, op);
	sja1105_packing(buf, &pll_control->fbsel,      6,  6, size, op);
	sja1105_packing(buf, &pll_control->bypass,     1,  1, size, op);
	sja1105_packing(buf, &pll_control->pd,         0,  0, size, op);
	if (IS_PQRS(device_id)) {
		sja1105_packing(buf, &pll_control->nsel,
				13, 12, size, op);
		sja1105_packing(buf, &pll_control->p23en,
				2,  2, size, op);
	}
}

static int
sja1105_cgu_rgmii_tx_clk_config(struct sja1105_private *priv, int port, int speed)
{
#define BUF_LEN 4
	/* UM10944.pdf, Table 78, CGU Register overview */
	const int txc_offsets_et[] = {0x16, 0x1D, 0x24, 0x2B, 0x32};
	/* UM11040.pdf, Table 114, CGU Register overview */
	const int txc_offsets_pqrs[] = {0x16, 0x1C, 0x22, 0x28, 0x2E};
	struct sja1105_cgu_mii_control txc;
	const int *txc_offsets;
	u8 packed_buf[BUF_LEN];
	int clksrc;

	/* E/T and P/Q/R/S compatibility */
	if (IS_ET(priv->device_id))
		txc_offsets = txc_offsets_et;
	else
		txc_offsets = txc_offsets_pqrs;

	if (speed == SJA1105_SPEED_1000MBPS) {
		clksrc = CLKSRC_PLL0;
	} else {
		int clk_sources[] = {CLKSRC_IDIV0, CLKSRC_IDIV1, CLKSRC_IDIV2,
				     CLKSRC_IDIV3, CLKSRC_IDIV4};
		clksrc = clk_sources[port];
	}

	/* RGMII: 125MHz for 1000, 25MHz for 100, 2.5MHz for 10 */
	txc.clksrc = clksrc;
	/* Autoblock clk while changing clksrc */
	txc.autoblock = 1;
	/* Power Down off => enabled */
	txc.pd = 0;
	sja1105_cgu_mii_control_packing(packed_buf, &txc, PACK);

	return sja1105_spi_send_packed_buf(priv, SPI_WRITE, CGU_ADDR +
			txc_offsets[port], packed_buf, BUF_LEN);
#undef BUF_LEN
}

/* AGU */
static void sja1105_cfg_pad_mii_tx_packing(void *buf,
		struct sja1105_cfg_pad_mii_tx *pad_mii_tx, enum packing_op op)
{
	const int size = 4;

	if (op == UNPACK)
		memset(pad_mii_tx, 0, sizeof(*pad_mii_tx));
	else
		memset(buf, 0, size);

	sja1105_packing(buf, &pad_mii_tx->d32_os,   28, 27, size, op);
	sja1105_packing(buf, &pad_mii_tx->d32_ipud, 25, 24, size, op);
	sja1105_packing(buf, &pad_mii_tx->d10_os,   20, 19, size, op);
	sja1105_packing(buf, &pad_mii_tx->d10_ipud, 17, 16, size, op);
	sja1105_packing(buf, &pad_mii_tx->ctrl_os,  12, 11, size, op);
	sja1105_packing(buf, &pad_mii_tx->ctrl_ipud, 9,  8, size, op);
	sja1105_packing(buf, &pad_mii_tx->clk_os,    4,  3, size, op);
	sja1105_packing(buf, &pad_mii_tx->clk_ih,    2,  2, size, op);
	sja1105_packing(buf, &pad_mii_tx->clk_ipud,  1,  0, size, op);
}

static int sja1105_rgmii_cfg_pad_tx_config(struct sja1105_private *priv, int port)
{
#define BUF_LEN 4
	u8 packed_buf[BUF_LEN];
	/* UM10944.pdf, Table 86, ACU Register overview */
	int     pad_mii_tx_offsets[] = {0x00, 0x02, 0x04, 0x06, 0x08};
	struct  sja1105_cfg_pad_mii_tx pad_mii_tx;

	/* Payload */
	pad_mii_tx.d32_os    = 3; /* TXD[3:2] output stage: */
				  /*          high noise/high speed */
	pad_mii_tx.d32_ipud  = 2; /* TXD[3:2] input stage: */
				  /*          plain input (default) */
	pad_mii_tx.d10_os    = 3; /* TXD[1:0] output stage: */
				  /*          high noise/high speed */
	pad_mii_tx.d10_ipud  = 2; /* TXD[1:0] input stage: */
				  /*          plain input (default) */
	pad_mii_tx.ctrl_os   = 3; /* TX_CTL / TX_ER output stage */
	pad_mii_tx.ctrl_ipud = 2; /* TX_CTL / TX_ER input stage (default) */
	pad_mii_tx.clk_os    = 3; /* TX_CLK output stage */
	pad_mii_tx.clk_ih    = 0; /* TX_CLK input hysteresis (default) */
	pad_mii_tx.clk_ipud  = 2; /* TX_CLK input stage (default) */
	sja1105_cfg_pad_mii_tx_packing(packed_buf, &pad_mii_tx, PACK);

	return sja1105_spi_send_packed_buf(priv, SPI_WRITE, AGU_ADDR +
			pad_mii_tx_offsets[port], packed_buf, BUF_LEN);
#undef BUF_LEN
}

static int rgmii_clocking_setup(struct sja1105_private *priv, int port)
{
	struct device *dev = priv->ds->dev;
	struct sja1105_table *mac;
	int speed;
	int rc;

	mac = &priv->static_config.tables[BLK_IDX_MAC_CONFIG];
	speed = ((struct sja1105_mac_config_entry *) mac->entries)[port].speed;

	dev_dbg(dev, "Configuring port %d RGMII at speed %dMbps\n",
		port, speed);

	switch (speed) {
	case SJA1105_SPEED_1000MBPS:
		/* 1000Mbps, IDIV disabled, divide by 1 */
		rc = sja1105_cgu_idiv_config(priv, port, false, 1);
		break;
	case SJA1105_SPEED_100MBPS:
		/* 100Mbps, IDIV enabled, divide by 1 */
		rc = sja1105_cgu_idiv_config(priv, port, true, 1);
		break;
	case SJA1105_SPEED_10MBPS:
		/* 10Mbps, IDIV enabled, divide by 10 */
		rc = sja1105_cgu_idiv_config(priv, port, true, 10);
		break;
	case SJA1105_SPEED_AUTO:
		/* Skip CGU configuration if there is no speed available
		 * (e.g. link is not established yet)
		 */
		dev_dbg(dev, "Speed not available, skipping CGU config\n");
		rc = 0;
		goto out;
	default:
		rc = -EINVAL;
	}

	if (rc < 0) {
		dev_err(dev, "Failed to configure idiv\n");
		goto out;
	}
	rc = sja1105_cgu_rgmii_tx_clk_config(priv, port, speed);
	if (rc < 0) {
		dev_err(dev, "Failed to configure RGMII Tx clock\n");
		goto out;
	}
	rc = sja1105_rgmii_cfg_pad_tx_config(priv, port);
	if (rc < 0) {
		dev_err(dev, "Failed to configure Tx pad registers\n");
		goto out;
	}
out:
	return rc;
}

static int sja1105_cgu_rmii_ref_clk_config(struct sja1105_private *priv, int port)
{
#define BUF_LEN 4
	struct  sja1105_cgu_mii_control ref_clk;
	u8 packed_buf[BUF_LEN];
	/* UM10944.pdf, Table 78, CGU Register overview */
	const int ref_clk_offsets_et[] = {0x15, 0x1C, 0x23, 0x2A, 0x31};
	/* UM11040.pdf, Table 114, CGU Register overview */
	const int ref_clk_offsets_pqrs[] = {0x15, 0x1B, 0x21, 0x27, 0x2D};
	const int *ref_clk_offsets;
	const int clk_sources[] = {
		CLKSRC_MII0_TX_CLK,
		CLKSRC_MII1_TX_CLK,
		CLKSRC_MII2_TX_CLK,
		CLKSRC_MII3_TX_CLK,
		CLKSRC_MII4_TX_CLK,
	};

	/* E/T and P/Q/R/S compatibility */
	if (IS_ET(priv->device_id))
		ref_clk_offsets = ref_clk_offsets_et;
	else
		ref_clk_offsets = ref_clk_offsets_pqrs;

	/* Payload for packed_buf */
	ref_clk.clksrc    = clk_sources[port];
	ref_clk.autoblock = 1;      /* Autoblock clk while changing clksrc */
	ref_clk.pd        = 0;      /* Power Down off => enabled */
	sja1105_cgu_mii_control_packing(packed_buf, &ref_clk, PACK);

	return sja1105_spi_send_packed_buf(priv, SPI_WRITE, CGU_ADDR +
			ref_clk_offsets[port], packed_buf, BUF_LEN);
#undef BUF_LEN
}

static int
sja1105_cgu_rmii_ext_tx_clk_config(struct sja1105_private *priv, int port)
{
#define BUF_LEN 4
	struct sja1105_cgu_mii_control ext_tx_clk;
	u8 packed_buf[BUF_LEN];
	/* UM10944.pdf, Table 78, CGU Register overview */
	const int ext_tx_clk_offsets_et[] = {0x18, 0x1F, 0x26, 0x2D, 0x34};
	/* UM11040.pdf, Table 114, CGU Register overview */
	const int ext_tx_clk_offsets_pqrs[] = {0x17, 0x1D, 0x23, 0x29, 0x2F};
	const int *ext_tx_clk_offsets;

	/* E/T and P/Q/R/S compatibility */
	if (IS_ET(priv->device_id))
		ext_tx_clk_offsets = ext_tx_clk_offsets_et;
	else
		ext_tx_clk_offsets = ext_tx_clk_offsets_pqrs;

	/* Payload for packed_buf */
	ext_tx_clk.clksrc    = CLKSRC_PLL1;
	ext_tx_clk.autoblock = 1;   /* Autoblock clk while changing clksrc */
	ext_tx_clk.pd        = 0;   /* Power Down off => enabled */
	sja1105_cgu_mii_control_packing(packed_buf, &ext_tx_clk, PACK);

	return sja1105_spi_send_packed_buf(priv, SPI_WRITE, CGU_ADDR +
			ext_tx_clk_offsets[port], packed_buf, BUF_LEN);
#undef BUF_LEN
}

static int sja1105_cgu_rmii_pll_config(struct sja1105_private *priv)
{
#define BUF_LEN 4
	struct device *dev = priv->ds->dev;
	struct sja1105_cgu_pll_control pll;
	const int PLL1_OFFSET = 0x0A;
	u8 packed_buf[BUF_LEN];
	int rc;

	/* PLL1 must be enabled and output 50 Mhz.
	 * This is done by writing first 0x0A010941 to
	 * the PLL_1_C register and then deasserting
	 * power down (PD) 0x0A010940. */

	/* Step 1: PLL1 setup for 50Mhz */
	pll.pllclksrc = 0xA;
	pll.msel      = 0x1;
	pll.autoblock = 0x1;
	pll.psel      = 0x1;
	pll.direct    = 0x0;
	pll.fbsel     = 0x1;
	pll.bypass    = 0x0;
	pll.pd        = 0x1;
	/* P/Q/R/S only */
	pll.nsel      = 0x0; /* PLL pre-divider is 1 (nsel + 1) */
	pll.p23en     = 0x0; /* disable 120 and 240 degree phase PLL outputs */

	sja1105_cgu_pll_control_packing(packed_buf, &pll, priv->device_id, PACK);
	rc = sja1105_spi_send_packed_buf(priv, SPI_WRITE, CGU_ADDR +
				PLL1_OFFSET, packed_buf, BUF_LEN);
	if (rc < 0) {
		dev_err(dev, "failed to configure PLL1 for 50MHz\n");
		goto out;
	}

	/* Step 2: Enable PLL1 */
	pll.pd = 0x0;

	sja1105_cgu_pll_control_packing(packed_buf, &pll, priv->device_id, PACK);
	rc = sja1105_spi_send_packed_buf(priv, SPI_WRITE, CGU_ADDR +
				PLL1_OFFSET, packed_buf, BUF_LEN);
	if (rc < 0) {
		dev_err(dev, "failed to enable PLL1\n");
		goto out;
	}
out:
	return rc;
#undef BUF_LEN
}

static int rmii_clocking_setup(struct sja1105_private *priv, int port, int rmii_mode)
{
	struct device *dev = priv->ds->dev;
	int rc;

	if (rmii_mode != XMII_MAC && rmii_mode != XMII_PHY) {
		dev_err(dev, "RMII mode must either be MAC or PHY\n");
		return -EINVAL;
	}
	dev_dbg(dev, "Configuring RMII-%s clocking\n",
		  (rmii_mode == XMII_MAC) ? "MAC" : "PHY");
	/* AH1601.pdf chapter 2.5.1. Sources */
	if (rmii_mode == XMII_MAC) {
		/* Configure and enable PLL1 for 50Mhz output */
		rc = sja1105_cgu_rmii_pll_config(priv);
		if (rc < 0)
			return rc;
	}
	/* Disable IDIV for this port */
	rc = sja1105_cgu_idiv_config(priv, port, false, 1);
	if (rc < 0)
		return rc;
	/* Source to sink mappings */
	rc = sja1105_cgu_rmii_ref_clk_config(priv, port);
	if (rc < 0)
		return rc;
	if (rmii_mode == XMII_MAC) {
		rc = sja1105_cgu_rmii_ext_tx_clk_config(priv, port);
		if (rc < 0)
			return rc;
	}
	return 0;
}

/* TODO:
 * Standard clause 22 registers for the internal SGMII PCS are
 * memory-mapped starting at SPI address 0x1F0000.
 * The SGMII port should already have a basic initialization done
 * through the static configuration tables.
 * If any further SGMII initialization steps (autonegotiation or checking the
 * link status) need to be done, they might as well be added here.
 */
static int sgmii_clocking_setup(struct sja1105_private *priv, int port)
{
	struct device *dev = priv->ds->dev;

	dev_err(dev, "TODO: Configure SGMII clocking\n");
	return 0;
}

int sja1105_clocking_setup_port(struct sja1105_private *priv, int port)
{
	struct sja1105_xmii_params_entry *mii;
	struct device *dev = priv->ds->dev;
	int rc = 0;

	mii = priv->static_config.tables[BLK_IDX_XMII_PARAMS].entries;

	switch (mii->xmii_mode[port]) {
	case XMII_MODE_MII:
		rc = mii_clocking_setup(priv, port, mii->phy_mac[port]);
		break;
	case XMII_MODE_RMII:
		rc = rmii_clocking_setup(priv, port, mii->phy_mac[port]);
		break;
	case XMII_MODE_RGMII:
		rc = rgmii_clocking_setup(priv, port);
		break;
	case XMII_MODE_SGMII:
		if (!IS_PQRS(priv->device_id)) {
			dev_err(dev, "SGMII mode not supported!\n");
			rc = -EINVAL;
			goto out;
		}
		if ((IS_R(priv->device_id, priv->part_nr) ||
		     IS_S(priv->device_id, priv->part_nr)) &&
		    (port == 4))
			rc = sgmii_clocking_setup(priv, port);
		else
			dev_info(dev, "port is tri-stated\n");
		break;
	default:
		dev_err(dev, "Invalid MII mode specified: %llx\n",
			mii->xmii_mode[port]);
		rc = -EINVAL;
	}
out:
	if (rc)
		dev_err(dev, "Clocking setup for port %d failed: %d\n",
			port, rc);
	return rc;
}

int sja1105_clocking_setup(struct sja1105_private *priv)
{
	int port, rc;

	for (port = 0; port < SJA1105_NUM_PORTS; port++)
		if ((rc = sja1105_clocking_setup_port(priv, port)) < 0)
			return rc;

	return 0;
}

