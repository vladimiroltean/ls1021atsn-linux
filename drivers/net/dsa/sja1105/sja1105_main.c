// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2018, Sensor-Technik Wiedemann GmbH
 * Copyright (c) 2018-2019, Vladimir Oltean <olteanv@gmail.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/spi/spi.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/of_mdio.h>
#include <linux/of_device.h>
#include <linux/netdev_features.h>
#include <linux/netdevice.h>
#include <linux/if_bridge.h>
#include <linux/if_ether.h>
#include "sja1105.h"

static void sja1105_hw_reset(struct gpio_desc *gpio, unsigned int pulse_len,
			     unsigned int startup_delay)
{
	gpiod_set_value_cansleep(gpio, 1);
	/* Wait for minimum reset pulse length */
	msleep(pulse_len);
	gpiod_set_value_cansleep(gpio, 0);
	/* Wait until chip is ready after reset */
	msleep(startup_delay);
}

static void
sja1105_port_allow_traffic(struct sja1105_l2_forwarding_entry *l2_fwd,
			   int from, int to, bool allow)
{
	if (allow) {
		l2_fwd[from].bc_domain  |= BIT(to);
		l2_fwd[from].reach_port |= BIT(to);
		l2_fwd[from].fl_domain  |= BIT(to);
	} else {
		l2_fwd[from].bc_domain  &= ~BIT(to);
		l2_fwd[from].reach_port &= ~BIT(to);
		l2_fwd[from].fl_domain  &= ~BIT(to);
	}
}

/* Structure used to temporarily transport device tree
 * settings into sja1105_setup
 */
struct sja1105_dt_port {
	phy_interface_t phy_mode;
	sja1105_mii_role_t role;
};

static int sja1105_init_mac_settings(struct sja1105_private *priv)
{
	struct sja1105_mac_config_entry default_mac = {
		/* Enable all 8 priority queues on egress.
		 * Every queue i holds top[i] - base[i] frames.
		 * Sum of top[i] - base[i] is 511 (max hardware limit).
		 */
		.top  = {0x3F, 0x7F, 0xBF, 0xFF, 0x13F, 0x17F, 0x1BF, 0x1FF},
		.base = {0x0, 0x40, 0x80, 0xC0, 0x100, 0x140, 0x180, 0x1C0},
		.enabled = {true, true, true, true, true, true, true, true},
		/* Keep standard IFG of 12 bytes on egress. */
		.ifg = 0,
		/* Always put the MAC speed in automatic mode, where it can be
		 * retrieved from the PHY object through phylib and
		 * sja1105_adjust_port_config.
		 */
		.speed = SJA1105_SPEED_AUTO,
		/* No static correction for 1-step 1588 events */
		.tp_delin = 0,
		.tp_delout = 0,
		/* Disable aging for critical TTEthernet traffic */
		.maxage = 0xFF,
		/* Internal VLAN (pvid) to apply to untagged ingress */
		.vlanprio = 0,
		.vlanid = 0,
		.ing_mirr = false,
		.egr_mirr = false,
		/* Don't drop traffic with other EtherType than ETH_P_IP */
		.drpnona664 = false,
		/* Don't drop double-tagged traffic */
		.drpdtag = false,
		/* Don't drop VLAN with single outer tag - P/Q/R/S only */
		.drpsotag = false,
		/* Don't drop VLAN with single inner tag - P/Q/R/S only */
		.drpsitag = false,
		/* Don't drop untagged traffic */
		.drpuntag = false,
		/* Don't retag 802.1p (VID 0) traffic with the pvid */
		.retag = false,
		/* Enable learning and I/O on user ports by default. */
		.dyn_learn = true,
		.egress = false,
		.ingress = false,
		.mirrcie = 0,
		.mirrcetag = 0,
		.ingmirrvid = 0,
		.ingmirrpcp = 0,
		.ingmirrdei = 0,
	};
	struct sja1105_mac_config_entry *mac;
	struct sja1105_table *table;
	int i;

	table = &priv->static_config.tables[BLK_IDX_MAC_CONFIG];

	/* Discard previous MAC Configuration Table */
	if (table->entry_count) {
		kfree(table->entries);
		table->entry_count = 0;
	}

	table->entries = kcalloc(SJA1105_NUM_PORTS,
				 table->ops->unpacked_entry_size, GFP_KERNEL);
	if (!table->entries)
		return -ENOMEM;

	/* Override table based on phylib DT bindings */
	table->entry_count = SJA1105_NUM_PORTS;

	mac = table->entries;

	for (i = 0; i < SJA1105_NUM_PORTS; i++)
		mac[i] = default_mac;

	return 0;
}

static int sja1105_init_mii_settings(struct sja1105_private *priv,
				     struct sja1105_dt_port *ports)
{
	struct device *dev = &priv->spidev->dev;
	struct sja1105_xmii_params_entry *mii;
	struct sja1105_table *table;
	int i;

	table = &priv->static_config.tables[BLK_IDX_XMII_PARAMS];

