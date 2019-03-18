// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Copyright (c) 2016-2018, NXP Semiconductors
 * Copyright (c) 2018-2019, Vladimir Oltean <olteanv@gmail.com>
 */
#include "sja1105_static_config.h"
#include <linux/crc32.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>

/* Convenience wrappers over the generic packing functions. These take into
 * account the SJA1105 memory layout quirks and provide some level of
 * programmer protection against incorrect API use. The errors are not expected
 * to occur durring runtime, therefore printing and swallowing them here is
 * appropriate instead of clutterring up higher-level code.
 */
void sja1105_pack(void *buf, const u64 *val, int start, int end, size_t len)
{
	int rc = packing(buf, (u64 *) val, start, end, len,
			 PACK, QUIRK_LSW32_IS_FIRST);

	if (likely(!rc))
		return;

	if (rc == -EINVAL) {
		pr_err("Start bit (%d) expected to be larger than end (%d)\n",
		       start, end);
	} else if (rc == -ERANGE) {
		if ((start - end + 1) > 64)
			pr_err("Field %d-%d too large for 64 bits!\n",
			       start, end);
		else
			pr_err("Cannot store %llx inside bits %d-%d (would truncate)\n",
			       *val, start, end);
	}
	dump_stack();
}

void sja1105_unpack(const void *buf, u64 *val, int start, int end, size_t len)
{
	int rc = packing((void *) buf, val, start, end, len,
			 UNPACK, QUIRK_LSW32_IS_FIRST);

	if (likely(!rc))
		return;

	if (rc == -EINVAL)
		pr_err("Start bit (%d) expected to be larger than end (%d)\n",
		       start, end);
	else if (rc == -ERANGE)
		pr_err("Field %d-%d too large for 64 bits!\n",
		       start, end);
	dump_stack();
}

void sja1105_packing(void *buf, u64 *val, int start, int end,
		     size_t len, enum packing_op op)
{
	int rc = packing(buf, val, start, end, len, op, QUIRK_LSW32_IS_FIRST);

	if (likely(!rc))
		return;

	if (rc == -EINVAL) {
		pr_err("Start bit (%d) expected to be larger than end (%d)\n",
		       start, end);
	} else if (rc == -ERANGE) {
		if ((start - end + 1) > 64)
			pr_err("Field %d-%d too large for 64 bits!\n",
			       start, end);
		else
			pr_err("Cannot store %llx inside bits %d-%d (would truncate)\n",
			       *val, start, end);
	}
	dump_stack();
}

/* Little-endian Ethernet CRC32 of data packed as big-endian u32 words */
u32 sja1105_crc32(const void *buf, size_t len)
{
	unsigned int i;
	u64 word;
	u32 crc;

	/* seed */
	crc = ~0;
	for (i = 0; i < len; i += 4) {
		sja1105_unpack((void *) buf + i, &word, 31, 0, 4);
		crc = crc32_le(crc, (u8 *) &word, 4);
	}
	return ~crc;
}

static size_t sja1105et_avb_params_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_AVB_PARAMS_ENTRY_ET;
	struct sja1105_avb_params_entry *entry;

	entry = (struct sja1105_avb_params_entry *) entry_ptr;

	sja1105_packing(buf, &entry->destmeta, 95, 48, size, op);
	sja1105_packing(buf, &entry->srcmeta,  47,  0, size, op);
	return size;
}

static size_t sja1105pqrs_avb_params_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_AVB_PARAMS_ENTRY_PQRS;
	struct sja1105_avb_params_entry *entry;

	entry = (struct sja1105_avb_params_entry *) entry_ptr;

	sja1105_packing(buf, &entry->l2cbs,      127, 127, size, op);
	sja1105_packing(buf, &entry->cas_master, 126, 126, size, op);
	sja1105_packing(buf, &entry->destmeta,   125,  78, size, op);
	sja1105_packing(buf, &entry->srcmeta,     77,  33, size, op);
	return size;
}

static size_t sja1105et_general_params_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_GENERAL_PARAMS_ENTRY_ET;
	struct sja1105_general_params_entry *entry;

	entry = (struct sja1105_general_params_entry *) entry_ptr;

	sja1105_packing(buf, &entry->vllupformat, 319, 319, size, op);
	sja1105_packing(buf, &entry->mirr_ptacu,  318, 318, size, op);
	sja1105_packing(buf, &entry->switchid,    317, 315, size, op);
	sja1105_packing(buf, &entry->hostprio,    314, 312, size, op);
	sja1105_packing(buf, &entry->mac_fltres1, 311, 264, size, op);
	sja1105_packing(buf, &entry->mac_fltres0, 263, 216, size, op);
	sja1105_packing(buf, &entry->mac_flt1,    215, 168, size, op);
	sja1105_packing(buf, &entry->mac_flt0,    167, 120, size, op);
	sja1105_packing(buf, &entry->incl_srcpt1, 119, 119, size, op);
	sja1105_packing(buf, &entry->incl_srcpt0, 118, 118, size, op);
	sja1105_packing(buf, &entry->send_meta1,  117, 117, size, op);
	sja1105_packing(buf, &entry->send_meta0,  116, 116, size, op);
	sja1105_packing(buf, &entry->casc_port,   115, 113, size, op);
	sja1105_packing(buf, &entry->host_port,   112, 110, size, op);
	sja1105_packing(buf, &entry->mirr_port,   109, 107, size, op);
	sja1105_packing(buf, &entry->vlmarker,    106,  75, size, op);
	sja1105_packing(buf, &entry->vlmask,       74,  43, size, op);
	sja1105_packing(buf, &entry->tpid,         42,  27, size, op);
	sja1105_packing(buf, &entry->ignore2stf,   26,  26, size, op);
	sja1105_packing(buf, &entry->tpid2,        25,  10, size, op);
	return size;
}

static size_t sja1105pqrs_general_params_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_GENERAL_PARAMS_ENTRY_PQRS;
	struct sja1105_general_params_entry *entry;

	entry = (struct sja1105_general_params_entry *) entry_ptr;

	sja1105_packing(buf, &entry->vllupformat, 351, 351, size, op);
	sja1105_packing(buf, &entry->mirr_ptacu,  350, 350, size, op);
	sja1105_packing(buf, &entry->switchid,    349, 347, size, op);
	sja1105_packing(buf, &entry->hostprio,    346, 344, size, op);
	sja1105_packing(buf, &entry->mac_fltres1, 343, 296, size, op);
	sja1105_packing(buf, &entry->mac_fltres0, 295, 248, size, op);
	sja1105_packing(buf, &entry->mac_flt1,    247, 200, size, op);
	sja1105_packing(buf, &entry->mac_flt0,    199, 152, size, op);
	sja1105_packing(buf, &entry->incl_srcpt1, 151, 151, size, op);
	sja1105_packing(buf, &entry->incl_srcpt0, 150, 150, size, op);
	sja1105_packing(buf, &entry->send_meta1,  149, 149, size, op);
	sja1105_packing(buf, &entry->send_meta0,  148, 148, size, op);
	sja1105_packing(buf, &entry->casc_port,   147, 145, size, op);
	sja1105_packing(buf, &entry->host_port,   144, 142, size, op);
	sja1105_packing(buf, &entry->mirr_port,   141, 139, size, op);
	sja1105_packing(buf, &entry->vlmarker,    138, 107, size, op);
	sja1105_packing(buf, &entry->vlmask,      106,  75, size, op);
	sja1105_packing(buf, &entry->tpid,         74,  59, size, op);
	sja1105_packing(buf, &entry->ignore2stf,   58,  58, size, op);
	sja1105_packing(buf, &entry->tpid2,        57,  42, size, op);
	sja1105_packing(buf, &entry->queue_ts,     41,  41, size, op);
	sja1105_packing(buf, &entry->egrmirrvid,   40,  29, size, op);
	sja1105_packing(buf, &entry->egrmirrpcp,   28,  26, size, op);
	sja1105_packing(buf, &entry->egrmirrdei,   25,  25, size, op);
	sja1105_packing(buf, &entry->replay_port,  24,  22, size, op);
	return size;
}

static size_t sja1105_l2_forwarding_params_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_L2_FORWARDING_PARAMS_ENTRY;
	struct sja1105_l2_forwarding_params_entry *entry;
	int offset, i;

	entry = (struct sja1105_l2_forwarding_params_entry *) entry_ptr;

	sja1105_packing(buf, &entry->max_dynp, 95, 93, size, op);
	for (i = 0, offset = 13; i < 8; i++, offset += 10)
		sja1105_packing(buf, &entry->part_spc[i],
				offset + 9, offset + 0, size, op);
	return size;
}

size_t sja1105_l2_forwarding_entry_packing(void *buf, void *entry_ptr,
					   enum packing_op op)
{
	const size_t size = SIZE_L2_FORWARDING_ENTRY;
	struct sja1105_l2_forwarding_entry *entry;
	int offset, i;

	entry = (struct sja1105_l2_forwarding_entry *) entry_ptr;

	sja1105_packing(buf, &entry->bc_domain,  63, 59, size, op);
	sja1105_packing(buf, &entry->reach_port, 58, 54, size, op);
	sja1105_packing(buf, &entry->fl_domain,  53, 49, size, op);
	for (i = 0, offset = 25; i < 8; i++, offset += 3)
		sja1105_packing(buf, &entry->vlan_pmap[i],
				offset + 2, offset + 0, size, op);
	return size;
}

static size_t sja1105et_l2_lookup_params_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_L2_LOOKUP_PARAMS_ENTRY_ET;
	struct sja1105_l2_lookup_params_entry *entry;

	entry = (struct sja1105_l2_lookup_params_entry *) entry_ptr;

	sja1105_packing(buf, &entry->maxage,         31, 17, size, op);
	sja1105_packing(buf, &entry->dyn_tbsz,       16, 14, size, op);
	sja1105_packing(buf, &entry->poly,           13,  6, size, op);
	sja1105_packing(buf, &entry->shared_learn,    5,  5, size, op);
	sja1105_packing(buf, &entry->no_enf_hostprt,  4,  4, size, op);
	sja1105_packing(buf, &entry->no_mgmt_learn,   3,  3, size, op);
	return size;
}

