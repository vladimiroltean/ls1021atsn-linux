/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2016-2018, NXP Semiconductors
 * Copyright (c) 2018-2019, Vladimir Oltean <olteanv@gmail.com>
 */
#ifndef _SJA1105_STATIC_CONFIG_H
#define _SJA1105_STATIC_CONFIG_H

#include <linux/packing.h>

#define SIZE_SJA1105_DEVICE_ID                  4
#define SIZE_TABLE_HEADER                       12
#define SIZE_L2_LOOKUP_ENTRY_ET                 12
#define SIZE_L2_LOOKUP_ENTRY_PQRS               20
#define SIZE_L2_POLICING_ENTRY                  8
#define SIZE_VLAN_LOOKUP_ENTRY                  8
#define SIZE_L2_FORWARDING_ENTRY                8
#define SIZE_MAC_CONFIG_ENTRY_ET                28
#define SIZE_MAC_CONFIG_ENTRY_PQRS              32
#define SIZE_L2_LOOKUP_PARAMS_ENTRY_ET          4
#define SIZE_L2_LOOKUP_PARAMS_ENTRY_PQRS        16
#define SIZE_L2_FORWARDING_PARAMS_ENTRY         12
#define SIZE_GENERAL_PARAMS_ENTRY_ET            40
#define SIZE_GENERAL_PARAMS_ENTRY_PQRS          44
#define SIZE_XMII_PARAMS_ENTRY                  4
#define SIZE_SGMII_ENTRY                        144

/* UM10944.pdf Page 11, Table 2. Configuration Blocks */
#define BLKID_L2_LOOKUP                    0x05
#define BLKID_L2_POLICING                  0x06
#define BLKID_VLAN_LOOKUP                  0x07
#define BLKID_L2_FORWARDING                0x08
#define BLKID_MAC_CONFIG                   0x09
#define BLKID_L2_LOOKUP_PARAMS             0x0D
#define BLKID_L2_FORWARDING_PARAMS         0x0E
#define BLKID_GENERAL_PARAMS               0x11
#define BLKID_XMII_PARAMS                  0x4E
#define BLKID_SGMII                        0xC8
#define BLKID_MAX                          BLKID_SGMII

enum sja1105_blk_idx {
	BLK_IDX_L2_LOOKUP = 0,
	BLK_IDX_L2_POLICING,
	BLK_IDX_VLAN_LOOKUP,
	BLK_IDX_L2_FORWARDING,
	BLK_IDX_MAC_CONFIG,
	BLK_IDX_L2_LOOKUP_PARAMS,
	BLK_IDX_L2_FORWARDING_PARAMS,
	BLK_IDX_GENERAL_PARAMS,
	BLK_IDX_XMII_PARAMS,
	BLK_IDX_SGMII,
	BLK_IDX_MAX,
	/* Fake block indices that are only valid for dynamic access */
	BLK_IDX_MGMT_ROUTE,
	BLK_IDX_MAX_DYN,
	BLK_IDX_INVAL = -1,
};

#define MAX_L2_LOOKUP_COUNT                      1024
#define MAX_L2_POLICING_COUNT                    45
#define MAX_VLAN_LOOKUP_COUNT                    4096
#define MAX_L2_FORWARDING_COUNT                  13
#define MAX_MAC_CONFIG_COUNT                     5
#define MAX_L2_LOOKUP_PARAMS_COUNT               1
#define MAX_L2_FORWARDING_PARAMS_COUNT           1
#define MAX_GENERAL_PARAMS_COUNT                 1
#define MAX_XMII_PARAMS_COUNT                    1
#define MAX_SGMII_COUNT                          1

#define MAX_FRAME_MEMORY                         929
#define MAX_FRAME_MEMORY_RETAGGING               910

#define SJA1105E_DEVICE_ID         0x9C00000Cull
#define SJA1105T_DEVICE_ID         0x9E00030Eull
#define SJA1105PR_DEVICE_ID        0xAF00030Eull
#define SJA1105QS_DEVICE_ID        0xAE00030Eull
#define SJA1105_NO_DEVICE_ID       0x00000000ull

#define SJA1105P_PART_NR           0x9A84
#define SJA1105Q_PART_NR           0x9A85
#define SJA1105R_PART_NR           0x9A86
#define SJA1105S_PART_NR           0x9A87
#define SJA1105_PART_NR_DONT_CARE  0xFFFF