	/* Discard previous xMII Mode Parameters Table */
	if (table->entry_count) {
		kfree(table->entries);
		table->entry_count = 0;
	}

	table->entries = kcalloc(MAX_XMII_PARAMS_COUNT,
				 table->ops->unpacked_entry_size, GFP_KERNEL);
	if (!table->entries)
		return -ENOMEM;

	/* Override table based on phylib DT bindings */
	table->entry_count = MAX_XMII_PARAMS_COUNT;

	mii = table->entries;

	for (i = 0; i < SJA1105_NUM_PORTS; i++) {
		switch (ports[i].phy_mode) {
		case PHY_INTERFACE_MODE_MII:
			mii->xmii_mode[i] = XMII_MODE_MII;
			break;
		case PHY_INTERFACE_MODE_RMII:
			mii->xmii_mode[i] = XMII_MODE_RMII;
			break;
		case PHY_INTERFACE_MODE_RGMII:
		case PHY_INTERFACE_MODE_RGMII_ID:
		case PHY_INTERFACE_MODE_RGMII_RXID:
		case PHY_INTERFACE_MODE_RGMII_TXID:
			mii->xmii_mode[i] = XMII_MODE_RGMII;
			break;
		default:
			dev_err(dev, "Unsupported PHY mode %s!\n",
				phy_modes(ports[i].phy_mode));
		}

		mii->phy_mac[i] = ports[i].role;
	}
	return 0;
}

static int sja1105_init_static_fdb(struct sja1105_private *priv)
{
	struct sja1105_table *table;

	table = &priv->static_config.tables[BLK_IDX_L2_LOOKUP];

	/* We only populate the FDB table through dynamic
	 * L2 Address Lookup entries
	 */
	if (table->entry_count) {
		kfree(table->entries);
		table->entry_count = 0;
	}
	return 0;
}

static int sja1105_init_l2_lookup_params(struct sja1105_private *priv)
{
	struct sja1105_table *table;
	struct sja1105_l2_lookup_params_entry default_l2_lookup_params = {
		/* TODO Learned FDB entries are never forgotten */
		.maxage = 0,
		/* All entries within a FDB bin are available for learning */
		.dyn_tbsz = SJA1105ET_FDB_BIN_SIZE,
		/* 2^8 + 2^5 + 2^3 + 2^2 + 2^1 + 1 in Koopman notation */
		.poly = 0x97,
		/* This selects between Independent VLAN Learning (IVL) and
		 * Shared VLAN Learning (SVL)
		 */
		.shared_learn = false,
		/* Don't discard management traffic based on ENFPORT -
		 * we don't perform SMAC port enforcement anyway, so
		 * what we are setting here doesn't matter.
		 */
		.no_enf_hostprt = false,
		/* Don't learn SMAC for mac_fltres1 and mac_fltres0.
		 * TODO Maybe correlate with no_linklocal_learn from bridge
		 * driver?
		 */
		.no_mgmt_learn = true,
	};

	table = &priv->static_config.tables[BLK_IDX_L2_LOOKUP_PARAMS];

	if (table->entry_count) {
		kfree(table->entries);
		table->entry_count = 0;
	}

	table->entries = kcalloc(MAX_L2_LOOKUP_PARAMS_COUNT,
				 table->ops->unpacked_entry_size, GFP_KERNEL);
	if (!table->entries)
		return -ENOMEM;

	table->entry_count = MAX_L2_LOOKUP_PARAMS_COUNT;

	/* This table only has a single entry */
	((struct sja1105_l2_lookup_params_entry *)table->entries)[0] =
				default_l2_lookup_params;

	return 0;
}

static int sja1105_init_static_vlan(struct sja1105_private *priv)
{
	struct sja1105_table *table;
	struct sja1105_vlan_lookup_entry pvid = {
		.ving_mirr = 0,
		.vegr_mirr = 0,
		.vmemb_port = 0,
		.vlan_bc = 0,
		.tag_port = 0,
		.vlanid = 0,
	};
	int i;

	table = &priv->static_config.tables[BLK_IDX_VLAN_LOOKUP];

	/* The static VLAN table will only contain the initial pvid of 0.
	 */
	if (table->entry_count) {
		kfree(table->entries);
		table->entry_count = 0;
	}

	table->entries = kcalloc(1, table->ops->unpacked_entry_size,
				 GFP_KERNEL);
	if (!table->entries)
		return -ENOMEM;

	table->entry_count = 1;

	/* VLAN ID 0: all DT-defined ports are members; no restrictions on
	 * forwarding; always transmit priority-tagged frames as untagged.
	 */
	for (i = 0; i < SJA1105_NUM_PORTS; i++) {
		pvid.vmemb_port |= BIT(i);
		pvid.vlan_bc |= BIT(i);
		pvid.tag_port &= ~BIT(i);
	}

	((struct sja1105_vlan_lookup_entry *)table->entries)[0] = pvid;
	return 0;
}

