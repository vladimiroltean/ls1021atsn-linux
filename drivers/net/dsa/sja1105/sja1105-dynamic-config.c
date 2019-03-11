// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2019, Vladimir Oltean <olteanv@gmail.com>
 */
#include "sja1105.h"

#define SIZE_DYN_CMD                     4
#define SIZE_MAC_CONFIG_DYN_ENTRY_ET     SIZE_DYN_CMD
#define SIZE_VL_LOOKUP_DYN_CMD_ET        SIZE_DYN_CMD
#define SIZE_VL_LOOKUP_DYN_CMD_PQRS     (SIZE_DYN_CMD + SIZE_VL_LOOKUP_ENTRY)
#define SIZE_L2_LOOKUP_DYN_CMD_ET       (SIZE_DYN_CMD + SIZE_L2_LOOKUP_ENTRY_ET)
#define SIZE_L2_LOOKUP_DYN_CMD_PQRS     (SIZE_DYN_CMD + SIZE_L2_LOOKUP_ENTRY_PQRS)
#define SIZE_VLAN_LOOKUP_DYN_CMD        (SIZE_DYN_CMD + 4 + SIZE_VLAN_LOOKUP_ENTRY)
#define SIZE_L2_FORWARDING_DYN_CMD      (SIZE_DYN_CMD + SIZE_L2_FORWARDING_ENTRY)
#define SIZE_MAC_CONFIG_DYN_CMD_ET      (SIZE_DYN_CMD + SIZE_MAC_CONFIG_DYN_ENTRY_ET)
#define SIZE_MAC_CONFIG_DYN_CMD_PQRS    (SIZE_DYN_CMD + SIZE_MAC_CONFIG_ENTRY_PQRS)
#define SIZE_L2_LOOKUP_PARAMS_DYN_CMD_ET SIZE_DYN_CMD
#define SIZE_GENERAL_PARAMS_DYN_CMD_ET   SIZE_DYN_CMD
#define SIZE_RETAGGING_DYN_CMD_ET       (SIZE_DYN_CMD + SIZE_RETAGGING_ENTRY)
#define MAX_DYN_CMD_SIZE                 SIZE_MAC_CONFIG_DYN_CMD_PQRS

static void
sja1105_vl_lookup_cmd_packing(void *buf, struct sja1105_dyn_cmd *cmd,
			      enum packing_op op)
{
	sja1105_packing(buf, &cmd->valid,   31, 31, SIZE_DYN_CMD, op);
	sja1105_packing(buf, &cmd->errors,  30, 30, SIZE_DYN_CMD, op);
	sja1105_packing(buf, &cmd->rdwrset, 29, 29, SIZE_DYN_CMD, op);
	sja1105_packing(buf, &cmd->index,    9,  0, SIZE_DYN_CMD, op);
}

static size_t sja1105et_vl_lookup_entry_packing(void *buf, void *entry_ptr,
						enum packing_op op)
{
	struct sja1105_vl_lookup_entry *entry = entry_ptr;
	const int size = SIZE_VL_LOOKUP_DYN_CMD_ET;

	sja1105_packing(buf, &entry->egrmirr,  21, 17, size, op);
	sja1105_packing(buf, &entry->ingrmirr, 16, 16, size, op);
	return size;
}

static void
sja1105pqrs_l2_lookup_cmd_packing(void *buf, struct sja1105_dyn_cmd *cmd,
				  enum packing_op op)
{
	u8 *p = buf + SIZE_L2_LOOKUP_ENTRY_PQRS;