static size_t sja1105pqrs_l2_lookup_params_entry_packing(void *buf,
				void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_L2_LOOKUP_PARAMS_ENTRY_PQRS;
	struct sja1105_l2_lookup_params_entry *entry;
	int offset, i;

	entry = (struct sja1105_l2_lookup_params_entry *) entry_ptr;

	sja1105_packing(buf, &entry->drpbc,         127, 123, size, op);
	sja1105_packing(buf, &entry->drpmc,         122, 118, size, op);
	sja1105_packing(buf, &entry->drpuni,        117, 113, size, op);
	for (i = 0, offset = 58; i < 5; i++, offset += 11)
		sja1105_packing(buf, &entry->maxaddrp[i],
				offset + 10, offset + 0, size, op);
	sja1105_packing(buf, &entry->maxage,         57,  43, size, op);
	sja1105_packing(buf, &entry->start_dynspc,   42,  33, size, op);
	sja1105_packing(buf, &entry->drpnolearn,     32,  28, size, op);
	sja1105_packing(buf, &entry->shared_learn,   27,  27, size, op);
	sja1105_packing(buf, &entry->no_enf_hostprt, 26,  26, size, op);
	sja1105_packing(buf, &entry->no_mgmt_learn,  25,  25, size, op);
	sja1105_packing(buf, &entry->use_static,     24,  24, size, op);
	sja1105_packing(buf, &entry->owr_dyn,        23,  23, size, op);
	sja1105_packing(buf, &entry->learn_once,     22,  22, size, op);
	return size;
}

size_t sja1105et_l2_lookup_entry_packing(void *buf, void *entry_ptr,
					 enum packing_op op)
{
	const size_t size = SIZE_L2_LOOKUP_ENTRY_ET;
	struct sja1105_l2_lookup_entry *entry;

	entry = (struct sja1105_l2_lookup_entry *) entry_ptr;

	sja1105_packing(buf, &entry->vlanid,    95, 84, size, op);
	sja1105_packing(buf, &entry->macaddr,   83, 36, size, op);
	sja1105_packing(buf, &entry->destports, 35, 31, size, op);
	sja1105_packing(buf, &entry->enfport,   30, 30, size, op);
	sja1105_packing(buf, &entry->index,     29, 20, size, op);
	return size;
}

size_t sja1105pqrs_l2_lookup_entry_packing(void *buf, void *entry_ptr,
					   enum packing_op op)
{
	const size_t size = SIZE_L2_LOOKUP_ENTRY_PQRS;
	struct sja1105_l2_lookup_entry *entry;

	entry = (struct sja1105_l2_lookup_entry *) entry_ptr;

	/* These are static L2 lookup entries, so the structure
	 * should match UM11040 Table 16/17 definitions when
	 * LOCKEDS is 1.
	 */
	sja1105_packing(buf, &entry->mirrvlan,     158, 147, size, op);
	sja1105_packing(buf, &entry->mirr,         145, 145, size, op);
	sja1105_packing(buf, &entry->retag,        144, 144, size, op);
	sja1105_packing(buf, &entry->mask_iotag,   143, 143, size, op);
	sja1105_packing(buf, &entry->mask_vlanid,  142, 131, size, op);
	sja1105_packing(buf, &entry->mask_macaddr, 130,  83, size, op);
	sja1105_packing(buf, &entry->iotag,         82,  82, size, op);
	sja1105_packing(buf, &entry->vlanid,        81,  70, size, op);
	sja1105_packing(buf, &entry->macaddr,       69,  22, size, op);
	sja1105_packing(buf, &entry->destports,     21,  17, size, op);
	sja1105_packing(buf, &entry->enfport,       16,  16, size, op);
	sja1105_packing(buf, &entry->index,         15,   6, size, op);
	return size;
}

static size_t sja1105_l2_policing_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_L2_POLICING_ENTRY;
	struct sja1105_l2_policing_entry *entry;

	entry = (struct sja1105_l2_policing_entry *) entry_ptr;

	sja1105_packing(buf, &entry->sharindx,  63, 58, size, op);
	sja1105_packing(buf, &entry->smax,      57, 42, size, op);
	sja1105_packing(buf, &entry->rate,      41, 26, size, op);
	sja1105_packing(buf, &entry->maxlen,    25, 15, size, op);
	sja1105_packing(buf, &entry->partition, 14, 12, size, op);
	return size;
}

static size_t sja1105et_mac_config_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_MAC_CONFIG_ENTRY_ET;
	struct sja1105_mac_config_entry *entry;
	int offset, i;

	entry = (struct sja1105_mac_config_entry *) entry_ptr;

	for (i = 0, offset = 72; i < 8; i++, offset += 19) {
		sja1105_packing(buf, &entry->enabled[i],
				offset +  0, offset +  0, size, op);
		sja1105_packing(buf, &entry->base[i],
				offset +  9, offset +  1, size, op);
		sja1105_packing(buf, &entry->top[i],
				offset + 18, offset + 10, size, op);
	}
	sja1105_packing(buf, &entry->ifg,       71, 67, size, op);
	sja1105_packing(buf, &entry->speed,     66, 65, size, op);
	sja1105_packing(buf, &entry->tp_delin,  64, 49, size, op);
	sja1105_packing(buf, &entry->tp_delout, 48, 33, size, op);
	sja1105_packing(buf, &entry->maxage,    32, 25, size, op);
	sja1105_packing(buf, &entry->vlanprio,  24, 22, size, op);
	sja1105_packing(buf, &entry->vlanid,    21, 10, size, op);
	sja1105_packing(buf, &entry->ing_mirr,   9,  9, size, op);
	sja1105_packing(buf, &entry->egr_mirr,   8,  8, size, op);
	sja1105_packing(buf, &entry->drpnona664, 7,  7, size, op);
	sja1105_packing(buf, &entry->drpdtag,    6,  6, size, op);
	sja1105_packing(buf, &entry->drpuntag,   5,  5, size, op);
	sja1105_packing(buf, &entry->retag,      4,  4, size, op);
	sja1105_packing(buf, &entry->dyn_learn,  3,  3, size, op);
	sja1105_packing(buf, &entry->egress,     2,  2, size, op);
	sja1105_packing(buf, &entry->ingress,    1,  1, size, op);
	return size;
}

size_t sja1105pqrs_mac_config_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_MAC_CONFIG_ENTRY_PQRS;
	struct sja1105_mac_config_entry *entry;
	int offset, i;

	entry = (struct sja1105_mac_config_entry *) entry_ptr;

	for (i = 0, offset = 104; i < 8; i++, offset += 19) {
		sja1105_packing(buf, &entry->enabled[i],
				offset +  0, offset +  0, size, op);
		sja1105_packing(buf, &entry->base[i],
				offset +  9, offset +  1, size, op);
		sja1105_packing(buf, &entry->top[i],
				offset + 18, offset + 10, size, op);
	}
	sja1105_packing(buf, &entry->ifg,       103, 99, size, op);
	sja1105_packing(buf, &entry->speed,      98, 97, size, op);
	sja1105_packing(buf, &entry->tp_delin,   96, 81, size, op);
	sja1105_packing(buf, &entry->tp_delout,  80, 65, size, op);
	sja1105_packing(buf, &entry->maxage,     64, 57, size, op);
	sja1105_packing(buf, &entry->vlanprio,   56, 54, size, op);
	sja1105_packing(buf, &entry->vlanid,     53, 42, size, op);
	sja1105_packing(buf, &entry->ing_mirr,   41, 41, size, op);
	sja1105_packing(buf, &entry->egr_mirr,   40, 40, size, op);
	sja1105_packing(buf, &entry->drpnona664, 39, 39, size, op);
	sja1105_packing(buf, &entry->drpdtag,    38, 38, size, op);
	sja1105_packing(buf, &entry->drpsotag,   37, 37, size, op);
	sja1105_packing(buf, &entry->drpsitag,   36, 36, size, op);
	sja1105_packing(buf, &entry->drpuntag,   35, 35, size, op);
	sja1105_packing(buf, &entry->retag,      34, 34, size, op);
	sja1105_packing(buf, &entry->dyn_learn,  33, 33, size, op);
	sja1105_packing(buf, &entry->egress,     32, 32, size, op);
	sja1105_packing(buf, &entry->ingress,    31, 31, size, op);
	sja1105_packing(buf, &entry->mirrcie,    30, 30, size, op);
	sja1105_packing(buf, &entry->mirrcetag,  29, 29, size, op);
	sja1105_packing(buf, &entry->ingmirrvid, 28, 17, size, op);
	sja1105_packing(buf, &entry->ingmirrpcp, 16, 14, size, op);
	sja1105_packing(buf, &entry->ingmirrdei, 13, 13, size, op);
	return size;
}

static size_t sja1105_schedule_entry_points_params_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	struct sja1105_schedule_entry_points_params_entry *entry;
	const size_t size = SIZE_SCHEDULE_ENTRY_POINTS_PARAMS_ENTRY;

	entry = (struct sja1105_schedule_entry_points_params_entry *) entry_ptr;

	sja1105_packing(buf, &entry->clksrc,    31, 30, size, op);
	sja1105_packing(buf, &entry->actsubsch, 29, 27, size, op);
	return size;
}

static size_t sja1105_schedule_entry_points_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	struct sja1105_schedule_entry_points_entry *entry;
	const size_t size = SIZE_SCHEDULE_ENTRY_POINTS_ENTRY;

	entry = (struct sja1105_schedule_entry_points_entry *) entry_ptr;

	sja1105_packing(buf, &entry->subschindx, 31, 29, size, op);
	sja1105_packing(buf, &entry->delta,      28, 11, size, op);
	sja1105_packing(buf, &entry->address,    10, 1,  size, op);
	return size;
}

static size_t sja1105_schedule_params_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_SCHEDULE_PARAMS_ENTRY;
	struct sja1105_schedule_params_entry *entry;
	int offset, i;

	entry = (struct sja1105_schedule_params_entry *) entry_ptr;

	for (i = 0, offset = 16; i < 8; i++, offset += 10)
		sja1105_packing(buf, &entry->subscheind[i],
				offset + 9, offset + 0, size, op);
	return size;
}

static size_t sja1105_schedule_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_SCHEDULE_ENTRY;
	struct sja1105_schedule_entry *entry;

	entry = (struct sja1105_schedule_entry *) entry_ptr;

	sja1105_packing(buf, &entry->winstindex,  63, 54, size, op);
	sja1105_packing(buf, &entry->winend,      53, 53, size, op);
	sja1105_packing(buf, &entry->winst,       52, 52, size, op);
	sja1105_packing(buf, &entry->destports,   51, 47, size, op);
	sja1105_packing(buf, &entry->setvalid,    46, 46, size, op);
	sja1105_packing(buf, &entry->txen,        45, 45, size, op);
	sja1105_packing(buf, &entry->resmedia_en, 44, 44, size, op);
	sja1105_packing(buf, &entry->resmedia,    43, 36, size, op);
	sja1105_packing(buf, &entry->vlindex,     35, 26, size, op);
	sja1105_packing(buf, &entry->delta,       25, 8,  size, op);
	return size;
}

