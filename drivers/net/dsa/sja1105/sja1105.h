// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, Sensor-Technik Wiedemann GmbH
 * Copyright (c) 2018-2019, Vladimir Oltean <olteanv@gmail.com>
 */
#ifndef _SJA1105_H
#define _SJA1105_H

#include <net/dsa.h>
#include "sja1105_static_config.h"
#include <linux/dsa/sja1105.h>

#define SJA1105_NUM_PORTS 5
#define SJA1105_NUM_TC    8
#define SJA1105ET_FDB_BIN_SIZE 4

struct sja1105_port {
	struct dsa_port *dp;
	struct work_struct xmit_work;
	struct sja1105_skb_ring xmit_ring;
};

struct sja1105_private {
	const struct sja1105_dynamic_table_ops *dyn_ops;
	struct sja1105_static_config static_config;
	struct gpio_desc *reset_gpio;
	struct spi_device *spidev;
	struct dsa_switch *ds;
	u64 device_id;
	u64 part_nr; /* Needed for P/R distinction (same switch core) */
	struct sja1105_port ports[SJA1105_NUM_PORTS];
};

#include "sja1105_dynamic_config.h"

struct sja1105_spi_message {
	u64 access;
	u64 read_count;
	u64 address;
};

enum sja1105_spi_access_mode {
	SPI_READ = 0,
	SPI_WRITE = 1,
};

/* From sja1105-spi.c */
int sja1105_spi_send_packed_buf(const struct sja1105_private *,
		enum sja1105_spi_access_mode, u64, void *, size_t);
int sja1105_spi_send_int(const struct sja1105_private *,
		enum sja1105_spi_access_mode, u64, u64 *, u64);
int sja1105_spi_send_long_packed_buf(const struct sja1105_private *,
		enum sja1105_spi_access_mode, u64, void *, u64);
int sja1105_static_config_upload(struct sja1105_private *priv);
int sja1105_device_id_get(struct sja1105_private *priv);
const char *sja1105_device_id_string_get(u64 device_id, u64 part_nr);

#define CORE_ADDR   0x000000
#define CONFIG_ADDR 0x020000
#define CGU_ADDR    0x100000
#define RGU_ADDR    0x100440
#define AGU_ADDR    0x100800
#define ACU_ADDR    0x100800

#define SIZE_SPI_MSG_HEADER    4
#define SIZE_SPI_MSG_MAXLEN    (64 * 4)

/* From sja1105-clocking.c */

#define XMII_MAC               0ull
#define XMII_PHY               1ull
#define XMII_MODE_MII          0ull
#define XMII_MODE_RMII         1ull
#define XMII_MODE_RGMII        2ull
#define XMII_MODE_SGMII        3ull /* Only available for port 4 on R/S */
#define XMII_MODE_TRISTATE     3ull

#define SJA1105_SPEED_10MBPS   3ull
#define SJA1105_SPEED_100MBPS  2ull
#define SJA1105_SPEED_1000MBPS 1ull
#define SJA1105_SPEED_AUTO     0ull

int sja1105_clocking_setup_port(struct sja1105_private *priv, int port);
int sja1105_clocking_setup(struct sja1105_private *priv);

/* From sja1105-ethtool.c */
void sja1105_get_ethtool_stats(struct dsa_switch *ds, int port, u64 *data);
void sja1105_get_strings(struct dsa_switch *ds, int port,
			 u32 stringset, u8 *data);
int sja1105_get_sset_count(struct dsa_switch *ds, int port, int sset);

/* From sja1105-dynamic-config.c */

int sja1105_dynamic_config_read(struct sja1105_private *priv,
				enum sja1105_blk_idx blk_idx,
				int index, void *entry);
int sja1105_dynamic_config_write(struct sja1105_private *priv,
				 enum sja1105_blk_idx blk_idx,
				 int index, void *entry, bool keep);
int sja1105_dynamic_config_init(struct sja1105_private *priv);

u8 sja1105_fdb_hash(struct sja1105_private *priv, const u8 *addr, u16 vid);

/* Common implementations for the static and dynamic configs */
size_t sja1105_l2_forwarding_entry_packing(void *buf, void *entry_ptr,
					   enum packing_op op);
size_t sja1105pqrs_l2_lookup_entry_packing(void *buf, void *entry_ptr,
					   enum packing_op op);
size_t sja1105et_l2_lookup_entry_packing(void *buf, void *entry_ptr,
					 enum packing_op op);
size_t sja1105_vlan_lookup_entry_packing(void *buf, void *entry_ptr,
					 enum packing_op op);
size_t sja1105_retagging_entry_packing(void *buf, void *entry_ptr,
				       enum packing_op op);
size_t sja1105pqrs_mac_config_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op);
size_t sja1105_vl_lookup_entry_packing(void *buf, void *entry_ptr,
				       enum packing_op op);

#endif