#define IS_PQRS(device_id) \
	(((device_id) == SJA1105PR_DEVICE_ID) || \
	 ((device_id) == SJA1105QS_DEVICE_ID))
#define IS_ET(device_id) \
	(((device_id) == SJA1105E_DEVICE_ID) || \
	 ((device_id) == SJA1105T_DEVICE_ID))
/* P and R have same Device ID, and differ by Part Number */
#define IS_P(device_id, part_nr) \
	(((device_id) == SJA1105PR_DEVICE_ID) && \
	 ((part_nr) == SJA1105P_PART_NR))
#define IS_R(device_id, part_nr) \
	(((device_id) == SJA1105PR_DEVICE_ID) && \
	 ((part_nr) == SJA1105R_PART_NR))
/* Same do Q and S */
#define IS_Q(device_id, part_nr) \
	(((device_id) == SJA1105QS_DEVICE_ID) && \
	 ((part_nr) == SJA1105Q_PART_NR))
#define IS_S(device_id, part_nr) \
	(((device_id) == SJA1105QS_DEVICE_ID) && \
	 ((part_nr) == SJA1105S_PART_NR))
#define DEVICE_ID_VALID(device_id) \
	(IS_ET(device_id) || IS_PQRS(device_id))

#ifdef __KERNEL__
#include <asm/types.h>
#include <linux/types.h>
#endif

struct sja1105_general_params_entry {
	u64 vllupformat;
	u64 mirr_ptacu;
	u64 switchid;
	u64 hostprio;
	u64 mac_fltres1;
	u64 mac_fltres0;
	u64 mac_flt1;
	u64 mac_flt0;
	u64 incl_srcpt1;
	u64 incl_srcpt0;
	u64 send_meta1;
	u64 send_meta0;
	u64 casc_port;
	u64 host_port;
	u64 mirr_port;
	u64 vlmarker;
	u64 vlmask;
	u64 tpid;
	u64 ignore2stf;
	u64 tpid2;
	/* P/Q/R/S only */
	u64 queue_ts;
	u64 egrmirrvid;
	u64 egrmirrpcp;
	u64 egrmirrdei;
	u64 replay_port;
};

struct sja1105_vlan_lookup_entry {
	u64 ving_mirr;
	u64 vegr_mirr;
	u64 vmemb_port;
	u64 vlan_bc;
	u64 tag_port;
	u64 vlanid;
};

struct sja1105_l2_lookup_entry {
	u64 mirrvlan;      /* P/Q/R/S only - LOCKEDS=1 */
	u64 mirr;          /* P/Q/R/S only - LOCKEDS=1 */
	u64 retag;         /* P/Q/R/S only - LOCKEDS=1 */
	u64 mask_iotag;    /* P/Q/R/S only */
	u64 mask_vlanid;   /* P/Q/R/S only */
	u64 mask_macaddr;  /* P/Q/R/S only */
	u64 iotag;         /* P/Q/R/S only */
	u64 vlanid;
	u64 macaddr;
	u64 destports;
	u64 enfport;
	u64 index;
};

struct sja1105_l2_lookup_params_entry {
	u64 drpbc;           /* P/Q/R/S only */
	u64 drpmc;           /* P/Q/R/S only */
	u64 drpuni;          /* P/Q/R/S only */
	u64 maxaddrp[5];     /* P/Q/R/S only */
	u64 start_dynspc;    /* P/Q/R/S only */
	u64 drpnolearn;      /* P/Q/R/S only */
	u64 use_static;      /* P/Q/R/S only */
	u64 owr_dyn;         /* P/Q/R/S only */
	u64 learn_once;      /* P/Q/R/S only */
	u64 maxage;          /* Shared */
	u64 dyn_tbsz;        /* E/T only */
	u64 poly;            /* E/T only */
	u64 shared_learn;    /* Shared */
	u64 no_enf_hostprt;  /* Shared */
	u64 no_mgmt_learn;   /* Shared */
};

struct sja1105_l2_forwarding_entry {
	u64 bc_domain;
	u64 reach_port;
	u64 fl_domain;
	u64 vlan_pmap[8];
};

struct sja1105_l2_forwarding_params_entry {
	u64 max_dynp;
	u64 part_spc[8];
};

struct sja1105_l2_policing_entry {
	u64 sharindx;
	u64 smax;
	u64 rate;
	u64 maxlen;
	u64 partition;
};