static size_t sja1105_sgmii_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_SGMII_ENTRY;
	struct sja1105_sgmii_entry *entry;
	u64 tmp;

	entry = (struct sja1105_sgmii_entry *) entry_ptr;

	sja1105_packing(buf, &entry->digital_error_cnt,
			1151, 1120, size, op);
	sja1105_packing(buf, &entry->digital_control_2,
			1119, 1088, size, op);
	sja1105_packing(buf, &entry->debug_control,
			383,  352, size, op);
	sja1105_packing(buf, &entry->test_control,
			351,  320, size, op);
	sja1105_packing(buf, &entry->autoneg_control,
			287,  256, size, op);
	sja1105_packing(buf, &entry->digital_control_1,
			255,  224, size, op);
	sja1105_packing(buf, &entry->autoneg_adv,
			223,  192, size, op);
	sja1105_packing(buf, &entry->basic_control,
			191,  160, size, op);
	/* Reserved areas */
	if (op == PACK) {
		tmp = 0x0000ull; sja1105_pack(buf, &tmp, 1087, 1056, size);
		tmp = 0x0000ull; sja1105_pack(buf, &tmp, 1055, 1024, size);
		tmp = 0x0000ull; sja1105_pack(buf, &tmp, 1023,  992, size);
		tmp = 0x0100ull; sja1105_pack(buf, &tmp,  991,  960, size);
		tmp = 0x023Full; sja1105_pack(buf, &tmp,  959,  928, size);
		tmp = 0x000Aull; sja1105_pack(buf, &tmp,  927,  896, size);
		tmp = 0x1C22ull; sja1105_pack(buf, &tmp,  895,  864, size);
		tmp = 0x0001ull; sja1105_pack(buf, &tmp,  863,  832, size);
		tmp = 0x0003ull; sja1105_pack(buf, &tmp,  831,  800, size);
		tmp = 0x0000ull; sja1105_pack(buf, &tmp,  799,  768, size);
		tmp = 0x0001ull; sja1105_pack(buf, &tmp,  767,  736, size);
		tmp = 0x0005ull; sja1105_pack(buf, &tmp,  735,  704, size);
		tmp = 0x0101ull; sja1105_pack(buf, &tmp,  703,  672, size);
		tmp = 0x0000ull; sja1105_pack(buf, &tmp,  671,  640, size);
		tmp = 0x0001ull; sja1105_pack(buf, &tmp,  639,  608, size);
		tmp = 0x0000ull; sja1105_pack(buf, &tmp,  607,  576, size);
		tmp = 0x000Aull; sja1105_pack(buf, &tmp,  575,  544, size);
		tmp = 0x0000ull; sja1105_pack(buf, &tmp,  543,  512, size);
		tmp = 0x0000ull; sja1105_pack(buf, &tmp,  511,  480, size);
		tmp = 0x0000ull; sja1105_pack(buf, &tmp,  479,  448, size);
		tmp = 0x0000ull; sja1105_pack(buf, &tmp,  447,  416, size);
		tmp = 0x899Cull; sja1105_pack(buf, &tmp,  415,  384, size);
		tmp = 0x000Aull; sja1105_pack(buf, &tmp,  319,  288, size);
		tmp = 0x0004ull; sja1105_pack(buf, &tmp,  159,  128, size);
		tmp = 0x0000ull; sja1105_pack(buf, &tmp,  127,   96, size);
		tmp = 0x0000ull; sja1105_pack(buf, &tmp,   95,   64, size);
		tmp = 0x0000ull; sja1105_pack(buf, &tmp,   63,   32, size);
		tmp = 0x0000ull; sja1105_pack(buf, &tmp,   31,    0, size);
	}
	return size;
}

static size_t sja1105_vl_forwarding_params_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_VL_FORWARDING_PARAMS_ENTRY;
	struct sja1105_vl_forwarding_params_entry *entry;
	int offset, i;

	entry = (struct sja1105_vl_forwarding_params_entry *) entry_ptr;

	for (i = 0, offset = 16; i < 8; i++, offset += 10)
		sja1105_packing(buf, &entry->partspc[i],
				offset + 9, offset + 0, size, op);
	sja1105_packing(buf, &entry->debugen, 15, 15, size, op);
	return size;
}

static size_t sja1105_vl_forwarding_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_VL_FORWARDING_ENTRY;
	struct sja1105_vl_forwarding_entry *entry;

	entry = (struct sja1105_vl_forwarding_entry *) entry_ptr;

	sja1105_packing(buf, &entry->type,      31, 31, size, op);
	sja1105_packing(buf, &entry->priority,  30, 28, size, op);
	sja1105_packing(buf, &entry->partition, 27, 25, size, op);
	sja1105_packing(buf, &entry->destports, 24, 20, size, op);
	return size;
}

size_t sja1105_vl_lookup_entry_packing(void *buf, void *entry_ptr,
				       enum packing_op op)
{
	const size_t size = SIZE_VL_LOOKUP_ENTRY;
	struct sja1105_vl_lookup_entry *entry;

	entry = (struct sja1105_vl_lookup_entry *) entry_ptr;

	if (entry->format == 0) {
		/* Interpreting vllupformat as 0 */
		sja1105_packing(buf, &entry->destports,
				95, 91, size, op);
		sja1105_packing(buf, &entry->iscritical,
				90, 90, size, op);
		sja1105_packing(buf, &entry->macaddr,
				89, 42, size, op);
		sja1105_packing(buf, &entry->vlanid,
				41, 30, size, op);
		sja1105_packing(buf, &entry->port,
				29, 27, size, op);
		sja1105_packing(buf, &entry->vlanprior,
				26, 24, size, op);
	} else {
		/* Interpreting vllupformat as 1 */
		sja1105_packing(buf, &entry->egrmirr,
				95, 91, size, op);
		sja1105_packing(buf, &entry->ingrmirr,
				90, 90, size, op);
		sja1105_packing(buf, &entry->vlid,
				57, 42, size, op);
		sja1105_packing(buf, &entry->port,
				29, 27, size, op);
	}
	return size;
}

static size_t sja1105_vl_policing_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_VL_POLICING_ENTRY;
	struct sja1105_vl_policing_entry *entry;

	entry = (struct sja1105_vl_policing_entry *) entry_ptr;

	sja1105_packing(buf, &entry->type,      63, 63, size, op);
	sja1105_packing(buf, &entry->maxlen,    62, 52, size, op);
	sja1105_packing(buf, &entry->sharindx,  51, 42, size, op);
	if (entry->type == 0) {
		sja1105_packing(buf, &entry->bag,    41, 28, size, op);
		sja1105_packing(buf, &entry->jitter, 27, 18, size, op);
	}
	return size;
}

size_t sja1105_vlan_lookup_entry_packing(void *buf, void *entry_ptr,
					 enum packing_op op)
{
	const size_t size = SIZE_VLAN_LOOKUP_ENTRY;
	struct sja1105_vlan_lookup_entry *entry;

	entry = (struct sja1105_vlan_lookup_entry *) entry_ptr;

	sja1105_packing(buf, &entry->ving_mirr,  63, 59, size, op);
	sja1105_packing(buf, &entry->vegr_mirr,  58, 54, size, op);
	sja1105_packing(buf, &entry->vmemb_port, 53, 49, size, op);
	sja1105_packing(buf, &entry->vlan_bc,    48, 44, size, op);
	sja1105_packing(buf, &entry->tag_port,   43, 39, size, op);
	sja1105_packing(buf, &entry->vlanid,     38, 27, size, op);
	return size;
}

static size_t sja1105_clk_sync_params_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	/* TODO not implemented */
	return SIZE_CLK_SYNC_PARAMS_ENTRY;
}

size_t sja1105_retagging_entry_packing(void *buf, void *entry_ptr,
				       enum packing_op op)
{
	const size_t size = SIZE_RETAGGING_ENTRY;
	struct sja1105_retagging_entry *entry;

	entry = (struct sja1105_retagging_entry *) entry_ptr;

	sja1105_packing(buf, &entry->egr_port,     63, 59, size, op);
	sja1105_packing(buf, &entry->ing_port,     58, 54, size, op);
	sja1105_packing(buf, &entry->vlan_ing,     53, 42, size, op);
	sja1105_packing(buf, &entry->vlan_egr,     41, 30, size, op);
	sja1105_packing(buf, &entry->do_not_learn, 29, 29, size, op);
	sja1105_packing(buf, &entry->destports,    27, 23, size, op);
	return size;
}

static size_t sja1105_xmii_params_entry_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_XMII_PARAMS_ENTRY;
	struct sja1105_xmii_params_entry *entry;
	int offset, i;

	entry = (struct sja1105_xmii_params_entry *) entry_ptr;

	for (i = 0, offset = 17; i < 5; i++, offset += 3) {
		sja1105_packing(buf, &entry->xmii_mode[i],
				offset + 1, offset + 0, size, op);
		sja1105_packing(buf, &entry->phy_mac[i],
				offset + 2, offset + 2, size, op);
	}
	return size;
}

size_t sja1105_table_header_packing(void *buf,
			void *entry_ptr, enum packing_op op)
{
	const size_t size = SIZE_TABLE_HEADER;
	struct sja1105_table_header *entry;

	entry = (struct sja1105_table_header *) entry_ptr;

	sja1105_packing(buf, &entry->block_id, 31, 24, size, op);
	sja1105_packing(buf, &entry->len,      55, 32, size, op);
	sja1105_packing(buf, &entry->crc,      95, 64, size, op);
	return size;
}

/* WARNING: the *hdr pointer is really non-const, because it is
 * modifying the CRC of the header for a 2-stage packing operation
 */
void
sja1105_table_header_pack_with_crc(void *buf, struct sja1105_table_header *hdr)
{
	/* First pack the table as-is, then calculate the CRC, and
	 * finally put the proper CRC into the packed buffer
	 */
	memset(buf, 0, SIZE_TABLE_HEADER);
	sja1105_table_header_packing(buf, hdr, PACK);
	hdr->crc = sja1105_crc32(buf, SIZE_TABLE_HEADER - 4);
	sja1105_pack(buf + SIZE_TABLE_HEADER - 4, &hdr->crc, 31, 0, 4);
}

