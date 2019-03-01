// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2019, Vladimir Oltean <olteanv@gmail.com>
 */
#include "sja1105.h"

struct sja1105_port_status_mac {
	u64 n_runt;
	u64 n_soferr;
	u64 n_alignerr;
	u64 n_miierr;
	u64 typeerr;
	u64 sizeerr;
	u64 tctimeout;
	u64 priorerr;
	u64 nomaster;
	u64 memov;
	u64 memerr;
	u64 invtyp;
	u64 intcyov;
	u64 domerr;
	u64 pcfbagdrop;
	u64 spcprior;
	u64 ageprior;
	u64 portdrop;
	u64 lendrop;
	u64 bagdrop;
	u64 policeerr;
	u64 drpnona664err;
	u64 spcerr;
	u64 agedrp;
};

struct sja1105_port_status_hl1 {
	u64 n_n664err;
	u64 n_vlanerr;
	u64 n_unreleased;
	u64 n_sizeerr;
	u64 n_crcerr;
	u64 n_vlnotfound;
	u64 n_ctpolerr;
	u64 n_polerr;
	u64 n_rxfrmsh;
	u64 n_rxfrm;
	u64 n_rxbytesh;
	u64 n_rxbyte;
	u64 n_txfrmsh;
	u64 n_txfrm;
	u64 n_txbytesh;
	u64 n_txbyte;
};

struct sja1105_port_status_hl2 {
	u64 n_qfull;
	u64 n_part_drop;
	u64 n_egr_disabled;
	u64 n_not_reach;
	u64 qlevel_hwm[8]; /* Only for P/Q/R/S */
	u64 qlevel[8];     /* Only for P/Q/R/S */
};

struct sja1105_port_status {
	struct sja1105_port_status_mac mac;
	struct sja1105_port_status_hl1 hl1;
	struct sja1105_port_status_hl2 hl2;
};

static void sja1105_port_status_mac_unpack(void *buf,
		struct sja1105_port_status_mac *status)
{
	/* So that additions translate to 4 bytes */
	u32 *p = (u32 *) buf;

	sja1105_unpack(p + 0x0, &status->n_runt,       31, 24, 4);
	sja1105_unpack(p + 0x0, &status->n_soferr,     23, 16, 4);
	sja1105_unpack(p + 0x0, &status->n_alignerr,   15,  8, 4);
	sja1105_unpack(p + 0x0, &status->n_miierr,      7,  0, 4);
	sja1105_unpack(p + 0x1, &status->typeerr,      27, 27, 4);
	sja1105_unpack(p + 0x1, &status->sizeerr,      26, 26, 4);
	sja1105_unpack(p + 0x1, &status->tctimeout,    25, 25, 4);
	sja1105_unpack(p + 0x1, &status->priorerr,     24, 24, 4);
	sja1105_unpack(p + 0x1, &status->nomaster,     23, 23, 4);
	sja1105_unpack(p + 0x1, &status->memov,        22, 22, 4);
	sja1105_unpack(p + 0x1, &status->memerr,       21, 21, 4);
	sja1105_unpack(p + 0x1, &status->invtyp,       19, 19, 4);
	sja1105_unpack(p + 0x1, &status->intcyov,      18, 18, 4);
	sja1105_unpack(p + 0x1, &status->domerr,       17, 17, 4);
	sja1105_unpack(p + 0x1, &status->pcfbagdrop,   16, 16, 4);
	sja1105_unpack(p + 0x1, &status->spcprior,     15, 12, 4);
	sja1105_unpack(p + 0x1, &status->ageprior,     11,  8, 4);
	sja1105_unpack(p + 0x1, &status->portdrop,      6,  6, 4);
	sja1105_unpack(p + 0x1, &status->lendrop,       5,  5, 4);
	sja1105_unpack(p + 0x1, &status->bagdrop,       4,  4, 4);
	sja1105_unpack(p + 0x1, &status->policeerr,     3,  3, 4);
	sja1105_unpack(p + 0x1, &status->drpnona664err, 2,  2, 4);
	sja1105_unpack(p + 0x1, &status->spcerr,        1,  1, 4);
	sja1105_unpack(p + 0x1, &status->agedrp,        0,  0, 4);
}