static int sja1105_init_l2_forwarding(struct sja1105_private *priv)
{
	struct sja1105_l2_forwarding_entry *l2fwd;
	struct sja1105_table *table;
	int i, j;

	table = &priv->static_config.tables[BLK_IDX_L2_FORWARDING];

	if (table->entry_count) {
		kfree(table->entries);
		table->entry_count = 0;
	}

	table->entries = kcalloc(MAX_L2_FORWARDING_COUNT,
				 table->ops->unpacked_entry_size, GFP_KERNEL);
	if (!table->entries)
		return -ENOMEM;

	table->entry_count = MAX_L2_FORWARDING_COUNT;

	l2fwd = table->entries;

	/* First 5 entries define the forwarding rules */
	for (i = 0; i < SJA1105_NUM_PORTS; i++) {
		unsigned int upstream = dsa_upstream_port(priv->ds, i);

		for (j = 0; j < SJA1105_NUM_TC; j++)
			l2fwd[i].vlan_pmap[j] = j;

		if (i == upstream)
			continue;

		sja1105_port_allow_traffic(l2fwd, i, upstream, true);
		sja1105_port_allow_traffic(l2fwd, upstream, i, true);
	}
	/* Next 8 entries define VLAN PCP mapping from ingress to egress.
	 * Create a one-to-one mapping.
	 */
	for (i = 0; i < SJA1105_NUM_TC; i++)
		for (j = 0; j < SJA1105_NUM_PORTS; j++)
			l2fwd[SJA1105_NUM_PORTS + i].vlan_pmap[j] = i;

	return 0;
}

static int sja1105_init_l2_forwarding_params(struct sja1105_private *priv)
{
	struct sja1105_l2_forwarding_params_entry default_l2fwd_params = {
		/* Disallow dynamic reconfiguration of vlan_pmap */
		.max_dynp = 0,
		/* Use a single memory partition for all ingress queues */
		.part_spc = { MAX_FRAME_MEMORY, 0, 0, 0, 0, 0, 0, 0 },
	};
	struct sja1105_table *table;

	table = &priv->static_config.tables[BLK_IDX_L2_FORWARDING_PARAMS];

	if (table->entry_count) {
		kfree(table->entries);
		table->entry_count = 0;
	}

	table->entries = kcalloc(MAX_L2_FORWARDING_PARAMS_COUNT,
				 table->ops->unpacked_entry_size, GFP_KERNEL);
	if (!table->entries)
		return -ENOMEM;

	table->entry_count = MAX_L2_FORWARDING_PARAMS_COUNT;

	/* This table only has a single entry */
	((struct sja1105_l2_forwarding_params_entry *)table->entries)[0] =
				default_l2fwd_params;

	return 0;
}

static int sja1105_init_general_params(struct sja1105_private *priv)
{
	struct sja1105_general_params_entry default_general_params = {
		/* Disallow dynamic changing of the mirror port */
		.mirr_ptacu = 0,
		.switchid = priv->ds->index,
		/* Priority queue for link-local frames trapped to CPU */
		.hostprio = 0,
		.mac_fltres1 = SJA1105_LINKLOCAL_FILTER_A,
		.mac_flt1    = SJA1105_LINKLOCAL_FILTER_A_MASK,
		.incl_srcpt1 = true,
		.send_meta1  = false,
		.mac_fltres0 = SJA1105_LINKLOCAL_FILTER_B,
		.mac_flt0    = SJA1105_LINKLOCAL_FILTER_B_MASK,
		.incl_srcpt0 = true,
		.send_meta0  = false,
		/* The destination for traffic matching mac_fltres1 and
		 * mac_fltres0 on all ports except host_port. Such traffic
		 * receieved on host_port itself would be dropped, except
		 * by installing a temporary 'management route'
		 */
		.host_port = dsa_upstream_port(priv->ds, 0),
		/* Same as host port */
		.mirr_port = dsa_upstream_port(priv->ds, 0),
		/* Link-local traffic received on casc_port will be forwarded
		 * to host_port without embedding the source port and device ID
		 * info in the destination MAC address (presumably because it
		 * is a cascaded port and a downstream SJA switch already did
		 * that). Default to an invalid port (to disable the feature)
		 * and overwrite this if we find any DSA (cascaded) ports.
		 */
		.casc_port = SJA1105_NUM_PORTS,
		/* No TTEthernet */
		.vllupformat = 0,
		.vlmarker = 0,
		.vlmask = 0,
		/* Only update correctionField for 1-step PTP (L2 transport) */
		.ignore2stf = 0,
		.tpid = ETH_P_8021Q,
		.tpid2 = ETH_P_8021Q,
		/* P/Q/R/S only */
		.queue_ts = 0,
		.egrmirrvid = 0,
		.egrmirrpcp = 0,
		.egrmirrdei = 0,
		.replay_port = 0,
	};
	struct sja1105_table *table;
	int i;

	for (i = 0; i < SJA1105_NUM_PORTS; i++)
		if (dsa_is_dsa_port(priv->ds, i))
			default_general_params.casc_port = i;

	table = &priv->static_config.tables[BLK_IDX_GENERAL_PARAMS];

	if (table->entry_count) {
		kfree(table->entries);
		table->entry_count = 0;
	}

	table->entries = kcalloc(MAX_GENERAL_PARAMS_COUNT,
				 table->ops->unpacked_entry_size, GFP_KERNEL);
	if (!table->entries)
		return -ENOMEM;

	table->entry_count = MAX_GENERAL_PARAMS_COUNT;

	/* This table only has a single entry */
	((struct sja1105_general_params_entry *)table->entries)[0] =
				default_general_params;

	return 0;
}