static void sja1105_table_write_crc(char *table_start, char *crc_ptr)
{
	u64 computed_crc;
	int len_bytes;

	len_bytes = (int) (crc_ptr - table_start);
	computed_crc = sja1105_crc32(table_start, len_bytes);
	sja1105_pack(crc_ptr, &computed_crc, 31, 0, 4);
}

/* The block IDs that the switches support are unfortunately sparse, so keep a
 * mapping table to "block indices" and translate back and forth so that we
 * don't waste useless memory in struct sja1105_static_config.
 * Also, since the block id comes from essentially untrusted input (unpacking
 * the static config from userspace) it has to be sanitized (range-checked)
 * before blindly indexing kernel memory with the blk_idx.
 */
static u64 blk_id_map[BLK_IDX_MAX] = {
	[BLK_IDX_SCHEDULE] = BLKID_SCHEDULE,
	[BLK_IDX_SCHEDULE_ENTRY_POINTS] = BLKID_SCHEDULE_ENTRY_POINTS,
	[BLK_IDX_VL_LOOKUP] = BLKID_VL_LOOKUP,
	[BLK_IDX_VL_POLICING] = BLKID_VL_POLICING,
	[BLK_IDX_VL_FORWARDING] = BLKID_VL_FORWARDING,
	[BLK_IDX_L2_LOOKUP] = BLKID_L2_LOOKUP,
	[BLK_IDX_L2_POLICING] = BLKID_L2_POLICING,
	[BLK_IDX_VLAN_LOOKUP] = BLKID_VLAN_LOOKUP,
	[BLK_IDX_L2_FORWARDING] = BLKID_L2_FORWARDING,
	[BLK_IDX_MAC_CONFIG] = BLKID_MAC_CONFIG,
	[BLK_IDX_SCHEDULE_PARAMS] = BLKID_SCHEDULE_PARAMS,
	[BLK_IDX_SCHEDULE_ENTRY_POINTS_PARAMS] = BLKID_SCHEDULE_ENTRY_POINTS_PARAMS,
	[BLK_IDX_VL_FORWARDING_PARAMS] = BLKID_VL_FORWARDING_PARAMS,
	[BLK_IDX_L2_LOOKUP_PARAMS] = BLKID_L2_LOOKUP_PARAMS,
	[BLK_IDX_L2_FORWARDING_PARAMS] = BLKID_L2_FORWARDING_PARAMS,
	[BLK_IDX_CLK_SYNC_PARAMS] = BLKID_CLK_SYNC_PARAMS,
	[BLK_IDX_AVB_PARAMS] = BLKID_AVB_PARAMS,
	[BLK_IDX_GENERAL_PARAMS] = BLKID_GENERAL_PARAMS,
	[BLK_IDX_RETAGGING] = BLKID_RETAGGING,
	[BLK_IDX_XMII_PARAMS] = BLKID_XMII_PARAMS,
	[BLK_IDX_SGMII] = BLKID_SGMII,
};

static enum sja1105_blk_idx blk_idx_from_blk_id(u64 block_id)
{
	enum sja1105_blk_idx blk_idx;

	if (block_id > BLKID_MAX)
		return BLK_IDX_INVAL;

	for (blk_idx = 0; blk_idx < BLK_IDX_MAX; blk_idx++)
		if (blk_id_map[blk_idx] == block_id)
			return blk_idx;

	return BLK_IDX_INVAL;
}

static ssize_t
sja1105_table_add_entry(struct sja1105_table *table, const void *buf)
{
	void *entry_ptr;

	if (table->entry_count >= table->ops->max_entry_count)
		return -ERANGE;

	entry_ptr = table->entries;
	entry_ptr += (uintptr_t) table->ops->unpacked_entry_size * table->entry_count;

	table->entry_count++;

	memset(entry_ptr, 0, table->ops->unpacked_entry_size);

	/* Discard const pointer due to common implementation
	 * of PACK and UNPACK. */
	return table->ops->packing((void *) buf, entry_ptr, UNPACK);
}

/* This is needed so that all information needed for
 * sja1105_vl_lookup_entry_packing is self-contained within
 * the structure and does not depend upon the general_params_table.
 */
static void
sja1105_static_config_patch_vllupformat(struct sja1105_static_config *config)
{
	struct sja1105_vl_lookup_entry *vl_lookup_entries;
	struct sja1105_general_params_entry *general_params_entries;
	struct sja1105_table *tables = config->tables;
	u64 vllupformat;
	int i;

	vl_lookup_entries = tables[BLK_IDX_VL_LOOKUP].entries;
	general_params_entries = tables[BLK_IDX_GENERAL_PARAMS].entries;

	vllupformat = general_params_entries[0].vllupformat;

	for (i = 0; i < tables[BLK_IDX_VL_LOOKUP].entry_count; i++)
		vl_lookup_entries[i].format = vllupformat;
}

const char *sja1105_static_config_error_msg[] = {
	[SJA1105_CONFIG_OK] = "",
	[SJA1105_DEVICE_ID_INVALID] =
		"Device ID present in the static config is invalid",
	[SJA1105_TTETHERNET_NOT_SUPPORTED] =
		"schedule-table present, but TTEthernet is "
		"only supported on T and Q/S",
	[SJA1105_INCORRECT_TTETHERNET_CONFIGURATION] =
		"schedule-table present, but one of "
		"schedule-entry-points-table, schedule-parameters-table or "
		"schedule-entry-points-parameters table is empty",
	[SJA1105_INCORRECT_VIRTUAL_LINK_CONFIGURATION] =
		"vl-lookup-table present, but one of vl-policing-table, "
		"vl-forwarding-table or vl-forwarding-parameters-table is empty",
	[SJA1105_MISSING_L2_POLICING_TABLE] =
		"l2-policing-table needs to have at least one entry",
	[SJA1105_MISSING_L2_FORWARDING_TABLE] =
		"l2-forwarding-table is either missing or incomplete",
	[SJA1105_MISSING_L2_FORWARDING_PARAMS_TABLE] =
		"l2-forwarding-parameters-table is missing",
	[SJA1105_MISSING_GENERAL_PARAMS_TABLE] =
		"general-parameters-table is missing",
	[SJA1105_MISSING_VLAN_TABLE] =
		"vlan-lookup-table needs to have at least the default untagged VLAN",
	[SJA1105_MISSING_XMII_TABLE] =
		"xmii-table is missing",
	[SJA1105_MISSING_MAC_TABLE] =
		"mac-configuration-table needs to contain an entry for each port",
	[SJA1105_OVERCOMMITTED_FRAME_MEMORY] =
		"Not allowed to overcommit frame memory. L2 memory partitions "
		"and VL memory partitions share the same space. The sum of all "
		"16 memory partitions is not allowed to be larger than 929 "
		"128-byte blocks (or 910 with retagging). Please adjust "
		"l2-forwarding-parameters-table.part_spc and/or "
		"vl-forwarding-parameters-table.partspc.",
	[SJA1105_UNEXPECTED_END_OF_BUFFER] =
		"Unexpected end of buffer",
	[SJA1105_INVALID_DEVICE_ID] =
		"Invalid device ID present in static config",
	[SJA1105_INVALID_TABLE_HEADER_CRC] =
		"One of the table headers has an incorrect CRC",
	[SJA1105_INVALID_TABLE_HEADER] =
		"One of the table headers contains an invalid block id",
	[SJA1105_INCORRECT_TABLE_LENGTH] =
		"The data length specified in one of the table headers is "
		"longer than the actual size of the entries that were parsed",
	[SJA1105_DATA_CRC_INVALID] =
		"One of the tables has an incorrect CRC over the data area",
	[SJA1105_EXTRA_BYTES_AT_END_OF_BUFFER] =
		"Extra bytes found at the end of buffer after parsing it",
};

static enum sja1105_static_config_validity
static_config_check_memory_size(const struct sja1105_table *tables)
{
	const struct sja1105_l2_forwarding_params_entry *l2_forwarding_params;
	const struct sja1105_vl_forwarding_params_entry *vl_forwarding_params;
	int i, max_mem, mem = 0;

	l2_forwarding_params = tables[BLK_IDX_L2_FORWARDING_PARAMS].entries;

	for (i = 0; i < 8; i++)
		mem += l2_forwarding_params->part_spc[i];

	if (tables[BLK_IDX_VL_FORWARDING_PARAMS].entry_count) {
		vl_forwarding_params = tables[BLK_IDX_VL_FORWARDING_PARAMS].entries;
		for (i = 0; i < 8; i++)
			mem += vl_forwarding_params->partspc[i];
	}

	if (tables[BLK_IDX_RETAGGING].entry_count)
		max_mem = MAX_FRAME_MEMORY_RETAGGING;
	else
		max_mem = MAX_FRAME_MEMORY;

	if (mem > max_mem)
		return SJA1105_OVERCOMMITTED_FRAME_MEMORY;

	return SJA1105_CONFIG_OK;
}