	sja1105_packing(p, &cmd->valid,    31, 31, SIZE_DYN_CMD, op);
	sja1105_packing(p, &cmd->rdwrset,  30, 30, SIZE_DYN_CMD, op);
	sja1105_packing(p, &cmd->errors,   29, 29, SIZE_DYN_CMD, op);
	sja1105_packing(p, &cmd->valident, 27, 27, SIZE_DYN_CMD, op);
	/* Hack - The hardware takes the 'index' field within
	 * struct sja1105_l2_lookup_entry as the index on which this command
	 * will operate. However it will ignore everything else, so 'index'
	 * is logically part of command but physically part of entry.
	 * Populate the 'index' entry field from within the command callback,
	 * such that our API doesn't need to ask for a full-blown entry
	 * structure when e.g. a delete is requested.
	 */
	sja1105_packing(buf, &cmd->index, 29, 20, SIZE_L2_LOOKUP_ENTRY_PQRS, op);
	/* TODO hostcmd */
}

static void
sja1105et_l2_lookup_cmd_packing(void *buf, struct sja1105_dyn_cmd *cmd,
				enum packing_op op)
{
	u8 *p = buf + SIZE_L2_LOOKUP_ENTRY_ET;

	sja1105_packing(p, &cmd->valid,    31, 31, SIZE_DYN_CMD, op);
	sja1105_packing(p, &cmd->rdwrset,  30, 30, SIZE_DYN_CMD, op);
	sja1105_packing(p, &cmd->errors,   29, 29, SIZE_DYN_CMD, op);
	sja1105_packing(p, &cmd->valident, 27, 27, SIZE_DYN_CMD, op);
	/* Hack - see comments above. */
	sja1105_packing(buf, &cmd->index, 29, 20, SIZE_L2_LOOKUP_ENTRY_ET, op);
}

/* In E/T, entry is at addresses 0x27-0x28. There is a 4 byte gap at 0x29,
 * and command is at 0x2a. Similarly in P/Q/R/S there is a 1 register gap
 * between entry (0x2d, 0x2e) and command (0x30).
 */
static void
sja1105_vlan_lookup_cmd_packing(void *buf, struct sja1105_dyn_cmd *cmd,
				enum packing_op op)
{
	u8 *p = buf + SIZE_VLAN_LOOKUP_ENTRY + 4;

	sja1105_packing(p, &cmd->valid,    31, 31, SIZE_DYN_CMD, op);
	sja1105_packing(p, &cmd->rdwrset,  30, 30, SIZE_DYN_CMD, op);
	sja1105_packing(p, &cmd->valident, 27, 27, SIZE_DYN_CMD, op);
	/* Hack - see comments above, applied for 'vlanid' field of
	 * struct sja1105_vlan_lookup_entry.
	 */
	sja1105_packing(buf, &cmd->index, 38, 27, SIZE_VLAN_LOOKUP_ENTRY, op);
}

static void
sja1105_l2_forwarding_cmd_packing(void *buf, struct sja1105_dyn_cmd *cmd,
				  enum packing_op op)
{
	u8 *p = buf + SIZE_L2_FORWARDING_ENTRY;

	sja1105_packing(p, &cmd->valid,   31, 31, SIZE_DYN_CMD, op);
	sja1105_packing(p, &cmd->errors,  30, 30, SIZE_DYN_CMD, op);
	sja1105_packing(p, &cmd->rdwrset, 29, 29, SIZE_DYN_CMD, op);
	sja1105_packing(p, &cmd->index,    4,  0, SIZE_DYN_CMD, op);
}

static void
sja1105et_mac_config_cmd_packing(void *buf, struct sja1105_dyn_cmd *cmd,
				 enum packing_op op)
{
	/* Yup, user manual definitions are reversed */
	u8 *reg1 = buf + 4;

	sja1105_packing(reg1, &cmd->valid, 31, 31, SIZE_DYN_CMD, op);
	sja1105_packing(reg1, &cmd->index, 26, 24, SIZE_DYN_CMD, op);
}

static size_t sja1105et_mac_config_entry_packing(void *buf, void *entry_ptr,
						 enum packing_op op)
{
	struct sja1105_mac_config_entry *entry = entry_ptr;
	const int size = SIZE_MAC_CONFIG_DYN_ENTRY_ET;
	/* Yup, user manual definitions are reversed */
	u8 *reg1 = buf + 4;
	u8 *reg2 = buf;