static inline void
sja1105_setup_policer(struct sja1105_l2_policing_entry *policing,
		      int index)
{
#define RATE_MBPS(speed) (((speed) * 64000) / 1000)
	policing[index].sharindx = index;
	policing[index].smax = 65535; /* Burst size in bytes */
	policing[index].rate = RATE_MBPS(1000);
	policing[index].maxlen = ETH_FRAME_LEN + VLAN_HLEN + ETH_FCS_LEN;
	policing[index].partition = 0;
#undef RATE_MBPS
}

static int sja1105_init_l2_policing(struct sja1105_private *priv)
{
	struct sja1105_l2_policing_entry *policing;
	struct sja1105_table *table;
	int i, j, k;

	table = &priv->static_config.tables[BLK_IDX_L2_POLICING];

	/* Discard previous L2 Policing Table */
	if (table->entry_count) {
		kfree(table->entries);
		table->entry_count = 0;
	}

	table->entries = kcalloc(MAX_L2_POLICING_COUNT,
				 table->ops->unpacked_entry_size, GFP_KERNEL);
	if (!table->entries)
		return -ENOMEM;

	table->entry_count = MAX_L2_POLICING_COUNT;

	policing = table->entries;

	/* k sweeps through all unicast policers (0-39).
	 * bcast sweeps through policers 40-44.
	 */
	for (i = 0, k = 0; i < SJA1105_NUM_PORTS; i++) {
		int bcast = (SJA1105_NUM_PORTS * SJA1105_NUM_TC) + i;

		for (j = 0; j < SJA1105_NUM_TC; j++, k++)
			sja1105_setup_policer(policing, k);

		/* Set up this port's policer for broadcast traffic */
		sja1105_setup_policer(policing, bcast);
	}
	return 0;
}

static int sja1105_static_config_load(struct sja1105_private *priv,
				      struct sja1105_dt_port *ports)
{
	int rc;

	sja1105_static_config_free(&priv->static_config);
	rc = sja1105_static_config_init(&priv->static_config,
					priv->info->static_ops,
					priv->info->device_id);
	if (rc)
		return rc;

	/* Build static configuration */
	rc = sja1105_init_mac_settings(priv);
	if (rc < 0)
		return rc;
	rc = sja1105_init_mii_settings(priv, ports);
	if (rc < 0)
		return rc;
	rc = sja1105_init_static_fdb(priv);
	if (rc < 0)
		return rc;
	rc = sja1105_init_static_vlan(priv);
	if (rc < 0)
		return rc;
	rc = sja1105_init_l2_lookup_params(priv);
	if (rc < 0)
		return rc;
	rc = sja1105_init_l2_forwarding(priv);
	if (rc < 0)
		return rc;
	rc = sja1105_init_l2_forwarding_params(priv);
	if (rc < 0)
		return rc;
	rc = sja1105_init_l2_policing(priv);
	if (rc < 0)
		return rc;
	rc = sja1105_init_general_params(priv);
	if (rc < 0)
		return rc;

	/* Send initial configuration to hardware via SPI */
	return sja1105_static_config_upload(priv);
}

static int sja1105_parse_ports_node(struct sja1105_private *priv,
				    struct sja1105_dt_port *ports,
				    struct device_node *ports_node)
{
	struct device *dev = &priv->spidev->dev;
	struct device_node *child;

	for_each_child_of_node(ports_node, child) {
		struct device_node *phy_node;
		int phy_mode;
		u32 index;

		/* Get switch port number from DT */
		if (of_property_read_u32(child, "reg", &index) < 0) {
			dev_err(dev, "Port number not defined in device tree "
				"(property \"reg\")\n");
			return -ENODEV;
		}

		/* Get PHY mode from DT */
		phy_mode = of_get_phy_mode(child);
		if (phy_mode < 0) {
			dev_err(dev, "Failed to read phy-mode or "
				"phy-interface-type property for port %d\n",
				index);
			return -ENODEV;
		}
		ports[index].phy_mode = phy_mode;

		phy_node = of_parse_phandle(child, "phy-handle", 0);
		if (!phy_node) {
			if (!of_phy_is_fixed_link(child)) {
				dev_err(dev, "phy-handle or fixed-link "
					"properties missing!\n");
				return -ENODEV;
			}
			/* phy-handle is missing, but fixed-link isn't.
			 * So it's a fixed link. Default to PHY role.
			 */
			ports[index].role = XMII_PHY;
		} else {
			/* phy-handle present => put port in MAC role */
			ports[index].role = XMII_MAC;
			of_node_put(phy_node);
		}

		/* The MAC/PHY role can be overridden with explicit bindings */
		if (of_property_read_bool(child, "sja1105,role-mac"))
			ports[index].role = XMII_MAC;
		else if (of_property_read_bool(child, "sja1105,role-phy"))
			ports[index].role = XMII_PHY;
	}