enum sja1105_static_config_validity
sja1105_static_config_check_valid(const struct sja1105_static_config *config)
{
	const struct sja1105_table *tables = config->tables;

	if (!DEVICE_ID_VALID(config->device_id))
		return SJA1105_DEVICE_ID_INVALID;

	if (tables[BLK_IDX_SCHEDULE].entry_count) {

		if (!SUPPORTS_TTETHERNET(config->device_id))
			return SJA1105_TTETHERNET_NOT_SUPPORTED;

		if (tables[BLK_IDX_SCHEDULE_ENTRY_POINTS].entry_count !=
		    tables[BLK_IDX_SCHEDULE_ENTRY_POINTS].ops->max_entry_count)
			return SJA1105_INCORRECT_TTETHERNET_CONFIGURATION;

		if (tables[BLK_IDX_SCHEDULE_PARAMS].entry_count !=
		    tables[BLK_IDX_SCHEDULE_PARAMS].ops->max_entry_count)
			return SJA1105_INCORRECT_TTETHERNET_CONFIGURATION;

		if (tables[BLK_IDX_SCHEDULE_ENTRY_POINTS_PARAMS].entry_count !=
		    tables[BLK_IDX_SCHEDULE_ENTRY_POINTS_PARAMS].ops->max_entry_count)
			return SJA1105_INCORRECT_TTETHERNET_CONFIGURATION;
	}
	if (tables[BLK_IDX_VL_LOOKUP].entry_count) {
		if (tables[BLK_IDX_VL_POLICING].entry_count == 0)
			return SJA1105_INCORRECT_VIRTUAL_LINK_CONFIGURATION;

		if (tables[BLK_IDX_VL_FORWARDING].entry_count == 0)
			return SJA1105_INCORRECT_VIRTUAL_LINK_CONFIGURATION;

		if (tables[BLK_IDX_VL_FORWARDING_PARAMS].entry_count !=
		    tables[BLK_IDX_VL_FORWARDING_PARAMS].ops->max_entry_count)
			return SJA1105_INCORRECT_VIRTUAL_LINK_CONFIGURATION;
	}
	if (tables[BLK_IDX_L2_POLICING].entry_count == 0)
		return SJA1105_MISSING_L2_POLICING_TABLE;

	if (tables[BLK_IDX_VLAN_LOOKUP].entry_count == 0)
		return SJA1105_MISSING_VLAN_TABLE;

	if (tables[BLK_IDX_L2_FORWARDING].entry_count !=
	    tables[BLK_IDX_L2_FORWARDING].ops->max_entry_count)
		return SJA1105_MISSING_L2_FORWARDING_TABLE;

	if (tables[BLK_IDX_MAC_CONFIG].entry_count !=
	    tables[BLK_IDX_MAC_CONFIG].ops->max_entry_count)
		return SJA1105_MISSING_MAC_TABLE;

	if (tables[BLK_IDX_L2_FORWARDING_PARAMS].entry_count !=
	    tables[BLK_IDX_L2_FORWARDING_PARAMS].ops->max_entry_count)
		return SJA1105_MISSING_L2_FORWARDING_PARAMS_TABLE;

	if (tables[BLK_IDX_GENERAL_PARAMS].entry_count !=
	    tables[BLK_IDX_GENERAL_PARAMS].ops->max_entry_count)
		return SJA1105_MISSING_GENERAL_PARAMS_TABLE;

	if (tables[BLK_IDX_XMII_PARAMS].entry_count !=
	    tables[BLK_IDX_XMII_PARAMS].ops->max_entry_count)
		return SJA1105_MISSING_XMII_TABLE;

	return static_config_check_memory_size(tables);
}

enum sja1105_static_config_validity
sja1105_static_config_unpack(const void *buf, ssize_t buf_len,
			struct sja1105_static_config *config)
{
	struct sja1105_table_header hdr;
	enum sja1105_blk_idx blk_idx;
	struct sja1105_table *table;
	u64 computed_crc, read_crc;
	int expected_entry_count;
	const u8 *table_end;
	const u8 *p = buf;
	int bytes;

	/* Guard memory access to buffer */
	if (buf_len >= 4)
		buf_len -= 4;
	else
		return SJA1105_UNEXPECTED_END_OF_BUFFER;

	/* Retrieve device_id from first 4 bytes of packed buffer */
	sja1105_unpack(p, &config->device_id, 31, 0, 4);
	if (!DEVICE_ID_VALID(config->device_id))
		return SJA1105_INVALID_DEVICE_ID;

	p += SIZE_SJA1105_DEVICE_ID;

	while (1) {
		/* Guard memory access to buffer */
		if (buf_len >= SIZE_TABLE_HEADER)
			buf_len -= SIZE_TABLE_HEADER;
		else
			return SJA1105_UNEXPECTED_END_OF_BUFFER;

		/* Discard const pointer due to common implementation
		 * of PACK and UNPACK. */
		memset(&hdr, 0, sizeof(hdr));
		sja1105_table_header_packing((void *) p, &hdr, UNPACK);

		/* This should match on last table header */
		if (hdr.len == 0)
			break;

		computed_crc = sja1105_crc32(p, SIZE_TABLE_HEADER - 4);
		computed_crc &= 0xFFFFFFFF;
		read_crc = hdr.crc & 0xFFFFFFFF;
		if (read_crc != computed_crc)
			return SJA1105_INVALID_TABLE_HEADER_CRC;

		p += SIZE_TABLE_HEADER;

		/* Guard memory access to buffer */
		if (buf_len >= (int) hdr.len * 4)
			buf_len -= (int) hdr.len * 4;
		else
			return SJA1105_UNEXPECTED_END_OF_BUFFER;

		table_end = p + hdr.len * 4;
		computed_crc = sja1105_crc32(p, hdr.len * 4);

		blk_idx = blk_idx_from_blk_id(hdr.block_id);
		if (blk_idx == BLK_IDX_INVAL)
			return -EINVAL;
		table = &config->tables[blk_idx];
		/* Detected duplicate table headers with the same block id */
		if (table->entry_count)
			return -EINVAL;

		expected_entry_count = (int) (hdr.len * 4);
		expected_entry_count /= table->ops->packed_entry_size;
		table->entries = kcalloc(expected_entry_count,
				 table->ops->unpacked_entry_size, GFP_KERNEL);
		if (!table->entries)
			return -ENOMEM;

		while (p < table_end) {
			bytes = sja1105_table_add_entry(table, p);
			if (bytes < 0)
				return SJA1105_INVALID_TABLE_HEADER;
			p += bytes;
		};
		if (p != table_end)
			/* Incorrect table length for this block id:
			 * table data has (table_end - p) extra bytes.
			 */
			return SJA1105_INCORRECT_TABLE_LENGTH;
		/* Guard memory access to buffer */
		if (buf_len >= 4)
			buf_len -= 4;
		else
			return SJA1105_UNEXPECTED_END_OF_BUFFER;

		sja1105_unpack(p, &read_crc, 31, 0, 4);
		p += 4;
		if (computed_crc != read_crc)
			return SJA1105_DATA_CRC_INVALID;
	}
	if (buf_len)
		return SJA1105_EXTRA_BYTES_AT_END_OF_BUFFER;

	sja1105_static_config_patch_vllupformat(config);
	return SJA1105_CONFIG_OK;
}

void
sja1105_static_config_pack(void *buf, struct sja1105_static_config *config)
{
	struct sja1105_table_header header = {0};
	enum sja1105_blk_idx i;
	char *p = buf;
	int j;

	sja1105_pack(p, &config->device_id, 31, 0, 4);
	p += SIZE_SJA1105_DEVICE_ID;

	for (i = 0; i < BLK_IDX_MAX; i++) {
		const struct sja1105_table *table;
		char *table_start;

		table = &config->tables[i];
		if (!table->entry_count)
			continue;

		header.block_id = blk_id_map[i];
		header.len = table->entry_count * table->ops->packed_entry_size / 4;
		sja1105_table_header_pack_with_crc(p, &header);
		p += SIZE_TABLE_HEADER;
		table_start = p;
		for (j = 0; j < table->entry_count; j++) {
			void *entry_ptr = table->entries;
			entry_ptr += (uintptr_t) j * table->ops->unpacked_entry_size;
			memset(p, 0, table->ops->packed_entry_size);
			table->ops->packing(p, entry_ptr, PACK);
			p += table->ops->packed_entry_size;
		}
		sja1105_table_write_crc(table_start, p);
		p += 4;
	}
	/*
	 * Final header:
	 * Block ID does not matter
	 * Length of 0 marks that header is final
	 * CRC will be replaced on-the-fly on "config upload"
	 */
	header.block_id = 0;
	header.len = 0;
	header.crc = 0xDEADBEEF;
	memset(p, 0, SIZE_TABLE_HEADER);
	sja1105_table_header_packing(p, &header, PACK);
}

size_t
sja1105_static_config_get_length(const struct sja1105_static_config *config)
{
	unsigned int sum;
	unsigned int header_count;
	enum sja1105_blk_idx i;

	/* Ending header */
	header_count = 1;
	sum = SIZE_SJA1105_DEVICE_ID;

	/* Tables (headers and entries) */
	for (i = 0; i < BLK_IDX_MAX; i++) {
		const struct sja1105_table *table;

		table = &config->tables[i];
		if (table->entry_count)
			header_count++;

		sum += table->ops->packed_entry_size * table->entry_count;
	}
	/* Headers have an additional CRC at the end */
	sum += header_count * (SIZE_TABLE_HEADER + 4);
	/* Last header does not have an extra CRC because there is no data */
	sum -= 4;

	return sum;
}

/* Compatibility matrices */

/* SJA1105E: First generation, no TTEthernet */
static struct sja1105_table_ops sja1105e_table_ops[BLK_IDX_MAX] = {
	[BLK_IDX_SCHEDULE] = { 0 },
	[BLK_IDX_SCHEDULE_ENTRY_POINTS] = { 0 },
	[BLK_IDX_VL_LOOKUP] = { 0 },
	[BLK_IDX_VL_POLICING] = { 0 },
	[BLK_IDX_VL_FORWARDING] = { 0 },
	[BLK_IDX_L2_LOOKUP] = {
		.packing = sja1105et_l2_lookup_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_lookup_entry),
		.packed_entry_size = SIZE_L2_LOOKUP_ENTRY_ET,
		.max_entry_count = MAX_L2_LOOKUP_COUNT,
	},
	[BLK_IDX_L2_POLICING] = {
		.packing = sja1105_l2_policing_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_policing_entry),
		.packed_entry_size = SIZE_L2_POLICING_ENTRY,
		.max_entry_count = MAX_L2_POLICING_COUNT,
	},
	[BLK_IDX_VLAN_LOOKUP] = {
		.packing = sja1105_vlan_lookup_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vlan_lookup_entry),
		.packed_entry_size = SIZE_VLAN_LOOKUP_ENTRY,
		.max_entry_count = MAX_VLAN_LOOKUP_COUNT,
	},
	[BLK_IDX_L2_FORWARDING] = {
		.packing = sja1105_l2_forwarding_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_forwarding_entry),
		.packed_entry_size = SIZE_L2_FORWARDING_ENTRY,
		.max_entry_count = MAX_L2_FORWARDING_COUNT,
	},
	[BLK_IDX_MAC_CONFIG] = {
		.packing = sja1105et_mac_config_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_mac_config_entry),
		.packed_entry_size = SIZE_MAC_CONFIG_ENTRY_ET,
		.max_entry_count = MAX_MAC_CONFIG_COUNT,
	},
	[BLK_IDX_SCHEDULE_PARAMS] = { 0 },
	[BLK_IDX_SCHEDULE_ENTRY_POINTS_PARAMS] = { 0 },
	[BLK_IDX_VL_FORWARDING_PARAMS] = { 0 },
	[BLK_IDX_L2_LOOKUP_PARAMS] = {
		.packing = sja1105et_l2_lookup_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_lookup_params_entry),
		.packed_entry_size = SIZE_L2_LOOKUP_PARAMS_ENTRY_ET,
		.max_entry_count = MAX_L2_LOOKUP_PARAMS_COUNT,
	},
	[BLK_IDX_L2_FORWARDING_PARAMS] = {
		.packing = sja1105_l2_forwarding_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_forwarding_params_entry),
		.packed_entry_size = SIZE_L2_FORWARDING_PARAMS_ENTRY,
		.max_entry_count = MAX_L2_FORWARDING_PARAMS_COUNT,
	},
	[BLK_IDX_CLK_SYNC_PARAMS] = { 0 },
	[BLK_IDX_AVB_PARAMS] = {
		.packing = sja1105et_avb_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_avb_params_entry),
		.packed_entry_size = SIZE_AVB_PARAMS_ENTRY_ET,
		.max_entry_count = MAX_AVB_PARAMS_COUNT,
	},
	[BLK_IDX_GENERAL_PARAMS] = {
		.packing = sja1105et_general_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_general_params_entry),
		.packed_entry_size = SIZE_GENERAL_PARAMS_ENTRY_ET,
		.max_entry_count = MAX_GENERAL_PARAMS_COUNT,
	},
	[BLK_IDX_RETAGGING] = {
		.packing = sja1105_retagging_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_retagging_entry),
		.packed_entry_size = SIZE_RETAGGING_ENTRY,
		.max_entry_count = MAX_RETAGGING_COUNT,
	},
	[BLK_IDX_XMII_PARAMS] = {
		.packing = sja1105_xmii_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_xmii_params_entry),
		.packed_entry_size = SIZE_XMII_PARAMS_ENTRY,
		.max_entry_count = MAX_XMII_PARAMS_COUNT,
	},
	[BLK_IDX_SGMII] = { 0 },
};