static void sja1105_port_status_hl1_unpack(void *buf,
				struct sja1105_port_status_hl1 *status)
{
	/* So that additions translate to 4 bytes */
	u32 *p = (u32 *) buf;

	sja1105_unpack(p + 0xF, &status->n_n664err,    31,  0, 4);
	sja1105_unpack(p + 0xE, &status->n_vlanerr,    31,  0, 4);
	sja1105_unpack(p + 0xD, &status->n_unreleased, 31,  0, 4);
	sja1105_unpack(p + 0xC, &status->n_sizeerr,    31,  0, 4);
	sja1105_unpack(p + 0xB, &status->n_crcerr,     31,  0, 4);
	sja1105_unpack(p + 0xA, &status->n_vlnotfound, 31,  0, 4);
	sja1105_unpack(p + 0x9, &status->n_ctpolerr,   31,  0, 4);
	sja1105_unpack(p + 0x8, &status->n_polerr,     31,  0, 4);
	sja1105_unpack(p + 0x7, &status->n_rxfrmsh,    31,  0, 4);
	sja1105_unpack(p + 0x6, &status->n_rxfrm,      31,  0, 4);
	sja1105_unpack(p + 0x5, &status->n_rxbytesh,   31,  0, 4);
	sja1105_unpack(p + 0x4, &status->n_rxbyte,     31,  0, 4);
	sja1105_unpack(p + 0x3, &status->n_txfrmsh,    31,  0, 4);
	sja1105_unpack(p + 0x2, &status->n_txfrm,      31,  0, 4);
	sja1105_unpack(p + 0x1, &status->n_txbytesh,   31,  0, 4);
	sja1105_unpack(p + 0x0, &status->n_txbyte,     31,  0, 4);
	status->n_rxfrm  += status->n_rxfrmsh  << 32;
	status->n_rxbyte += status->n_rxbytesh << 32;
	status->n_txfrm  += status->n_txfrmsh  << 32;
	status->n_txbyte += status->n_txbytesh << 32;
}

static void sja1105_port_status_hl2_unpack(void *buf,
			struct sja1105_port_status_hl2 *status)
{
	/* So that additions translate to 4 bytes */
	u32 *p = (u32 *) buf;

	sja1105_unpack(p + 0x3, &status->n_qfull,        31,  0, 4);
	sja1105_unpack(p + 0x2, &status->n_part_drop,    31,  0, 4);
	sja1105_unpack(p + 0x1, &status->n_egr_disabled, 31,  0, 4);
	sja1105_unpack(p + 0x0, &status->n_not_reach,    31,  0, 4);
}

static void sja1105pqrs_port_status_qlevel_unpack(void *buf,
				struct sja1105_port_status_hl2 *status)
{
	/* So that additions translate to 4 bytes */
	u32 *p = (u32 *) buf;
	int i;

	for (i = 0; i < 8; i++) {
		sja1105_unpack(p + i, &status->qlevel_hwm[i], 24, 16, 4);
		sja1105_unpack(p + i, &status->qlevel[i],      8,  0, 4);
	}
}

int sja1105_port_status_get_mac(struct sja1105_private *priv,
				struct sja1105_port_status_mac *status,
				int port)
{
#define SIZE_MAC_AREA (0x02 * 4)
	const u64 mac_base_addr[] = {0x200, 0x202, 0x204, 0x206, 0x208};
	u8 packed_buf[SIZE_MAC_AREA];
	int rc = 0;

	memset(status, 0, sizeof(*status));

	/* MAC area */
	rc = sja1105_spi_send_packed_buf(priv, SPI_READ, CORE_ADDR +
			mac_base_addr[port], packed_buf, SIZE_MAC_AREA);
	if (rc < 0)
		return rc;

	sja1105_port_status_mac_unpack(packed_buf, status);

	return 0;
#undef SIZE_MAC_AREA
}