struct sja1105_mac_config_entry {
	u64 top[8];
	u64 base[8];
	u64 enabled[8];
	u64 ifg;
	u64 speed;
	u64 tp_delin;
	u64 tp_delout;
	u64 maxage;
	u64 vlanprio;
	u64 vlanid;
	u64 ing_mirr;
	u64 egr_mirr;
	u64 drpnona664;
	u64 drpdtag;
	u64 drpsotag;   /* only on P/Q/R/S */
	u64 drpsitag;   /* only on P/Q/R/S */
	u64 drpuntag;
	u64 retag;
	u64 dyn_learn;
	u64 egress;
	u64 ingress;
	u64 mirrcie;    /* only on P/Q/R/S */
	u64 mirrcetag;  /* only on P/Q/R/S */
	u64 ingmirrvid; /* only on P/Q/R/S */
	u64 ingmirrpcp; /* only on P/Q/R/S */
	u64 ingmirrdei; /* only on P/Q/R/S */
};

struct sja1105_xmii_params_entry {
	u64 phy_mac[5];
	u64 xmii_mode[5];
};

struct sja1105_avb_params_entry {
	u64 l2cbs; /* only on P/Q/R/S */
	u64 cas_master; /* only on P/Q/R/S */
	u64 destmeta;
	u64 srcmeta;
};

struct sja1105_sgmii_entry {
	u64 digital_error_cnt;
	u64 digital_control_2;
	u64 debug_control;
	u64 test_control;
	u64 autoneg_control;
	u64 digital_control_1;
	u64 autoneg_adv;
	u64 basic_control;
};

struct sja1105_table_header {
	u64 block_id;
	u64 len;
	u64 crc;
};

struct sja1105_table_ops {
	size_t (*packing)(void *buf, void *entry_ptr, enum packing_op op);
	size_t unpacked_entry_size;
	size_t packed_entry_size;
	size_t max_entry_count;
};

struct sja1105_table {
	const struct sja1105_table_ops *ops;
	size_t entry_count;
	void *entries;
};

struct sja1105_static_config {
	u64 device_id;
	struct sja1105_table tables[BLK_IDX_MAX];
};

size_t sja1105_table_header_packing(void *buf, void *hdr, enum packing_op op);
void
sja1105_table_header_pack_with_crc(void *buf, struct sja1105_table_header *hdr);
size_t
sja1105_static_config_get_length(const struct sja1105_static_config *config);

enum sja1105_static_config_validity {
	SJA1105_CONFIG_OK = 0,
	SJA1105_DEVICE_ID_INVALID,
	SJA1105_MISSING_L2_POLICING_TABLE,
	SJA1105_MISSING_L2_FORWARDING_TABLE,
	SJA1105_MISSING_L2_FORWARDING_PARAMS_TABLE,
	SJA1105_MISSING_GENERAL_PARAMS_TABLE,
	SJA1105_MISSING_VLAN_TABLE,
	SJA1105_MISSING_XMII_TABLE,
	SJA1105_MISSING_MAC_TABLE,
	SJA1105_OVERCOMMITTED_FRAME_MEMORY,
	SJA1105_UNEXPECTED_END_OF_BUFFER,
	SJA1105_INVALID_DEVICE_ID,
	SJA1105_INVALID_TABLE_HEADER_CRC,
	SJA1105_INVALID_TABLE_HEADER,
	SJA1105_INCORRECT_TABLE_LENGTH,
	SJA1105_DATA_CRC_INVALID,
	SJA1105_EXTRA_BYTES_AT_END_OF_BUFFER,
};

extern const char *sja1105_static_config_error_msg[];

enum sja1105_static_config_validity
sja1105_static_config_check_valid(const struct sja1105_static_config *config);
void
sja1105_static_config_pack(void *buf, struct sja1105_static_config *config);
int sja1105_static_config_init(struct sja1105_static_config *config,
			       u64 device_id, u64 part_nr);
void sja1105_static_config_free(struct sja1105_static_config *config);

int sja1105_table_delete_entry(struct sja1105_table *table, int i);
int sja1105_table_resize(struct sja1105_table *table, size_t new_count);

u32 sja1105_crc32(const void *buf, size_t len);

void sja1105_pack(void *buf, const u64 *val, int start, int end, size_t len);
void sja1105_unpack(const void *buf, u64 *val, int start, int end, size_t len);
void sja1105_packing(void *buf, u64 *val, int start, int end,
		     size_t len, enum packing_op op);

#endif