/* SJA1105T: First generation, TTEthernet */
static struct sja1105_table_ops sja1105t_table_ops[BLK_IDX_MAX] = {
	[BLK_IDX_SCHEDULE] = {
		.packing = sja1105_schedule_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_schedule_entry),
		.packed_entry_size = SIZE_SCHEDULE_ENTRY,
		.max_entry_count = MAX_SCHEDULE_COUNT,
	},
	[BLK_IDX_SCHEDULE_ENTRY_POINTS] = {
		.packing = sja1105_schedule_entry_points_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_schedule_entry_points_entry),
		.packed_entry_size = SIZE_SCHEDULE_ENTRY_POINTS_ENTRY,
		.max_entry_count = MAX_SCHEDULE_ENTRY_POINTS_COUNT,
	},
	[BLK_IDX_VL_LOOKUP] = {
		.packing = sja1105_vl_lookup_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vl_lookup_entry),
		.packed_entry_size = SIZE_VL_LOOKUP_ENTRY,
		.max_entry_count = MAX_VL_LOOKUP_COUNT,
	},
	[BLK_IDX_VL_POLICING] = {
		.packing = sja1105_vl_policing_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vl_policing_entry),
		.packed_entry_size = SIZE_VL_POLICING_ENTRY,
		.max_entry_count = MAX_VL_POLICING_COUNT,
	},
	[BLK_IDX_VL_FORWARDING] = {
		.packing = sja1105_vl_forwarding_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vl_forwarding_entry),
		.packed_entry_size = SIZE_VL_FORWARDING_ENTRY,
		.max_entry_count = MAX_VL_FORWARDING_COUNT,
	},
	[BLK_IDX_L2_LOOKUP] = {
		.packing = sja1105et_l2_lookup_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_lookup_entry),
		.packed_entry_size = SIZE_L2_LOOKUP_ENTRY_ET,
		.max_entry_count = MAX_L2_LOOKUP_COUNT,
	},
	[BLK_IDX_L2_POLICING] = {
		.packing = sja1105_l2_policing_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_policing_entry),
		.packed_entry_size = SIZE_L2_POLICING_ENTRY,
		.max_entry_count = MAX_L2_POLICING_COUNT,
	},
	[BLK_IDX_VLAN_LOOKUP] = {
		.packing = sja1105_vlan_lookup_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vlan_lookup_entry),
		.packed_entry_size = SIZE_VLAN_LOOKUP_ENTRY,
		.max_entry_count = MAX_VLAN_LOOKUP_COUNT,
	},
	[BLK_IDX_L2_FORWARDING] = {
		.packing = sja1105_l2_forwarding_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_forwarding_entry),
		.packed_entry_size = SIZE_L2_FORWARDING_ENTRY,
		.max_entry_count = MAX_L2_FORWARDING_COUNT,
	},
	[BLK_IDX_MAC_CONFIG] = {
		.packing = sja1105et_mac_config_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_mac_config_entry),
		.packed_entry_size = SIZE_MAC_CONFIG_ENTRY_ET,
		.max_entry_count = MAX_MAC_CONFIG_COUNT,
	},
	[BLK_IDX_SCHEDULE_PARAMS] = {
		.packing = sja1105_schedule_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_schedule_params_entry),
		.packed_entry_size = SIZE_SCHEDULE_PARAMS_ENTRY,
		.max_entry_count = MAX_SCHEDULE_PARAMS_COUNT,
	},
	[BLK_IDX_SCHEDULE_ENTRY_POINTS_PARAMS] = {
		.packing = sja1105_schedule_entry_points_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_schedule_entry_points_params_entry),
		.packed_entry_size = SIZE_SCHEDULE_ENTRY_POINTS_PARAMS_ENTRY,
		.max_entry_count = MAX_SCHEDULE_ENTRY_POINTS_PARAMS_COUNT,
	},
	[BLK_IDX_VL_FORWARDING_PARAMS] = {
		.packing = sja1105_vl_forwarding_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vl_forwarding_params_entry),
		.packed_entry_size = SIZE_VL_FORWARDING_PARAMS_ENTRY,
		.max_entry_count = MAX_VL_FORWARDING_PARAMS_COUNT,
	},
	[BLK_IDX_L2_LOOKUP_PARAMS] = {
		.packing = sja1105et_l2_lookup_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_lookup_params_entry),
		.packed_entry_size = SIZE_L2_LOOKUP_PARAMS_ENTRY_ET,
		.max_entry_count = MAX_L2_LOOKUP_PARAMS_COUNT,
	},
	[BLK_IDX_L2_FORWARDING_PARAMS] = {
		.packing = sja1105_l2_forwarding_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_forwarding_params_entry),
		.packed_entry_size = SIZE_L2_FORWARDING_PARAMS_ENTRY,
		.max_entry_count = MAX_L2_FORWARDING_PARAMS_COUNT,
	},
	[BLK_IDX_CLK_SYNC_PARAMS] = {
		.packing = sja1105_clk_sync_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_clk_sync_params_entry),
		.packed_entry_size = SIZE_CLK_SYNC_PARAMS_ENTRY,
		.max_entry_count = MAX_CLK_SYNC_COUNT,
	},
	[BLK_IDX_AVB_PARAMS] = {
		.packing = sja1105et_avb_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_avb_params_entry),
		.packed_entry_size = SIZE_AVB_PARAMS_ENTRY_ET,
		.max_entry_count = MAX_AVB_PARAMS_COUNT,
	},
	[BLK_IDX_GENERAL_PARAMS] = {
		.packing = sja1105et_general_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_general_params_entry),
		.packed_entry_size = SIZE_GENERAL_PARAMS_ENTRY_ET,
		.max_entry_count = MAX_GENERAL_PARAMS_COUNT,
	},
	[BLK_IDX_RETAGGING] = {
		.packing = sja1105_retagging_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_retagging_entry),
		.packed_entry_size = SIZE_RETAGGING_ENTRY,
		.max_entry_count = MAX_RETAGGING_COUNT,
	},
	[BLK_IDX_XMII_PARAMS] = {
		.packing = sja1105_xmii_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_xmii_params_entry),
		.packed_entry_size = SIZE_XMII_PARAMS_ENTRY,
		.max_entry_count = MAX_XMII_PARAMS_COUNT,
	},
	[BLK_IDX_SGMII] = { 0 },
};

/* SJA1105P: Second generation, no TTEthernet, no SGMII */
static struct sja1105_table_ops sja1105p_table_ops[BLK_IDX_MAX] = {
	[BLK_IDX_SCHEDULE] = { 0 },
	[BLK_IDX_SCHEDULE_ENTRY_POINTS] = { 0 },
	[BLK_IDX_VL_LOOKUP] = { 0 },
	[BLK_IDX_VL_POLICING] = { 0 },
	[BLK_IDX_VL_FORWARDING] = { 0 },
	[BLK_IDX_L2_LOOKUP] = {
		.packing = sja1105pqrs_l2_lookup_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_lookup_entry),
		.packed_entry_size = SIZE_L2_LOOKUP_ENTRY_PQRS,
		.max_entry_count = MAX_L2_LOOKUP_COUNT,
	},
	[BLK_IDX_L2_POLICING] = {
		.packing = sja1105_l2_policing_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_policing_entry),
		.packed_entry_size = SIZE_L2_POLICING_ENTRY,
		.max_entry_count = MAX_L2_POLICING_COUNT,
	},
	[BLK_IDX_VLAN_LOOKUP] = {
		.packing = sja1105_vlan_lookup_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vlan_lookup_entry),
		.packed_entry_size = SIZE_VLAN_LOOKUP_ENTRY,
		.max_entry_count = MAX_VLAN_LOOKUP_COUNT,
	},
	[BLK_IDX_L2_FORWARDING] = {
		.packing = sja1105_l2_forwarding_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_forwarding_entry),
		.packed_entry_size = SIZE_L2_FORWARDING_ENTRY,
		.max_entry_count = MAX_L2_FORWARDING_COUNT,
	},
	[BLK_IDX_MAC_CONFIG] = {
		.packing = sja1105pqrs_mac_config_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_mac_config_entry),
		.packed_entry_size = SIZE_MAC_CONFIG_ENTRY_PQRS,
		.max_entry_count = MAX_MAC_CONFIG_COUNT,
	},
	[BLK_IDX_SCHEDULE_PARAMS] = { 0 },
	[BLK_IDX_SCHEDULE_ENTRY_POINTS_PARAMS] = { 0 },
	[BLK_IDX_VL_FORWARDING_PARAMS] = { 0 },
	[BLK_IDX_L2_LOOKUP_PARAMS] = {
		.packing = sja1105pqrs_l2_lookup_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_lookup_params_entry),
		.packed_entry_size = SIZE_L2_LOOKUP_PARAMS_ENTRY_PQRS,
		.max_entry_count = MAX_L2_LOOKUP_PARAMS_COUNT,
	},
	[BLK_IDX_L2_FORWARDING_PARAMS] = {
		.packing = sja1105_l2_forwarding_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_forwarding_params_entry),
		.packed_entry_size = SIZE_L2_FORWARDING_PARAMS_ENTRY,
		.max_entry_count = MAX_L2_FORWARDING_PARAMS_COUNT,
	},
	[BLK_IDX_CLK_SYNC_PARAMS] = { 0 },
	[BLK_IDX_AVB_PARAMS] = {
		.packing = sja1105pqrs_avb_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_avb_params_entry),
		.packed_entry_size = SIZE_AVB_PARAMS_ENTRY_PQRS,
		.max_entry_count = MAX_AVB_PARAMS_COUNT,
	},
	[BLK_IDX_GENERAL_PARAMS] = {
		.packing = sja1105pqrs_general_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_general_params_entry),
		.packed_entry_size = SIZE_GENERAL_PARAMS_ENTRY_PQRS,
		.max_entry_count = MAX_GENERAL_PARAMS_COUNT,
	},
	[BLK_IDX_RETAGGING] = {
		.packing = sja1105_retagging_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_retagging_entry),
		.packed_entry_size = SIZE_RETAGGING_ENTRY,
		.max_entry_count = MAX_RETAGGING_COUNT,
	},
	[BLK_IDX_XMII_PARAMS] = {
		.packing = sja1105_xmii_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_xmii_params_entry),
		.packed_entry_size = SIZE_XMII_PARAMS_ENTRY,
		.max_entry_count = MAX_XMII_PARAMS_COUNT,
	},
	[BLK_IDX_SGMII] = { 0 },
};