static int sja1105_port_status_get_hl1(struct sja1105_private *priv,
				       struct sja1105_port_status_hl1 *status,
				       int port)
{
#define SIZE_HL1_AREA (0x10 * 4)
	const u64 high_level_1_base_addr[] = {
		0x400, 0x410, 0x420, 0x430, 0x440 };
	u8 packed_buf[SIZE_HL1_AREA];
	int rc = 0;

	memset(status, 0, sizeof(*status));

	rc = sja1105_spi_send_packed_buf(priv, SPI_READ, CORE_ADDR +
		  high_level_1_base_addr[port], packed_buf, SIZE_HL1_AREA);
	if (rc < 0)
		return rc;

	sja1105_port_status_hl1_unpack(packed_buf, status);

	return 0;
#undef SIZE_HL1_AREA
}

int sja1105_port_status_get_hl2(struct sja1105_private *priv,
				struct sja1105_port_status_hl2 *status,
				int port)
{
#define SIZE_HL2_AREA (0x4 * 4)
#define SIZE_QLEVEL_AREA (0x8 * 4) /* 0x4 to 0xB */
	const u64 high_level_2_base_addr[] = {
		0x600, 0x610, 0x620, 0x630, 0x640};
	const u64 qlevel_base_addr[] = {
		0x604, 0x614, 0x624, 0x634, 0x644};
	u8 packed_buf[SIZE_QLEVEL_AREA];
	int rc = 0;

	memset(status, 0, sizeof(*status));

	rc = sja1105_spi_send_packed_buf(priv, SPI_READ, CORE_ADDR +
		  high_level_2_base_addr[port], packed_buf, SIZE_HL2_AREA);
	if (rc < 0)
		return rc;

	sja1105_port_status_hl2_unpack(packed_buf, status);

	if (IS_ET(priv->device_id))
		/* Code below is strictly P/Q/R/S specific. */
		return 0;

	rc = sja1105_spi_send_packed_buf(priv, SPI_READ, CORE_ADDR +
			qlevel_base_addr[port], packed_buf, SIZE_QLEVEL_AREA);
	if (rc < 0)
		return rc;

	sja1105pqrs_port_status_qlevel_unpack(packed_buf, status);

	return 0;
#undef SIZE_QLEVEL_AREA
#undef SIZE_HL2_AREA
}

static int sja1105_port_status_get(struct sja1105_private *priv,
				   struct sja1105_port_status *status,
				   int port)
{
	int rc;

	if ((rc = sja1105_port_status_get_mac(priv, &status->mac, port)) < 0)
		return rc;
	if ((rc = sja1105_port_status_get_hl1(priv, &status->hl1, port)) < 0)
		return rc;
	if ((rc = sja1105_port_status_get_hl2(priv, &status->hl2, port)) < 0)
		return rc;

	return 0;
}

static char sja1105_port_stats[][ETH_GSTRING_LEN] = {
	/* MAC-Level Diagnostic Counters */
	"n_runt",
	"n_soferr",
	"n_alignerr",
	"n_miierr",
	/* MAC-Level Diagnostic Flags */
	"typeerr",
	"sizeerr",
	"tctimeout",
	"priorerr",
	"nomaster",
	"memov",
	"memerr",
	"invtyp",
	"intcyov",
	"domerr",
	"pcfbagdrop",
	"spcprior",
	"ageprior",
	"portdrop",
	"lendrop",
	"bagdrop",
	"policeerr",
	"drpnona664err",
	"spcerr",
	"agedrp",
	/* High-Level Diagnostic Counters */
	"n_n664err",
	"n_vlanerr",
	"n_unreleased",
	"n_sizeerr",
	"n_crcerr",
	"n_vlnotfound",
	"n_ctpolerr",
	"n_polerr",
	"n_rxfrm",
	"n_rxbyte",
	"n_txfrm",
	"n_txbyte",
	"n_qfull",
	"n_part_drop",
	"n_egr_disabled",
	"n_not_reach",
};

static char sja1105pqrs_extra_port_stats[][ETH_GSTRING_LEN] = {
	/* Queue Levels */
	"qlevel_hwm_0",
	"qlevel_hwm_1",
	"qlevel_hwm_2",
	"qlevel_hwm_3",
	"qlevel_hwm_4",
	"qlevel_hwm_5",
	"qlevel_hwm_6",
	"qlevel_hwm_7",
	"qlevel_0",
	"qlevel_1",
	"qlevel_2",
	"qlevel_3",
	"qlevel_4",
	"qlevel_5",
	"qlevel_6",
	"qlevel_7",
};