	sja1105_packing(reg1, &entry->speed,     30, 29, size, op);
	sja1105_packing(reg1, &entry->drpdtag,   23, 23, size, op);
	sja1105_packing(reg1, &entry->drpuntag,  22, 22, size, op);
	sja1105_packing(reg1, &entry->retag,     21, 21, size, op);
	sja1105_packing(reg1, &entry->dyn_learn, 20, 20, size, op);
	sja1105_packing(reg1, &entry->egress,    19, 19, size, op);
	sja1105_packing(reg1, &entry->ingress,   18, 18, size, op);
	sja1105_packing(reg1, &entry->ing_mirr,  17, 17, size, op);
	sja1105_packing(reg1, &entry->egr_mirr,  16, 16, size, op);
	sja1105_packing(reg1, &entry->vlanprio,  14, 12, size, op);
	sja1105_packing(reg1, &entry->vlanid,    11,  0, size, op);
	sja1105_packing(reg2, &entry->tp_delin,  31, 16, size, op);
	sja1105_packing(reg2, &entry->tp_delout, 15,  0, size, op);
	/* MAC configuration table entries which can't be reconfigured:
	 * top, base, enabled, ifg, maxage, drpnona664
	 */
	/* Bogus return value, not used anywhere */
	return 0;
}

static void
sja1105pqrs_mac_config_cmd_packing(void *buf, struct sja1105_dyn_cmd *cmd,
				   enum packing_op op)
{
	u8 *p = buf + SIZE_MAC_CONFIG_ENTRY_PQRS;

	sja1105_packing(p, &cmd->valid,   31, 31, SIZE_DYN_CMD, op);
	sja1105_packing(p, &cmd->errors,  30, 30, SIZE_DYN_CMD, op);
	sja1105_packing(p, &cmd->rdwrset, 29, 29, SIZE_DYN_CMD, op);
	sja1105_packing(p, &cmd->index,    2,  0, SIZE_DYN_CMD, op);
}

static void
sja1105et_l2_lookup_params_cmd_packing(void *buf, struct sja1105_dyn_cmd *cmd,
				       enum packing_op op)
{
	sja1105_packing(buf, &cmd->valid, 31, 31,
			SIZE_L2_LOOKUP_PARAMS_DYN_CMD_ET, op);
}

static size_t
sja1105et_l2_lookup_params_entry_packing(void *buf, void *entry_ptr,
					 enum packing_op op)
{
	struct sja1105_l2_lookup_params_entry *entry = entry_ptr;

	sja1105_packing(buf, &entry->poly, 7, 0,
			SIZE_L2_LOOKUP_PARAMS_DYN_CMD_ET, op);
	/* Bogus return value, not used anywhere */
	return 0;
}

static void
sja1105et_general_params_cmd_packing(void *buf, struct sja1105_dyn_cmd *cmd,
				enum packing_op op)
{
	const int size = SIZE_GENERAL_PARAMS_DYN_CMD_ET;

	sja1105_packing(buf, &cmd->valid,  31, 31, size, op);
	sja1105_packing(buf, &cmd->errors, 30, 30, size, op);
}

static size_t
sja1105et_general_params_entry_packing(void *buf, void *entry_ptr,
				       enum packing_op op)
{
	struct sja1105_general_params_entry *entry = entry_ptr;
	const int size = SIZE_GENERAL_PARAMS_DYN_CMD_ET;

	sja1105_packing(buf, &entry->mirr_port, 2, 0, size, op);
	/* Bogus return value, not used anywhere */
	return 0;
}

static void
sja1105_retagging_cmd_packing(void *buf, struct sja1105_dyn_cmd *cmd,
			      enum packing_op op)
{
	sja1105_packing(buf, &cmd->valid,    31, 31, SIZE_DYN_CMD, op);
	sja1105_packing(buf, &cmd->errors,   30, 30, SIZE_DYN_CMD, op);
	sja1105_packing(buf, &cmd->valident, 29, 29, SIZE_DYN_CMD, op);
	sja1105_packing(buf, &cmd->index,     5,  0, SIZE_DYN_CMD, op);
}