/* SJA1105Q: Second generation, TTEthernet, no SGMII */
static struct sja1105_table_ops sja1105q_table_ops[BLK_IDX_MAX] = {
	[BLK_IDX_SCHEDULE] = {
		.packing = sja1105_schedule_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_schedule_entry),
		.packed_entry_size = SIZE_SCHEDULE_ENTRY,
		.max_entry_count = MAX_SCHEDULE_COUNT,
	},
	[BLK_IDX_SCHEDULE_ENTRY_POINTS] = {
		.packing = sja1105_schedule_entry_points_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_schedule_entry_points_entry),
		.packed_entry_size = SIZE_SCHEDULE_ENTRY_POINTS_ENTRY,
		.max_entry_count = MAX_SCHEDULE_ENTRY_POINTS_COUNT,
	},
	[BLK_IDX_VL_LOOKUP] = {
		.packing = sja1105_vl_lookup_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vl_lookup_entry),
		.packed_entry_size = SIZE_VL_LOOKUP_ENTRY,
		.max_entry_count = MAX_VL_LOOKUP_COUNT,
	},
	[BLK_IDX_VL_POLICING] = {
		.packing = sja1105_vl_policing_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vl_policing_entry),
		.packed_entry_size = SIZE_VL_POLICING_ENTRY,
		.max_entry_count = MAX_VL_POLICING_COUNT,
	},
	[BLK_IDX_VL_FORWARDING] = {
		.packing = sja1105_vl_forwarding_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vl_forwarding_entry),
		.packed_entry_size = SIZE_VL_FORWARDING_ENTRY,
		.max_entry_count = MAX_VL_FORWARDING_COUNT,
	},
	[BLK_IDX_L2_LOOKUP] = {
		.packing = sja1105pqrs_l2_lookup_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_lookup_entry),
		.packed_entry_size = SIZE_L2_LOOKUP_ENTRY_PQRS,
		.max_entry_count = MAX_L2_LOOKUP_COUNT,
	},
	[BLK_IDX_L2_POLICING] = {
		.packing = sja1105_l2_policing_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_policing_entry),
		.packed_entry_size = SIZE_L2_POLICING_ENTRY,
		.max_entry_count = MAX_L2_POLICING_COUNT,
	},
	[BLK_IDX_VLAN_LOOKUP] = {
		.packing = sja1105_vlan_lookup_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vlan_lookup_entry),
		.packed_entry_size = SIZE_VLAN_LOOKUP_ENTRY,
		.max_entry_count = MAX_VLAN_LOOKUP_COUNT,
	},
	[BLK_IDX_L2_FORWARDING] = {
		.packing = sja1105_l2_forwarding_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_forwarding_entry),
		.packed_entry_size = SIZE_L2_FORWARDING_ENTRY,
		.max_entry_count = MAX_L2_FORWARDING_COUNT,
	},
	[BLK_IDX_MAC_CONFIG] = {
		.packing = sja1105pqrs_mac_config_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_mac_config_entry),
		.packed_entry_size = SIZE_MAC_CONFIG_ENTRY_PQRS,
		.max_entry_count = MAX_MAC_CONFIG_COUNT,
	},
	[BLK_IDX_SCHEDULE_PARAMS] = {
		.packing = sja1105_schedule_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_schedule_params_entry),
		.packed_entry_size = SIZE_SCHEDULE_PARAMS_ENTRY,
		.max_entry_count = MAX_SCHEDULE_PARAMS_COUNT,
	},
	[BLK_IDX_SCHEDULE_ENTRY_POINTS_PARAMS] = {
		.packing = sja1105_schedule_entry_points_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_schedule_entry_points_params_entry),
		.packed_entry_size = SIZE_SCHEDULE_ENTRY_POINTS_PARAMS_ENTRY,
		.max_entry_count = MAX_SCHEDULE_ENTRY_POINTS_PARAMS_COUNT,
	},
	[BLK_IDX_VL_FORWARDING_PARAMS] = {
		.packing = sja1105_vl_forwarding_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vl_forwarding_params_entry),
		.packed_entry_size = SIZE_VL_FORWARDING_PARAMS_ENTRY,
		.max_entry_count = MAX_VL_FORWARDING_PARAMS_COUNT,
	},
	[BLK_IDX_L2_LOOKUP_PARAMS] = {
		.packing = sja1105pqrs_l2_lookup_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_lookup_params_entry),
		.packed_entry_size = SIZE_L2_LOOKUP_PARAMS_ENTRY_PQRS,
		.max_entry_count = MAX_L2_LOOKUP_PARAMS_COUNT,
	},
	[BLK_IDX_L2_FORWARDING_PARAMS] = {
		.packing = sja1105_l2_forwarding_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_forwarding_params_entry),
		.packed_entry_size = SIZE_L2_FORWARDING_PARAMS_ENTRY,
		.max_entry_count = MAX_L2_FORWARDING_PARAMS_COUNT,
	},
	[BLK_IDX_CLK_SYNC_PARAMS] = {
		.packing = sja1105_clk_sync_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_clk_sync_params_entry),
		.packed_entry_size = SIZE_CLK_SYNC_PARAMS_ENTRY,
		.max_entry_count = MAX_CLK_SYNC_COUNT,
	},
	[BLK_IDX_AVB_PARAMS] = {
		.packing = sja1105pqrs_avb_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_avb_params_entry),
		.packed_entry_size = SIZE_AVB_PARAMS_ENTRY_PQRS,
		.max_entry_count = MAX_AVB_PARAMS_COUNT,
	},
	[BLK_IDX_GENERAL_PARAMS] = {
		.packing = sja1105pqrs_general_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_general_params_entry),
		.packed_entry_size = SIZE_GENERAL_PARAMS_ENTRY_PQRS,
		.max_entry_count = MAX_GENERAL_PARAMS_COUNT,
	},
	[BLK_IDX_RETAGGING] = {
		.packing = sja1105_retagging_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_retagging_entry),
		.packed_entry_size = SIZE_RETAGGING_ENTRY,
		.max_entry_count = MAX_RETAGGING_COUNT,
	},
	[BLK_IDX_XMII_PARAMS] = {
		.packing = sja1105_xmii_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_xmii_params_entry),
		.packed_entry_size = SIZE_XMII_PARAMS_ENTRY,
		.max_entry_count = MAX_XMII_PARAMS_COUNT,
	},
	[BLK_IDX_SGMII] = { 0 },
};

/* SJA1105R: Second generation, no TTEthernet, SGMII */
static struct sja1105_table_ops sja1105r_table_ops[BLK_IDX_MAX] = {
	[BLK_IDX_SCHEDULE] = { 0 },
	[BLK_IDX_SCHEDULE_ENTRY_POINTS] = { 0 },
	[BLK_IDX_VL_LOOKUP] = { 0 },
	[BLK_IDX_VL_POLICING] = { 0 },
	[BLK_IDX_VL_FORWARDING] = { 0 },
	[BLK_IDX_L2_LOOKUP] = {
		.packing = sja1105pqrs_l2_lookup_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_lookup_entry),
		.packed_entry_size = SIZE_L2_LOOKUP_ENTRY_PQRS,
		.max_entry_count = MAX_L2_LOOKUP_COUNT,
	},
	[BLK_IDX_L2_POLICING] = {
		.packing = sja1105_l2_policing_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_policing_entry),
		.packed_entry_size = SIZE_L2_POLICING_ENTRY,
		.max_entry_count = MAX_L2_POLICING_COUNT,
	},
	[BLK_IDX_VLAN_LOOKUP] = {
		.packing = sja1105_vlan_lookup_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vlan_lookup_entry),
		.packed_entry_size = SIZE_VLAN_LOOKUP_ENTRY,
		.max_entry_count = MAX_VLAN_LOOKUP_COUNT,
	},
	[BLK_IDX_L2_FORWARDING] = {
		.packing = sja1105_l2_forwarding_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_forwarding_entry),
		.packed_entry_size = SIZE_L2_FORWARDING_ENTRY,
		.max_entry_count = MAX_L2_FORWARDING_COUNT,
	},
	[BLK_IDX_MAC_CONFIG] = {
		.packing = sja1105pqrs_mac_config_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_mac_config_entry),
		.packed_entry_size = SIZE_MAC_CONFIG_ENTRY_PQRS,
		.max_entry_count = MAX_MAC_CONFIG_COUNT,
	},
	[BLK_IDX_SCHEDULE_PARAMS] = { 0 },
	[BLK_IDX_SCHEDULE_ENTRY_POINTS_PARAMS] = { 0 },
	[BLK_IDX_VL_FORWARDING_PARAMS] = { 0 },
	[BLK_IDX_L2_LOOKUP_PARAMS] = {
		.packing = sja1105pqrs_l2_lookup_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_lookup_params_entry),
		.packed_entry_size = SIZE_L2_LOOKUP_PARAMS_ENTRY_PQRS,
		.max_entry_count = MAX_L2_LOOKUP_PARAMS_COUNT,
	},
	[BLK_IDX_L2_FORWARDING_PARAMS] = {
		.packing = sja1105_l2_forwarding_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_forwarding_params_entry),
		.packed_entry_size = SIZE_L2_FORWARDING_PARAMS_ENTRY,
		.max_entry_count = MAX_L2_FORWARDING_PARAMS_COUNT,
	},
	[BLK_IDX_CLK_SYNC_PARAMS] = { 0 },
	[BLK_IDX_AVB_PARAMS] = {
		.packing = sja1105pqrs_avb_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_avb_params_entry),
		.packed_entry_size = SIZE_AVB_PARAMS_ENTRY_PQRS,
		.max_entry_count = MAX_AVB_PARAMS_COUNT,
	},
	[BLK_IDX_GENERAL_PARAMS] = {
		.packing = sja1105pqrs_general_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_general_params_entry),
		.packed_entry_size = SIZE_GENERAL_PARAMS_ENTRY_PQRS,
		.max_entry_count = MAX_GENERAL_PARAMS_COUNT,
	},
	[BLK_IDX_RETAGGING] = {
		.packing = sja1105_retagging_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_retagging_entry),
		.packed_entry_size = SIZE_RETAGGING_ENTRY,
		.max_entry_count = MAX_RETAGGING_COUNT,
	},
	[BLK_IDX_XMII_PARAMS] = {
		.packing = sja1105_xmii_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_xmii_params_entry),
		.packed_entry_size = SIZE_XMII_PARAMS_ENTRY,
		.max_entry_count = MAX_XMII_PARAMS_COUNT,
	},
	[BLK_IDX_SGMII] = {
		.packing = sja1105_sgmii_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_sgmii_entry),
		.packed_entry_size = SIZE_SGMII_ENTRY,
		.max_entry_count = MAX_SGMII_COUNT,
	},
};