void sja1105_get_ethtool_stats(struct dsa_switch *ds, int port, u64 *data)
{
	struct sja1105_private *priv = ds->priv;
	struct sja1105_port_status status;
	int i, k = 0;
	int rc;

	if ((rc = sja1105_port_status_get(priv, &status, port)) < 0) {
		dev_err(ds->dev, "Failed to read port %d counters: %d\n",
			port, rc);
		return;
	}
	memset(data, 0, ARRAY_SIZE(sja1105_port_stats) * sizeof(u64));
	data[k++] = status.mac.n_runt;
	data[k++] = status.mac.n_soferr;
	data[k++] = status.mac.n_alignerr;
	data[k++] = status.mac.n_miierr;
	data[k++] = status.mac.typeerr;
	data[k++] = status.mac.sizeerr;
	data[k++] = status.mac.tctimeout;
	data[k++] = status.mac.priorerr;
	data[k++] = status.mac.nomaster;
	data[k++] = status.mac.memov;
	data[k++] = status.mac.memerr;
	data[k++] = status.mac.invtyp;
	data[k++] = status.mac.intcyov;
	data[k++] = status.mac.domerr;
	data[k++] = status.mac.pcfbagdrop;
	data[k++] = status.mac.spcprior;
	data[k++] = status.mac.ageprior;
	data[k++] = status.mac.portdrop;
	data[k++] = status.mac.lendrop;
	data[k++] = status.mac.bagdrop;
	data[k++] = status.mac.policeerr;
	data[k++] = status.mac.drpnona664err;
	data[k++] = status.mac.spcerr;
	data[k++] = status.mac.agedrp;
	data[k++] = status.hl1.n_n664err;
	data[k++] = status.hl1.n_vlanerr;
	data[k++] = status.hl1.n_unreleased;
	data[k++] = status.hl1.n_sizeerr;
	data[k++] = status.hl1.n_crcerr;
	data[k++] = status.hl1.n_vlnotfound;
	data[k++] = status.hl1.n_ctpolerr;
	data[k++] = status.hl1.n_polerr;
	data[k++] = status.hl1.n_rxfrm;
	data[k++] = status.hl1.n_rxbyte;
	data[k++] = status.hl1.n_txfrm;
	data[k++] = status.hl1.n_txbyte;
	data[k++] = status.hl2.n_qfull;
	data[k++] = status.hl2.n_part_drop;
	data[k++] = status.hl2.n_egr_disabled;
	data[k++] = status.hl2.n_not_reach;

	if (!IS_PQRS(priv->device_id))
		return;

	memset(data + k, 0, ARRAY_SIZE(sja1105pqrs_extra_port_stats) *
			sizeof(u64));
	for (i = 0; i < 8; i++) {
		data[k++] = status.hl2.qlevel_hwm[i];
		data[k++] = status.hl2.qlevel[i];
	}
}

void sja1105_get_strings(struct dsa_switch *ds, int port,
			 u32 stringset, u8 *data)
{
	struct sja1105_private *priv = ds->priv;
	u8 *p = data;
	int i;

	switch (stringset) {
	case ETH_SS_STATS:
		for (i = 0; i < ARRAY_SIZE(sja1105_port_stats); i++) {
			strlcpy(p, sja1105_port_stats[i], ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
		}
		if (!IS_PQRS(priv->device_id))
			return;
		for (i = 0; i < ARRAY_SIZE(sja1105pqrs_extra_port_stats); i++) {
			strlcpy(p, sja1105pqrs_extra_port_stats[i],
						ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
		}
		break;
	}
}

int sja1105_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	struct sja1105_private *priv = ds->priv;
	int count = ARRAY_SIZE(sja1105_port_stats);

	if (IS_PQRS(priv->device_id))
		count += ARRAY_SIZE(sja1105pqrs_extra_port_stats);

	return count;
}