#define OP_READ  BIT(0)
#define OP_WRITE BIT(1)
#define OP_DEL   BIT(2)

/* SJA1105E/T: First generation */
static struct sja1105_dynamic_table_ops sja1105et_table_ops[BLK_IDX_MAX] = {
	[BLK_IDX_SCHEDULE] = { 0 },
	[BLK_IDX_SCHEDULE_ENTRY_POINTS] = { 0 },
	[BLK_IDX_VL_LOOKUP] = {
		.entry_packing = sja1105et_vl_lookup_entry_packing,
		.cmd_packing = sja1105_vl_lookup_cmd_packing,
		.access = OP_WRITE,
		.max_entry_count = MAX_VL_LOOKUP_COUNT,
		.packed_size = SIZE_VL_LOOKUP_DYN_CMD_ET,
		.addr = 0x35,
	},
	[BLK_IDX_VL_POLICING] = { 0 },
	[BLK_IDX_VL_FORWARDING] = { 0 },
	[BLK_IDX_L2_LOOKUP] = {
		.entry_packing = sja1105et_l2_lookup_entry_packing,
		.cmd_packing = sja1105et_l2_lookup_cmd_packing,
		.access = (OP_READ | OP_WRITE | OP_DEL),
		.max_entry_count = MAX_L2_LOOKUP_COUNT,
		.packed_size = SIZE_L2_LOOKUP_DYN_CMD_ET,
		.addr = 0x20,
	},
	[BLK_IDX_L2_POLICING] = { 0 },
	[BLK_IDX_VLAN_LOOKUP] = {
		.entry_packing = sja1105_vlan_lookup_entry_packing,
		.cmd_packing = sja1105_vlan_lookup_cmd_packing,
		.access = (OP_WRITE | OP_DEL),
		.max_entry_count = MAX_VLAN_LOOKUP_COUNT,
		.packed_size = SIZE_VLAN_LOOKUP_DYN_CMD,
		.addr = 0x27,
	},
	[BLK_IDX_L2_FORWARDING] = {
		.entry_packing = sja1105_l2_forwarding_entry_packing,
		.cmd_packing = sja1105_l2_forwarding_cmd_packing,
		.max_entry_count = MAX_L2_FORWARDING_COUNT,
		.access = OP_WRITE,
		.packed_size = SIZE_L2_FORWARDING_DYN_CMD,
		.addr = 0x24,
	},
	[BLK_IDX_MAC_CONFIG] = {
		.entry_packing = sja1105et_mac_config_entry_packing,
		.cmd_packing = sja1105et_mac_config_cmd_packing,
		.max_entry_count = MAX_MAC_CONFIG_COUNT,
		.access = OP_WRITE,
		.packed_size = SIZE_MAC_CONFIG_DYN_CMD_ET,
		.addr = 0x36,
	},
	[BLK_IDX_SCHEDULE_PARAMS] = { 0 },
	[BLK_IDX_SCHEDULE_ENTRY_POINTS_PARAMS] = { 0 },
	[BLK_IDX_VL_FORWARDING_PARAMS] = { 0 },
	[BLK_IDX_L2_LOOKUP_PARAMS] = {
		.entry_packing = sja1105et_l2_lookup_params_entry_packing,
		.cmd_packing = sja1105et_l2_lookup_params_cmd_packing,
		.max_entry_count = MAX_L2_LOOKUP_PARAMS_COUNT,
		.access = OP_WRITE,
		.packed_size = SIZE_L2_LOOKUP_PARAMS_DYN_CMD_ET,
		.addr = 0x38,
	},
	[BLK_IDX_L2_FORWARDING_PARAMS] = { 0 },
	[BLK_IDX_CLK_SYNC_PARAMS] = { 0 },
	[BLK_IDX_AVB_PARAMS] = { 0 },
	[BLK_IDX_GENERAL_PARAMS] = {
		.entry_packing = sja1105et_general_params_entry_packing,
		.cmd_packing = sja1105et_general_params_cmd_packing,
		.max_entry_count = MAX_GENERAL_PARAMS_COUNT,
		.access = OP_WRITE,
		.packed_size = SIZE_GENERAL_PARAMS_DYN_CMD_ET,
		.addr = 0x34,
	},
	[BLK_IDX_RETAGGING] = {
		.entry_packing = sja1105_retagging_entry_packing,
		.cmd_packing = sja1105_retagging_cmd_packing,
		.max_entry_count = MAX_RETAGGING_COUNT,
		.access = (OP_WRITE | OP_DEL),
		.packed_size = SIZE_RETAGGING_DYN_CMD_ET,
		.addr = 0x31,
	},
	[BLK_IDX_XMII_PARAMS] = { 0 },
	[BLK_IDX_SGMII] = { 0 },
};