/* SJA1105S: Second generation, TTEthernet, SGMII */
static struct sja1105_table_ops sja1105s_table_ops[BLK_IDX_MAX] = {
	[BLK_IDX_SCHEDULE] = {
		.packing = sja1105_schedule_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_schedule_entry),
		.packed_entry_size = SIZE_SCHEDULE_ENTRY,
		.max_entry_count = MAX_SCHEDULE_COUNT,
	},
	[BLK_IDX_SCHEDULE_ENTRY_POINTS] = {
		.packing = sja1105_schedule_entry_points_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_schedule_entry_points_entry),
		.packed_entry_size = SIZE_SCHEDULE_ENTRY_POINTS_ENTRY,
		.max_entry_count = MAX_SCHEDULE_ENTRY_POINTS_COUNT,
	},
	[BLK_IDX_VL_LOOKUP] = {
		.packing = sja1105_vl_lookup_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vl_lookup_entry),
		.packed_entry_size = SIZE_VL_LOOKUP_ENTRY,
		.max_entry_count = MAX_VL_LOOKUP_COUNT,
	},
	[BLK_IDX_VL_POLICING] = {
		.packing = sja1105_vl_policing_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vl_policing_entry),
		.packed_entry_size = SIZE_VL_POLICING_ENTRY,
		.max_entry_count = MAX_VL_POLICING_COUNT,
	},
	[BLK_IDX_VL_FORWARDING] = {
		.packing = sja1105_vl_forwarding_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vl_forwarding_entry),
		.packed_entry_size = SIZE_VL_FORWARDING_ENTRY,
		.max_entry_count = MAX_VL_FORWARDING_COUNT,
	},
	[BLK_IDX_L2_LOOKUP] = {
		.packing = sja1105pqrs_l2_lookup_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_lookup_entry),
		.packed_entry_size = SIZE_L2_LOOKUP_ENTRY_PQRS,
		.max_entry_count = MAX_L2_LOOKUP_COUNT,
	},
	[BLK_IDX_L2_POLICING] = {
		.packing = sja1105_l2_policing_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_policing_entry),
		.packed_entry_size = SIZE_L2_POLICING_ENTRY,
		.max_entry_count = MAX_L2_POLICING_COUNT,
	},
	[BLK_IDX_VLAN_LOOKUP] = {
		.packing = sja1105_vlan_lookup_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vlan_lookup_entry),
		.packed_entry_size = SIZE_VLAN_LOOKUP_ENTRY,
		.max_entry_count = MAX_VLAN_LOOKUP_COUNT,
	},
	[BLK_IDX_L2_FORWARDING] = {
		.packing = sja1105_l2_forwarding_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_forwarding_entry),
		.packed_entry_size = SIZE_L2_FORWARDING_ENTRY,
		.max_entry_count = MAX_L2_FORWARDING_COUNT,
	},
	[BLK_IDX_MAC_CONFIG] = {
		.packing = sja1105pqrs_mac_config_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_mac_config_entry),
		.packed_entry_size = SIZE_MAC_CONFIG_ENTRY_PQRS,
		.max_entry_count = MAX_MAC_CONFIG_COUNT,
	},
	[BLK_IDX_SCHEDULE_PARAMS] = {
		.packing = sja1105_schedule_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_schedule_params_entry),
		.packed_entry_size = SIZE_SCHEDULE_PARAMS_ENTRY,
		.max_entry_count = MAX_SCHEDULE_PARAMS_COUNT,
	},
	[BLK_IDX_SCHEDULE_ENTRY_POINTS_PARAMS] = {
		.packing = sja1105_schedule_entry_points_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_schedule_entry_points_params_entry),
		.packed_entry_size = SIZE_SCHEDULE_ENTRY_POINTS_PARAMS_ENTRY,
		.max_entry_count = MAX_SCHEDULE_ENTRY_POINTS_PARAMS_COUNT,
	},
	[BLK_IDX_VL_FORWARDING_PARAMS] = {
		.packing = sja1105_vl_forwarding_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_vl_forwarding_params_entry),
		.packed_entry_size = SIZE_VL_FORWARDING_PARAMS_ENTRY,
		.max_entry_count = MAX_VL_FORWARDING_PARAMS_COUNT,
	},
	[BLK_IDX_L2_LOOKUP_PARAMS] = {
		.packing = sja1105pqrs_l2_lookup_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_lookup_params_entry),
		.packed_entry_size = SIZE_L2_LOOKUP_PARAMS_ENTRY_PQRS,
		.max_entry_count = MAX_L2_LOOKUP_PARAMS_COUNT,
	},
	[BLK_IDX_L2_FORWARDING_PARAMS] = {
		.packing = sja1105_l2_forwarding_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_l2_forwarding_params_entry),
		.packed_entry_size = SIZE_L2_FORWARDING_PARAMS_ENTRY,
		.max_entry_count = MAX_L2_FORWARDING_PARAMS_COUNT,
	},
	[BLK_IDX_CLK_SYNC_PARAMS] = {
		.packing = sja1105_clk_sync_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_clk_sync_params_entry),
		.packed_entry_size = SIZE_CLK_SYNC_PARAMS_ENTRY,
		.max_entry_count = MAX_CLK_SYNC_COUNT,
	},
	[BLK_IDX_AVB_PARAMS] = {
		.packing = sja1105pqrs_avb_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_avb_params_entry),
		.packed_entry_size = SIZE_AVB_PARAMS_ENTRY_PQRS,
		.max_entry_count = MAX_AVB_PARAMS_COUNT,
	},
	[BLK_IDX_GENERAL_PARAMS] = {
		.packing = sja1105pqrs_general_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_general_params_entry),
		.packed_entry_size = SIZE_GENERAL_PARAMS_ENTRY_PQRS,
		.max_entry_count = MAX_GENERAL_PARAMS_COUNT,
	},
	[BLK_IDX_RETAGGING] = {
		.packing = sja1105_retagging_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_retagging_entry),
		.packed_entry_size = SIZE_RETAGGING_ENTRY,
		.max_entry_count = MAX_RETAGGING_COUNT,
	},
	[BLK_IDX_XMII_PARAMS] = {
		.packing = sja1105_xmii_params_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_xmii_params_entry),
		.packed_entry_size = SIZE_XMII_PARAMS_ENTRY,
		.max_entry_count = MAX_XMII_PARAMS_COUNT,
	},
	[BLK_IDX_SGMII] = {
		.packing = sja1105_sgmii_entry_packing,
		.unpacked_entry_size = sizeof(struct sja1105_sgmii_entry),
		.packed_entry_size = SIZE_SGMII_ENTRY,
		.max_entry_count = MAX_SGMII_COUNT,
	},
};

int sja1105_static_config_init(struct sja1105_static_config *config,
			       u64 device_id, u64 part_nr)
{
	const struct sja1105_table_ops *ops;
	enum sja1105_blk_idx i;

	memset(config, 0, sizeof(*config));

	if (device_id == SJA1105E_DEVICE_ID)
		ops = sja1105e_table_ops;
	else if (device_id == SJA1105T_DEVICE_ID)
		ops = sja1105t_table_ops;
	else if (IS_P(device_id, part_nr))
		ops = sja1105p_table_ops;
	else if (IS_Q(device_id, part_nr))
		ops = sja1105q_table_ops;
	else if (IS_R(device_id, part_nr))
		ops = sja1105r_table_ops;
	else if (IS_S(device_id, part_nr))
		ops = sja1105s_table_ops;
	else
		return -EINVAL;

	for (i = 0; i < BLK_IDX_MAX; i++)
		config->tables[i].ops = &ops[i];

	config->device_id = device_id;
	return 0;
}

void sja1105_static_config_free(struct sja1105_static_config *config)
{
	enum sja1105_blk_idx i;

	for (i = 0; i < BLK_IDX_MAX; i++) {
		if (config->tables[i].entry_count) {
			kfree(config->tables[i].entries);
			config->tables[i].entry_count = 0;
		}
	}
}

int sja1105_table_delete_entry(struct sja1105_table *table, int i)
{
	size_t entry_size = table->ops->unpacked_entry_size;
	u8 *entries = table->entries;

	if (i > table->entry_count)
		return -ERANGE;

	memmove(entries + i * entry_size, entries + (i + 1) * entry_size,
		(table->entry_count - i) * entry_size);

	table->entry_count--;

	return 0;
}

/* No pointers to table->entries should be kept when this is called. */
int sja1105_table_resize(struct sja1105_table *table, size_t new_count)
{
	size_t entry_size = table->ops->unpacked_entry_size;
	void *new_entries, *old_entries = table->entries;

	if (new_count > table->ops->max_entry_count)
		return -ERANGE;

	new_entries = kcalloc(new_count, entry_size, GFP_KERNEL);
	if (!new_entries)
		return -ENOMEM;

	memcpy(new_entries, old_entries, min(new_count, table->entry_count) *
		entry_size);

	table->entries = new_entries;
	table->entry_count = new_count;
	kfree(old_entries);
	return 0;
}