	return 0;
}

static int sja1105_parse_dt(struct sja1105_private *priv,
			    struct sja1105_dt_port *ports)
{
	struct device *dev = &priv->spidev->dev;
	struct device_node *switch_node = dev->of_node;
	struct device_node *ports_node;
	int rc;

	ports_node = of_get_child_by_name(switch_node, "ports");
	if (!ports_node) {
		dev_err(dev, "Incorrect bindings: absent \"ports\" node\n");
		return -ENODEV;
	}

	rc = sja1105_parse_ports_node(priv, ports, ports_node);
	of_node_put(ports_node);

	return rc;
}

/* Convert back and forth MAC speed from Mbps to SJA1105 encoding */
static int sja1105_speed[] = {
	[SJA1105_SPEED_AUTO]     = 0,
	[SJA1105_SPEED_10MBPS]   = 10,
	[SJA1105_SPEED_100MBPS]  = 100,
	[SJA1105_SPEED_1000MBPS] = 1000,
};

static sja1105_speed_t sja1105_get_speed_cfg(unsigned int speed_mbps)
{
	int i;

	for (i = SJA1105_SPEED_AUTO; i <= SJA1105_SPEED_1000MBPS; i++)
		if (sja1105_speed[i] == speed_mbps)
			return i;
	return -EINVAL;
}

/* Set link speed and enable/disable traffic I/O in the MAC configuration
 * for a specific port.
 *
 * @speed_mbps: If 0, leave the speed unchanged, else adapt MAC to PHY speed.
 * @enabled: Manage Rx and Tx settings for this port. Overrides the static
 *	     configuration settings.
 */
static int sja1105_adjust_port_config(struct sja1105_private *priv, int port,
				      int speed_mbps, bool enabled)
{
	struct sja1105_xmii_params_entry *mii;
	struct sja1105_mac_config_entry *mac;
	struct device *dev = priv->ds->dev;
	sja1105_phy_interface_t phy_mode;
	sja1105_speed_t speed;
	int rc;

	mii = priv->static_config.tables[BLK_IDX_XMII_PARAMS].entries;
	mac = priv->static_config.tables[BLK_IDX_MAC_CONFIG].entries;

	speed = sja1105_get_speed_cfg(speed_mbps);
	if (speed_mbps && speed < 0) {
		dev_err(dev, "Invalid speed %iMbps\n", speed_mbps);
		return -EINVAL;
	}

	/* If requested, overwrite SJA1105_SPEED_AUTO from the static MAC
	 * configuration table, since this will be used for the clocking setup,
	 * and we no longer need to store it in the static config (already told
	 * hardware we want auto during upload phase).
	 */
	if (speed_mbps)
		mac[port].speed = speed;
	else
		mac[port].speed = SJA1105_SPEED_AUTO;

	/* On P/Q/R/S, one can read from the device via the MAC reconfiguration
	 * tables. On E/T, MAC reconfig tables are not readable, only writable.
	 * We have to *know* what the MAC looks like.  For the sake of keeping
	 * the code common, we'll use the static configuration tables as a
	 * reasonable approximation for both E/T and P/Q/R/S.
	 */
	mac[port].ingress = enabled;
	mac[port].egress  = enabled;

	/* Write to the dynamic reconfiguration tables */
	rc = sja1105_dynamic_config_write(priv, BLK_IDX_MAC_CONFIG,
					  port, &mac[port], true);
	if (rc < 0) {
		dev_err(dev, "Failed to write MAC config: %d\n", rc);
		return rc;
	}

	/* Reconfigure the PLLs for the RGMII interfaces (required 125 MHz at
	 * gigabit, 25 MHz at 100 Mbps and 2.5 MHz at 10 Mbps). For MII and
	 * RMII no change of the clock setup is required. Actually, changing
	 * the clock setup does interrupt the clock signal for a certain time
	 * which causes trouble for all PHYs relying on this signal.
	 */
	if (!enabled)
		return 0;

	phy_mode = mii->xmii_mode[port];
	if (phy_mode != XMII_MODE_RGMII)
		return 0;

	return sja1105_clocking_setup_port(priv, port);
}

static void sja1105_adjust_link(struct dsa_switch *ds, int port,
				struct phy_device *phydev)
{
	struct sja1105_private *priv = ds->priv;

	if (!phydev->link)
		sja1105_adjust_port_config(priv, port, 0, false);
	else
		sja1105_adjust_port_config(priv, port, phydev->speed, true);
}

#define fdb(bin, index) \
	((bin) * SJA1105ET_FDB_BIN_SIZE + (index))
#define is_bin_index_valid(i) \
	((i) >= 0 && (i) < SJA1105ET_FDB_BIN_SIZE)