/* SJA1105P/Q/R/S: Second generation: TODO */
static struct sja1105_dynamic_table_ops sja1105pqrs_table_ops[BLK_IDX_MAX] = {
	[BLK_IDX_SCHEDULE] = { 0 },
	[BLK_IDX_SCHEDULE_ENTRY_POINTS] = { 0 },
	[BLK_IDX_VL_LOOKUP] = {
		.entry_packing = sja1105_vl_lookup_entry_packing,
		.cmd_packing = sja1105_vl_lookup_cmd_packing,
		.access = (OP_READ | OP_WRITE),
		.max_entry_count = MAX_VL_LOOKUP_COUNT,
		.packed_size = SIZE_VL_LOOKUP_DYN_CMD_PQRS,
		.addr = 0x47,
	},
	[BLK_IDX_VL_POLICING] = { 0 },
	[BLK_IDX_VL_FORWARDING] = { 0 },
	[BLK_IDX_L2_LOOKUP] = {
		.entry_packing = sja1105pqrs_l2_lookup_entry_packing,
		.cmd_packing = sja1105pqrs_l2_lookup_cmd_packing,
		.access = (OP_READ | OP_WRITE | OP_DEL),
		.max_entry_count = MAX_L2_LOOKUP_COUNT,
		.packed_size = SIZE_L2_LOOKUP_DYN_CMD_ET,
		.addr = 0x24,
	},
	[BLK_IDX_L2_POLICING] = { 0 },
	[BLK_IDX_VLAN_LOOKUP] = {
		.entry_packing = sja1105_vlan_lookup_entry_packing,
		.cmd_packing = sja1105_vlan_lookup_cmd_packing,
		.access = (OP_READ | OP_WRITE | OP_DEL),
		.max_entry_count = MAX_VLAN_LOOKUP_COUNT,
		.packed_size = SIZE_VLAN_LOOKUP_DYN_CMD,
		.addr = 0x2D,
	},
	[BLK_IDX_L2_FORWARDING] = {
		.entry_packing = sja1105_l2_forwarding_entry_packing,
		.cmd_packing = sja1105_l2_forwarding_cmd_packing,
		.max_entry_count = MAX_L2_FORWARDING_COUNT,
		.access = OP_WRITE,
		.packed_size = SIZE_L2_FORWARDING_DYN_CMD,
		.addr = 0x2A,
	},
	[BLK_IDX_MAC_CONFIG] = {
		.entry_packing = sja1105pqrs_mac_config_entry_packing,
		.cmd_packing = sja1105pqrs_mac_config_cmd_packing,
		.max_entry_count = MAX_MAC_CONFIG_COUNT,
		.access = (OP_READ | OP_WRITE),
		.packed_size = SIZE_MAC_CONFIG_DYN_CMD_PQRS,
		.addr = 0x4B,
	},
	[BLK_IDX_SCHEDULE_PARAMS] = { 0 },
	[BLK_IDX_SCHEDULE_ENTRY_POINTS_PARAMS] = { 0 },
	[BLK_IDX_VL_FORWARDING_PARAMS] = { 0 },
	[BLK_IDX_L2_LOOKUP_PARAMS] = {
		.entry_packing = sja1105et_l2_lookup_params_entry_packing,
		.cmd_packing = sja1105et_l2_lookup_params_cmd_packing,
		.max_entry_count = MAX_L2_LOOKUP_PARAMS_COUNT,
		.access = (OP_READ | OP_WRITE),
		.packed_size = SIZE_L2_LOOKUP_PARAMS_DYN_CMD_ET,
		.addr = 0x38,
	},
	[BLK_IDX_L2_FORWARDING_PARAMS] = { 0 },
	[BLK_IDX_CLK_SYNC_PARAMS] = { 0 },
	[BLK_IDX_AVB_PARAMS] = { 0 },
	[BLK_IDX_GENERAL_PARAMS] = {
		.entry_packing = sja1105et_general_params_entry_packing,
		.cmd_packing = sja1105et_general_params_cmd_packing,
		.max_entry_count = MAX_GENERAL_PARAMS_COUNT,
		.access = OP_WRITE,
		.packed_size = SIZE_GENERAL_PARAMS_DYN_CMD_ET,
		.addr = 0x34,
	},
	[BLK_IDX_RETAGGING] = {
		.entry_packing = sja1105_retagging_entry_packing,
		.cmd_packing = sja1105_retagging_cmd_packing,
		.max_entry_count = MAX_RETAGGING_COUNT,
		.access = (OP_WRITE | OP_DEL),
		.packed_size = SIZE_RETAGGING_DYN_CMD_ET,
		.addr = 0x31,
	},
	[BLK_IDX_XMII_PARAMS] = { 0 },
	[BLK_IDX_SGMII] = { 0 },
};