static int
sja1105_is_fdb_entry_in_bin(struct sja1105_private *priv, int bin,
			    const u8 *addr, u16 vid,
			    struct sja1105_l2_lookup_entry *fdb_match,
			    int *last_unused)
{
	int index_in_bin;

	for (index_in_bin = 0; index_in_bin < SJA1105ET_FDB_BIN_SIZE;
	     index_in_bin++) {
		struct sja1105_l2_lookup_entry l2_lookup = { 0 };

		/* Skip unused entries, optionally marking them
		 * into the return value
		 */
		if (sja1105_dynamic_config_read(priv, BLK_IDX_L2_LOOKUP,
						fdb(bin, index_in_bin),
						&l2_lookup)) {
			if (last_unused)
				*last_unused = index_in_bin;
			continue;
		}

		if (l2_lookup.macaddr == ether_addr_to_u64(addr) &&
		    l2_lookup.vlanid == vid) {
			if (fdb_match)
				*fdb_match = l2_lookup;
			return index_in_bin;
		}
	}
	/* Return an invalid entry index if not found */
	return SJA1105ET_FDB_BIN_SIZE;
}

static int sja1105_fdb_add(struct dsa_switch *ds, int port,
			   const unsigned char *addr, u16 vid)
{
	struct sja1105_l2_lookup_entry l2_lookup = { 0 };
	struct sja1105_private *priv = ds->priv;
	struct device *dev = ds->dev;
	int bin, index_in_bin;
	int last_unused;

	bin = sja1105_fdb_hash(priv, addr, vid);

	index_in_bin = sja1105_is_fdb_entry_in_bin(priv, bin, addr, vid,
						   &l2_lookup, &last_unused);
	if (is_bin_index_valid(index_in_bin)) {
		/* We have an FDB entry. Is our port in the destination
		 * mask? If yes, we need to do nothing. If not, we need
		 * to rewrite the entry by adding this port to it.
		 */
		if (l2_lookup.destports & BIT(port))
			return 0;
		l2_lookup.destports |= BIT(port);
	} else {
		/* We don't have an FDB entry. We construct a new one and
		 * try to find a place for it within the FDB table.
		 */
		l2_lookup.macaddr = ether_addr_to_u64(addr);
		l2_lookup.destports = BIT(port);
		l2_lookup.vlanid = vid;

		if (is_bin_index_valid(last_unused)) {
			index_in_bin = last_unused;
		} else {
			/* Bin is full, need to evict somebody.
			 * Choose victim at random. If you get these messages
			 * often, you may need to consider changing the
			 * distribution function:
			 * static_config[BLK_IDX_L2_LOOKUP_PARAMS].entries->poly
			 */
			get_random_bytes(&index_in_bin, sizeof(u8));
			index_in_bin %= SJA1105ET_FDB_BIN_SIZE;
			dev_warn(dev, "Warning, FDB bin %d full while adding entry for %pM. Evicting entry %u.\n",
				 bin, addr, index_in_bin);
			/* Evict entry */
			sja1105_dynamic_config_write(priv, BLK_IDX_L2_LOOKUP,
						     fdb(bin, index_in_bin),
						     NULL, false);
		}
	}
	l2_lookup.index = fdb(bin, index_in_bin);

	return sja1105_dynamic_config_write(priv, BLK_IDX_L2_LOOKUP,
				l2_lookup.index, &l2_lookup, true);
}

static int sja1105_fdb_del(struct dsa_switch *ds, int port,
			   const unsigned char *addr, u16 vid)
{
	struct sja1105_l2_lookup_entry l2_lookup = { 0 };
	struct sja1105_private *priv = ds->priv;
	u8 bin, index_in_bin;
	bool keep;

	bin = sja1105_fdb_hash(priv, addr, vid);

	index_in_bin = sja1105_is_fdb_entry_in_bin(priv, bin, addr, vid,
						   &l2_lookup, NULL);
	if (!is_bin_index_valid(index_in_bin))
		return 0;

	/* We have an FDB entry. Is our port in the destination mask? If yes,
	 * we need to remove it. If the resulting port mask becomes empty, we
	 * need to completely evict the FDB entry.
	 * Otherwise we just write it back.
	 */
	if (l2_lookup.destports & BIT(port))
		l2_lookup.destports &= ~BIT(port);
	if (l2_lookup.destports)
		keep = true;
	else
		keep = false;

	return sja1105_dynamic_config_write(priv, BLK_IDX_L2_LOOKUP,
					    fdb(bin, index_in_bin),
					    &l2_lookup, keep);
}