int sja1105_dynamic_config_read(struct sja1105_private *priv,
				enum sja1105_blk_idx blk_idx,
				int index, void *entry)
{
	const struct sja1105_dynamic_table_ops *ops;
	struct sja1105_dyn_cmd cmd = { 0 };
	/* SPI payload buffer */
	u8 packed_buf[MAX_DYN_CMD_SIZE];
	int retries = 3;
	int rc;

	if (blk_idx >= BLK_IDX_MAX)
		return -ERANGE;

	ops = &priv->dyn_ops[blk_idx];

	if (index >= ops->max_entry_count)
		return -ERANGE;
	if (!(ops->access & OP_READ))
		return -EOPNOTSUPP;
	if (ops->packed_size > MAX_DYN_CMD_SIZE)
		return -ERANGE;
	if (!ops->cmd_packing)
		return -EOPNOTSUPP;
	if (!ops->entry_packing)
		return -EOPNOTSUPP;

	memset(packed_buf, 0, ops->packed_size);

	cmd.valid = true; /* Trigger action on table entry */
	cmd.rdwrset = SPI_READ; /* Action is read */
	cmd.index = index;
	ops->cmd_packing(packed_buf, &cmd, PACK);

	/* Send SPI write operation: read config table entry */
	rc = sja1105_spi_send_packed_buf(priv, SPI_WRITE, ops->addr,
					 packed_buf, ops->packed_size);
	if (rc < 0)
		return rc;

	/* Loop until we have confirmation that hardware has finished
	 * processing the command and has cleared the VALID field
	 */
	do {
		memset(packed_buf, 0, ops->packed_size);

		/* Retrieve the read operation's result */
		rc = sja1105_spi_send_packed_buf(priv, SPI_READ, ops->addr,
						 packed_buf, ops->packed_size);
		if (rc < 0)
			return rc;

		memset(&cmd, 0, sizeof(cmd));
		ops->cmd_packing(packed_buf, &cmd, UNPACK);
		if (!cmd.valident)
			return -EINVAL;
		cpu_relax();
	} while (cmd.valid && --retries);

	if (cmd.valid)
		return -ETIMEDOUT;

	/* Don't dereference possibly NULL pointer - maybe caller
	 * only wanted to see whether the entry existed or not.
	 */
	if (entry)
		ops->entry_packing(packed_buf, entry, UNPACK);
	return 0;
}