static int sja1105_fdb_dump(struct dsa_switch *ds, int port,
			    dsa_fdb_dump_cb_t *cb, void *data)
{
	struct sja1105_private *priv = ds->priv;
	struct device *dev = ds->dev;
	int i;

	for (i = 0; i < MAX_L2_LOOKUP_COUNT; i++) {
		struct sja1105_l2_lookup_entry l2_lookup;
		u8 macaddr[ETH_ALEN];
		int rc;

		memset(&l2_lookup, 0, sizeof(l2_lookup));
		rc = sja1105_dynamic_config_read(priv, BLK_IDX_L2_LOOKUP,
						 i, &l2_lookup);
		/* No fdb entry at i, not an issue */
		if (rc == -EINVAL)
			continue;
		if (rc) {
			dev_err(dev, "Failed to dump FDB: %d\n", rc);
			return rc;
		}

		/* FDB dump callback is per port. This means we have to
		 * disregard a valid entry if it's not for this port, even if
		 * only to revisit it later. This is inefficient because the
		 * 1024-sized FDB table needs to be traversed 4 times through
		 * SPI during a 'bridge fdb show' command.
		 */
		if (!(l2_lookup.destports & BIT(port)))
			continue;
		u64_to_ether_addr(l2_lookup.macaddr, macaddr);
		cb(macaddr, l2_lookup.vlanid, false, data);
	}
	return 0;
}

#undef fdb
#undef is_bin_index_valid

/* This callback needs to be present */
static int sja1105_mdb_prepare(struct dsa_switch *ds, int port,
			       const struct switchdev_obj_port_mdb *mdb)
{
	return 0;
}

static void sja1105_mdb_add(struct dsa_switch *ds, int port,
			    const struct switchdev_obj_port_mdb *mdb)
{
	sja1105_fdb_add(ds, port, mdb->addr, mdb->vid);
}

static int sja1105_mdb_del(struct dsa_switch *ds, int port,
			   const struct switchdev_obj_port_mdb *mdb)
{
	return sja1105_fdb_del(ds, port, mdb->addr, mdb->vid);
}

static int sja1105_bridge_member(struct dsa_switch *ds, int port,
				 struct net_device *br, bool member)
{
	struct sja1105_l2_forwarding_entry *l2_fwd;
	struct sja1105_private *priv = ds->priv;
	int i, rc;

	l2_fwd = priv->static_config.tables[BLK_IDX_L2_FORWARDING].entries;

	for (i = 0; i < SJA1105_NUM_PORTS; i++) {
		/* Add this port to the forwarding matrix of the
		 * other ports in the same bridge, and viceversa.
		 */
		if (!dsa_is_user_port(ds, i))
			continue;
		/* For the ports already under the bridge, only one thing needs
		 * to be done, and that is to add this port to their
		 * reachability domain. So we can perform the SPI write for
		 * them immediately. However, for this port itself (the one
		 * that is new to the bridge), we need to add all other ports
		 * to its reachability domain. So we do that incrementally in
		 * this loop, and perform the SPI write only at the end, once
		 * the domain contains all other bridge ports.
		 */
		if (i == port)
			continue;
		if (dsa_to_port(ds, i)->bridge_dev != br)
			continue;
		sja1105_port_allow_traffic(l2_fwd, i, port, member);
		sja1105_port_allow_traffic(l2_fwd, port, i, member);

		rc = sja1105_dynamic_config_write(priv, BLK_IDX_L2_FORWARDING,
						  i, &l2_fwd[i], true);
		if (rc < 0)
			return rc;
	}

	return sja1105_dynamic_config_write(priv, BLK_IDX_L2_FORWARDING,
					    port, &l2_fwd[port], true);
}

static int sja1105_bridge_join(struct dsa_switch *ds, int port,
			       struct net_device *br)
{
	return sja1105_bridge_member(ds, port, br, true);
}

static void sja1105_bridge_leave(struct dsa_switch *ds, int port,
				 struct net_device *br)
{
	sja1105_bridge_member(ds, port, br, false);
}

static enum dsa_tag_protocol
sja1105_get_tag_protocol(struct dsa_switch *ds, int port)
{
	return DSA_TAG_PROTO_NONE;
}

/* The programming model for the SJA1105 switch is "all-at-once" via static
 * configuration tables. Some of these can be dynamically modified at runtime,
 * but not the xMII mode parameters table.
 * Furthermode, some PHYs may not have crystals for generating their clocks
 * (e.g. RMII). Instead, their 50MHz clock is supplied via the SJA1105 port's
 * ref_clk pin. So port clocking needs to be initialized early, before
 * connecting to PHYs is attempted, otherwise they won't respond through MDIO.
 * Setting correct PHY link speed does not matter now.
 * But dsa_slave_phy_setup is called later than sja1105_setup, so the PHY
 * bindings are not yet parsed by DSA core. We need to parse early so that we
 * can populate the xMII mode parameters table.
 */
static int sja1105_setup(struct dsa_switch *ds)
{
	struct sja1105_dt_port ports[SJA1105_NUM_PORTS];
	struct sja1105_private *priv = ds->priv;
	int rc;

	rc = sja1105_parse_dt(priv, ports);
	if (rc < 0) {
		dev_err(ds->dev, "Failed to parse DT: %d\n", rc);
		return rc;
	}
	/* Create and send configuration down to device */
	rc = sja1105_static_config_load(priv, ports);
	if (rc < 0) {
		dev_err(ds->dev, "Failed to load static config: %d\n", rc);
		return rc;
	}
	/* Configure the CGU (PHY link modes and speeds) */
	rc = sja1105_clocking_setup(priv);
	if (rc < 0) {
		dev_err(ds->dev, "Failed to configure MII clocking: %d\n", rc);
		return rc;
	}

	return 0;
}

static const struct dsa_switch_ops sja1105_switch_ops = {
	.get_tag_protocol	= sja1105_get_tag_protocol,
	.setup			= sja1105_setup,
	.adjust_link		= sja1105_adjust_link,
	.port_fdb_dump		= sja1105_fdb_dump,
	.port_fdb_add		= sja1105_fdb_add,
	.port_fdb_del		= sja1105_fdb_del,
	.port_bridge_join	= sja1105_bridge_join,
	.port_bridge_leave	= sja1105_bridge_leave,
	.port_mdb_prepare	= sja1105_mdb_prepare,
	.port_mdb_add		= sja1105_mdb_add,
	.port_mdb_del		= sja1105_mdb_del,
};

static int sja1105_check_device_id(struct sja1105_private *priv)
{
	const struct sja1105_regs *regs = priv->info->regs;
	u8 prod_id[SIZE_SJA1105_DEVICE_ID] = {0};
	struct device *dev = &priv->spidev->dev;
	u64 device_id;
	u64 part_no;
	int rc;

	rc = sja1105_spi_send_int(priv, SPI_READ, regs->device_id,
				  &device_id, SIZE_SJA1105_DEVICE_ID);
	if (rc < 0)
		return rc;

	if (device_id != priv->info->device_id) {
		dev_err(dev, "Expected device ID 0x%llx but read 0x%llx\n",
			priv->info->device_id, device_id);
		return -ENODEV;
	}

	rc = sja1105_spi_send_packed_buf(priv, SPI_READ, regs->prod_id,
					 prod_id, SIZE_SJA1105_DEVICE_ID);
	if (rc < 0)
		return rc;

	sja1105_unpack(prod_id, &part_no, 19, 4, SIZE_SJA1105_DEVICE_ID);

	if (part_no != priv->info->part_no) {
		dev_err(dev, "Expected part number 0x%llx but read 0x%llx\n",
			priv->info->part_no, part_no);
		return -ENODEV;
	}

	return 0;
}

static int sja1105_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct sja1105_private *priv;
	struct dsa_switch *ds;
	int rc;

	if (!dev->of_node) {
		dev_err(dev, "No DTS bindings for SJA1105 driver\n");
		return -EINVAL;
	}

	priv = devm_kzalloc(dev, sizeof(struct sja1105_private), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* Configure the optional reset pin and bring up switch */
	priv->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset_gpio))
		dev_dbg(dev, "reset-gpios not defined, ignoring\n");
	else
		sja1105_hw_reset(priv->reset_gpio, 1, 1);

	/* Populate our driver private structure (priv) based on
	 * the device tree node that was probed (spi)
	 */
	priv->spidev = spi;
	spi_set_drvdata(spi, priv);

	/* Configure the SPI bus */
	spi->mode = SPI_CPHA;
	spi->bits_per_word = 8;
	rc = spi_setup(spi);
	if (rc < 0) {
		dev_err(dev, "Could not init SPI\n");
		return rc;
	}

	priv->info = of_device_get_match_data(dev);

	/* Detect hardware device */
	rc = sja1105_check_device_id(priv);
	if (rc < 0) {
		dev_err(dev, "Device ID check failed: %d\n", rc);
		return rc;
	}

	dev_info(dev, "Probed switch chip: %s\n", priv->info->name);

	ds = dsa_switch_alloc(dev, SJA1105_NUM_PORTS);
	if (!ds)
		return -ENOMEM;

	ds->ops = &sja1105_switch_ops;
	ds->priv = priv;
	priv->ds = ds;

	return dsa_register_switch(priv->ds);
}

static int sja1105_remove(struct spi_device *spi)
{
	struct sja1105_private *priv = spi_get_drvdata(spi);

	dsa_unregister_switch(priv->ds);
	sja1105_static_config_free(&priv->static_config);
	return 0;
}

static const struct of_device_id sja1105_dt_ids[] = {
	{ .compatible = "nxp,sja1105e", .data = &sja1105e_info },
	{ .compatible = "nxp,sja1105t", .data = &sja1105t_info },
	{ .compatible = "nxp,sja1105p", .data = &sja1105p_info },
	{ .compatible = "nxp,sja1105q", .data = &sja1105q_info },
	{ .compatible = "nxp,sja1105r", .data = &sja1105r_info },
	{ .compatible = "nxp,sja1105s", .data = &sja1105s_info },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, sja1105_dt_ids);

static struct spi_driver sja1105_driver = {
	.driver = {
		.name  = "sja1105",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(sja1105_dt_ids),
	},
	.probe  = sja1105_probe,
	.remove = sja1105_remove,
};

module_spi_driver(sja1105_driver);

MODULE_AUTHOR("Vladimir Oltean <olteanv@gmail.com>");
MODULE_AUTHOR("Georg Waibel <georg.waibel@sensor-technik.de>");
MODULE_DESCRIPTION("SJA1105 Driver");
MODULE_LICENSE("GPL v2");