int sja1105_dynamic_config_write(struct sja1105_private *priv,
				 enum sja1105_blk_idx blk_idx,
				 int index, void *entry, bool keep)
{
	const struct sja1105_dynamic_table_ops *ops;
	struct sja1105_dyn_cmd cmd = { 0 };
	/* SPI payload buffer */
	u8 packed_buf[MAX_DYN_CMD_SIZE];
	int rc;

	if (blk_idx >= BLK_IDX_MAX)
		return -ERANGE;

	ops = &priv->dyn_ops[blk_idx];

	if (index >= ops->max_entry_count)
		return -ERANGE;
	if (!(ops->access & OP_WRITE))
		return -EOPNOTSUPP;
	if (!keep && !(ops->access & OP_DEL))
		return -EOPNOTSUPP;
	if (ops->packed_size > MAX_DYN_CMD_SIZE)
		return -ERANGE;

	memset(packed_buf, 0, ops->packed_size);

	cmd.valident = keep; /* If false, deletes entry */
	cmd.valid = true; /* Trigger action on table entry */
	cmd.rdwrset = SPI_WRITE; /* Action is write */
	cmd.index = index;

	if (!ops->cmd_packing)
		return -EOPNOTSUPP;
	ops->cmd_packing(packed_buf, &cmd, PACK);

	if (!ops->entry_packing)
		return -EOPNOTSUPP;
	/* Don't dereference potentially NULL pointer if just
	 * deleting a table entry is what was requested. For cases
	 * where 'index' field is physically part of entry structure,
	 * and needed here, we deal with that in the cmd_packing callback.
	 */
	if (keep)
		ops->entry_packing(packed_buf, entry, PACK);

	/* Send SPI write operation: read config table entry */
	rc = sja1105_spi_send_packed_buf(priv, SPI_WRITE, ops->addr,
					 packed_buf, ops->packed_size);
	if (rc < 0)
		return rc;

	memset(&cmd, 0, sizeof(cmd));
	ops->cmd_packing(packed_buf, &cmd, UNPACK);
	if (cmd.errors)
		return -EINVAL;

	return 0;
}

int sja1105_dynamic_config_init(struct sja1105_private *priv)
{
	const struct sja1105_dynamic_table_ops *ops;

	if (IS_ET(priv->device_id))
		ops = sja1105et_table_ops;
	else if (IS_PQRS(priv->device_id))
		ops = sja1105pqrs_table_ops;
	else
		return -EINVAL;

	priv->dyn_ops = ops;
	return 0;
}

static u8 crc8_add(u8 crc, u8 byte, u8 poly)
{
	int i;

	for (i = 0; i < 8; i++) {
		if ((crc ^ byte) & (1 << 7)) {
			crc <<= 1;
			crc ^= poly;
		} else {
			crc <<= 1;
		}
		byte <<= 1;
	}
	return crc;
}

/* CRC8 algorithm with non-reversed input, non-reversed output,
 * no input xor and no output xor. Code customized for receiving
 * the SJA1105 E/T FDB keys (vlanid, macaddr) as input. CRC polynomial
 * is also received as argument in the Koopman notation that the switch
 * hardware stores it in.
 */
u8 sja1105_fdb_hash(struct sja1105_private *priv, const u8 *addr, u16 vid)
{
	struct sja1105_l2_lookup_params_entry *l2_lookup_params =
		priv->static_config.tables[BLK_IDX_L2_LOOKUP_PARAMS].entries;
	u64 poly_koopman = l2_lookup_params->poly;
	/* Convert polynomial from Koopman to 'normal' notation */
	u8 poly = (u8) (1 + (poly_koopman << 1));
	u64 vlanid = l2_lookup_params->shared_learn ? 0 : vid;
	u64 input = (vlanid << 48) | ether_addr_to_u64(addr);
	u8 crc = 0; /* seed */
	int i;

	/* Mask the eight bytes starting from MSB one at a time */
	for (i = 56; i >= 0; i -= 8)
		crc = crc8_add(crc, (input & (0xffull << i)) >> i, poly);
	return crc;
}

