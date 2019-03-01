// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, Sensor-Technik Wiedemann GmbH
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
#include <linux/netdev_features.h>
#include <linux/netdevice.h>
#include <linux/if_bridge.h>
#include <linux/if_ether.h>
#include "sja1105.h"

static int sja1105_hw_reset(struct gpio_desc *gpio, unsigned int pulse_len,
			    unsigned int startup_delay)
{
	if (IS_ERR(gpio))
		return PTR_ERR(gpio);

	gpiod_set_value_cansleep(gpio, 1);
	/* Wait for minimum reset pulse length */
	msleep(pulse_len);
	gpiod_set_value_cansleep(gpio, 0);
	/* Wait until chip is ready after reset */
	msleep(startup_delay);

	return 0;
}

static void sja1105_port_allow_traffic(struct sja1105_l2_forwarding_entry *l2_fwd,
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
	int xmii_mode;
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
		/* Disable aging for critical frame traffic */
		.maxage = 0xFF,
		/* Internal VLAN (pvid) to apply to untagged ingress */
		.vlanprio = 0,
		.vlanid = 0,
		.ing_mirr = false,
		.egr_mirr = false,
		/* Don't drop traffic with other EtherType than 800h */
		.drpnona664 = false,
		.drpdtag = false,
		.drpsotag = false,
		.drpsitag = false,
		.drpuntag = false,
		/* Don't retag 802.1p (VID 0) traffic with the pvid */
		.retag = false,
		/* Disable learning and I/O on user ports by default -
		 * STP will enable it.
		 */
		.dyn_learn = false,
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

	for (i = 0; i < SJA1105_NUM_PORTS; i++) {
		mac[i] = default_mac;
		if (i == dsa_upstream_port(priv->ds, i)) {
			/* STP doesn't get called for CPU port, so we need to
			 * set the I/O parameters statically.
			 */
			mac[i].dyn_learn = true;
			mac[i].ingress = true;
			mac[i].egress = true;
		}
	}

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
		case PHY_INTERFACE_MODE_SGMII:
			mii->xmii_mode[i] = XMII_MODE_SGMII;
			break;
		default:
			dev_err(dev, "Unsupported PHY mode %s!\n",
				phy_modes(ports[i].phy_mode));
		}

		mii->phy_mac[i] = ports[i].xmii_mode;
	}
	return 0;
}

static int sja1105_init_static_fdb(struct sja1105_private *priv)
{
	struct sja1105_table *table;

	table = &priv->static_config.tables[BLK_IDX_L2_LOOKUP];

	/* We only populate the FDB table through dynamic
	 * L2 Address Lookup entries */
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
		/* Include VLAN ID in the FDB entry key => IVL */
		.shared_learn = false,
		/* Don't discard management traffic based on ENFPORT -
		 * we don't perform SMAC port enforcement anyway, so
		 * what we are setting here doesn't matter.
		 */
		.no_enf_hostprt = false,
		/* Don't learn SMAC for mac_fltres1 and mac_fltres0.
		 * TODO Maybe correlate with no_linklocal_learn from bridge driver?
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
	((struct sja1105_l2_lookup_params_entry *) table->entries)[0] =
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
	 * All other VLANs are to be configured through dynamic entries,
	 * and kept in the static configuration table as backing memory.
	 * The pvid of 0 is sufficient to pass traffic while the ports are
	 * standalone and when vlan_filtering is disabled. When filtering
	 * gets enabled, the switchdev core sets up the VLAN ID 1 and sets
	 * it as the new pvid. Actually 'pvid 1' still comes up in 'bridge
	 * vlan' even when vlan_filtering is off, but it has no effect.
	 */
	if (table->entry_count) {
		kfree(table->entries);
		table->entry_count = 0;
	}

	table->entries = kcalloc(1, table->ops->unpacked_entry_size, GFP_KERNEL);
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

	((struct sja1105_vlan_lookup_entry *) table->entries)[0] = pvid;
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
	((struct sja1105_l2_forwarding_params_entry *) table->entries)[0] =
				default_l2fwd_params;

	return 0;
}

static int sja1105_init_general_params(struct sja1105_private *priv)
{
	struct sja1105_general_params_entry default_general_params = {
		/* Disallow dynamic changing of the mirror port */
		.mirr_ptacu = 0,
		/* TODO Find better mapping than just this */
		.switchid = priv->ds->index,
		/* Priority queue for multicast frames trapped to CPU */
		.hostprio = 0,
		/* IEEE 802.3 Annex 57A: Slow Protocols PDUs */
		.mac_fltres1 = 0x0180C2000000,
		.mac_flt1    = 0xFFFFFF000000,
		.incl_srcpt1 = false,
		.send_meta1  = false,
		.mac_fltres0 = 0x011B19000000,
		.mac_flt0    = 0xFFFFFF000000,
		/* IEEE 1588 Annex F: Transport of PTP over Ethernet */
		.mac_fltres0 = 0x011B19000000,
		.mac_flt0    = 0xFFFFFF000000,
		.incl_srcpt0 = false,
		.send_meta0  = false,
		/* No TTEthernet */
		.vllupformat = 0,
		.vlmarker = 0,
		.vlmask = 0,
		/* Only update correctionField for 1-step PTP (L2 transport) */
		.ignore2stf = 0,
		/* Forcefully disable VLAN filtering by telling
		 * the switch that VLAN has a different EtherType.
		 */
		.tpid = ETH_P_EDSA,
		.tpid2 = ETH_P_EDSA,
		/* The destination for traffic matching mac_fltres1 and
		 * mac_fltres0 on all ports except host_port. Such traffic
		 * receieved on host_port itself would be dropped, except
		 * by installing a temporary 'management route'
		 */
		.host_port = dsa_upstream_port(priv->ds, 0),
		/* Same as host port */
		.mirr_port = dsa_upstream_port(priv->ds, 0),
		/* If enabled, traffic that matches mac_fltres1 and mac_fltres0
		 * and is received on casc_port would be forwarded to host_port
		 * without embedding the source port and device ID info in the
		 * destination MAC address (presumably because it is a cascaded
		 * port and a downstream SJA switch already did that).
		 * But we aren't using the incl_srcpt1 feature anyway, but
		 * instead appending pseudo-VLAN tags. So make this invalid.
		 */
		.casc_port = SJA1105_NUM_PORTS,
		/* P/Q/R/S only */
		.queue_ts = 0,
		.egrmirrvid = 0,
		.egrmirrpcp = 0,
		.egrmirrdei = 0,
		.replay_port = 0,
	};
	struct sja1105_table *table;

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
	((struct sja1105_general_params_entry *) table->entries)[0] =
				default_general_params;

	return 0;
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

	/* Override table based on phylib DT bindings */
	table->entry_count = MAX_L2_POLICING_COUNT;

	policing = table->entries;

#define RATE_MBPS(speed) (((speed) * 64000) / 1000)
	/* k sweeps through all unicast policers (0-39).
	 * bcast sweeps through policers 40-44.
	 */
	for (i = 0, k = 0; i < SJA1105_NUM_PORTS; i++) {
		int bcast = (SJA1105_NUM_PORTS * SJA1105_NUM_TC) + i;

		for (j = 0; j < SJA1105_NUM_TC; j++, k++) {
			policing[k].sharindx = k;
			policing[k].smax = 65535; /* Burst size in bytes */
			policing[k].rate = RATE_MBPS(1000);
			policing[k].maxlen = 2043; /* TODO MTU */
			policing[k].partition = 0;
		}
		/* Set up this port's policer for broadcast traffic */
		policing[bcast].sharindx = bcast;
		policing[bcast].smax = 65535; /* Burst size in bytes */
		policing[bcast].rate = RATE_MBPS(1000);
		policing[bcast].maxlen = 2043; /* TODO MTU */
		policing[bcast].partition = 0;
	}
#undef RATE_MBPS
	return 0;
}

static int sja1105_static_config_load(struct sja1105_private *priv,
				      struct sja1105_dt_port *ports)
{
	int rc;

	sja1105_static_config_free(&priv->static_config);
	rc = sja1105_static_config_init(&priv->static_config,
					priv->device_id, priv->part_nr);
	if (rc)
		return rc;

	/* Build static configuration */
	if ((rc = sja1105_init_mac_settings(priv)) < 0)
		return rc;
	if ((rc = sja1105_init_mii_settings(priv, ports)) < 0)
		return rc;
	if ((rc = sja1105_init_static_fdb(priv)) < 0)
		return rc;
	if ((rc = sja1105_init_static_vlan(priv)) < 0)
		return rc;
	if ((rc = sja1105_init_l2_lookup_params(priv)) < 0)
		return rc;
	if ((rc = sja1105_init_l2_forwarding(priv)) < 0)
		return rc;
	if ((rc = sja1105_init_l2_forwarding_params(priv)) < 0)
		return rc;
	if ((rc = sja1105_init_l2_policing(priv)) < 0)
		return rc;
	if ((rc = sja1105_init_general_params(priv)) < 0)
		return rc;

	/* Send initial configuration to hardware via SPI */
	return sja1105_static_config_upload(priv);
}

static int sja1105_parse_ports_node(struct sja1105_dt_port *ports,
				    struct device_node *ports_node)
{
	struct device_node *child;

	for_each_child_of_node(ports_node, child) {
		struct device_node *phy_node;
		int phy_mode;
		u32 index;

		/* Get switch port number from DT */
		if (of_property_read_u32(child, "reg", &index) < 0) {
			pr_err("Port number not defined in device tree (property \"reg\")\n");
			return -ENODEV;
		}

		/* Get PHY mode from DT */
		phy_mode = of_get_phy_mode(child);
		if (phy_mode < 0) {
			pr_err("Failed to read phy-mode or phy-interface-type property for port %d\n",
				index);
			return -ENODEV;
		}
		ports[index].phy_mode = phy_mode;

		phy_node = of_parse_phandle(child, "phy-handle", 0);
		if (!phy_node) {
			if (!of_phy_is_fixed_link(child)) {
				pr_err("phy-handle or fixed-link properties missing!\n");
				return -ENODEV;
			}
			/* phy-handle is missing, but fixed-link isn't.
			 * So it's a fixed link. Default to PHY mode.
			 */
			ports[index].xmii_mode = XMII_PHY;
		} else {
			/* phy-handle present => put port in MAC mode */
			ports[index].xmii_mode = XMII_MAC;
			of_node_put(phy_node);
		}

		/* Allow xmii_mode to be overridden based on explicit bindings */
		if (of_property_read_bool(child, "sja1105,mac-mode"))
			ports[index].xmii_mode = XMII_MAC;
		else if (of_property_read_bool(child, "sja1105,phy-mode"))
			ports[index].xmii_mode = XMII_PHY;
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

	rc = sja1105_parse_ports_node(ports, ports_node);
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

static int sja1105_get_speed_cfg(unsigned int speed_mbps)
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
 * @enabled: Manage Rx and Tx settings for this port. If false, overrides the
 *	     settings from the STP state, but not persistently (does not
 *	     overwrite the static MAC info for this port).
 */
static int sja1105_adjust_port_config(struct sja1105_private *priv, int port,
				      int speed_mbps, bool enabled)
{
	struct sja1105_mac_config_entry dyn_mac;
	struct sja1105_xmii_params_entry *mii;
	struct sja1105_mac_config_entry *mac;
	struct device *dev = priv->ds->dev;
	int xmii_mode;
	int speed;
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
	dyn_mac = mac[port];
	dyn_mac.ingress = enabled && mac[port].ingress;
	dyn_mac.egress  = enabled && mac[port].egress;

	/* Write to the dynamic reconfiguration tables */
	if ((rc = sja1105_dynamic_config_write(priv, BLK_IDX_MAC_CONFIG,
					       port, &dyn_mac, true)) < 0) {
		dev_err(dev, "Failed to write MAC config: %d\n", rc);
		return rc;
	}

	/*
	 * Reconfigure the CGU only for RGMII and SGMII interfaces.
	 * xmii_mode and mac_phy setting cannot change at this point, only
	 * speed does. For MII and RMII no change of the clock setup is
	 * required. Actually, changing the clock setup does interrupt the
	 * clock signal for a certain time which causes trouble for all PHYs
	 * relying on this signal.
	 */
	if (!enabled)
		return 0;

	xmii_mode = mii->xmii_mode[port];
	if ((xmii_mode != XMII_MODE_RGMII) && (xmii_mode != XMII_MODE_SGMII))
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

#define fdb_index(bin, index) \
	((bin) * SJA1105ET_FDB_BIN_SIZE + (index))
#define for_each_bin_index(i) \
	for (i = 0; i < SJA1105ET_FDB_BIN_SIZE; i++)
#define is_bin_index_valid(i) \
	((i) >= 0 && (i) < SJA1105ET_FDB_BIN_SIZE)

static int
sja1105_is_fdb_entry_in_bin(struct sja1105_private *priv, int bin,
			    const u8 *addr, u16 vid,
			    struct sja1105_l2_lookup_entry *fdb_match,
			    int *last_unused)
{
	int index_in_bin;

	for_each_bin_index(index_in_bin) {
		struct sja1105_l2_lookup_entry l2_lookup = { 0 };

		/* Skip unused entries, optionally marking them
		 * into the return value
		 */
		if (sja1105_dynamic_config_read(priv, BLK_IDX_L2_LOOKUP,
				fdb_index(bin, index_in_bin), &l2_lookup)) {
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
		else
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
			 * static_config[BLK_IDX_L2_LOOKUP_PARAMS].entries[0].poly
			 */
			get_random_bytes(&index_in_bin, sizeof(u8));
			index_in_bin %= SJA1105ET_FDB_BIN_SIZE;
			dev_warn(dev, "Warning, FDB bin %d full while adding entry for %pM. Evicting entry %u.\n",
				 bin, addr, index_in_bin);
			/* Evict entry */
			sja1105_dynamic_config_write(priv, BLK_IDX_L2_LOOKUP,
						     fdb_index(bin, index_in_bin),
						     NULL, false);
		}
	}
	l2_lookup.index = fdb_index(bin, index_in_bin);

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
			fdb_index(bin, index_in_bin), &l2_lookup, keep);
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

	/* TODO: Concurrent for loops with read/modify/write cycles
	 * are not a great idea. If this proves to be racy, lock with a mutex.
	 */

	l2_fwd = priv->static_config.tables[BLK_IDX_L2_FORWARDING].entries;

	for (i = 0; i < SJA1105_NUM_PORTS; i++) {
		/* Add this port to the forwarding matrix of the
		 * other ports in the same bridge, and viceversa.
		 */
		if (!dsa_is_user_port(ds, i))
			continue;
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

static void sja1105_bridge_stp_state_set(struct dsa_switch *ds, int port, u8 state)
{
	struct sja1105_private *priv = ds->priv;
	struct sja1105_mac_config_entry *mac;

	mac = priv->static_config.tables[BLK_IDX_MAC_CONFIG].entries;

	switch (state) {
	case BR_STATE_DISABLED:
	case BR_STATE_BLOCKING:
		/* From UM10944 description of DRPDTAG (why put this there?):
		 * "Management traffic flows to the port regardless of the state
		 * of the INGRESS flag". So BPDUs are still be allowed to pass.
		 * TODO make a difference between DISABLED and BLOCKING.
		 */
		mac[port].ingress   = false;
		mac[port].egress    = false;
		mac[port].dyn_learn = false;
		break;
	case BR_STATE_LISTENING:
		mac[port].ingress   = true;
		mac[port].egress    = false;
		mac[port].dyn_learn = false;
		break;
	case BR_STATE_LEARNING:
		mac[port].ingress   = true;
		mac[port].egress    = false;
		mac[port].dyn_learn = true;
		break;
	case BR_STATE_FORWARDING:
		mac[port].ingress   = true;
		mac[port].egress    = true;
		mac[port].dyn_learn = true;
		break;
	default:
		dev_err(ds->dev, "invalid STP state: %d\n", state);
		return;
	}

	sja1105_dynamic_config_write(priv, BLK_IDX_MAC_CONFIG, port,
				    &mac[port], true);
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

static u8 sja1105_stp_state_get(struct sja1105_private *priv, int port)
{
	struct sja1105_mac_config_entry *mac;

	mac = priv->static_config.tables[BLK_IDX_MAC_CONFIG].entries;

	if (!mac[port].ingress && !mac[port].egress && !mac[port].dyn_learn)
		return BR_STATE_BLOCKING;
	if (mac[port].ingress && !mac[port].egress && !mac[port].dyn_learn)
		return BR_STATE_LISTENING;
	if (mac[port].ingress && !mac[port].egress && mac[port].dyn_learn)
		return BR_STATE_LEARNING;
	if (mac[port].ingress && mac[port].egress && mac[port].dyn_learn)
		return BR_STATE_FORWARDING;
	return -EINVAL;
}

/* For situations where we need to change a setting at runtime that is only
 * available through the static configuration, resetting the switch in order
 * to upload the new static config is unavoidable. Back up the settings we
 * modify at runtime (currently only MAC) and restore them after uploading,
 * such that this operation is relatively seamless.
 */
static int sja1105_static_config_reload(struct sja1105_private *priv)
{
	struct sja1105_mac_config_entry *mac;
	int speed_mbps[SJA1105_NUM_PORTS];
	u8 stp_state[SJA1105_NUM_PORTS];
	int rc, i;

	/* TODO: This might be both racy, and difficult to lock down with a mutex.
	 * Or neither.
	 */
	mac = priv->static_config.tables[BLK_IDX_MAC_CONFIG].entries;

	/* Back up settings changed by sja1105_adjust_port_config and
	 * sja1105_bridge_stp_state_set and restore their defaults.
	 */
	for (i = 0; i < SJA1105_NUM_PORTS; i++) {
		speed_mbps[i] = sja1105_speed[mac[i].speed];
		mac[i].speed = SJA1105_SPEED_AUTO;
		if (i == dsa_upstream_port(priv->ds, i)) {
			mac[i].ingress = true;
			mac[i].egress = true;
			mac[i].dyn_learn = true;
		} else {
			stp_state[i] = sja1105_stp_state_get(priv, i);
			mac[i].ingress = false;
			mac[i].egress = false;
			mac[i].dyn_learn = false;
		}
	}

	/* Reset switch and send updated static configuration */
	if ((rc = sja1105_static_config_upload(priv)) < 0)
		goto out;

	/* Configure the CGU (PLLs) for MII and RMII PHYs.
	 * For these interfaces there is no dynamic configuration
	 * needed, since PLLs have same settings at all speeds.
	 */
	if ((rc = sja1105_clocking_setup(priv)) < 0)
		goto out;

	for (i = 0; i < SJA1105_NUM_PORTS; i++) {
		bool enabled = (speed_mbps[i] != SJA1105_SPEED_AUTO);
		if (i != dsa_upstream_port(priv->ds, i))
			sja1105_bridge_stp_state_set(priv->ds, i, stp_state[i]);
		rc = sja1105_adjust_port_config(priv, i, speed_mbps[i], enabled);
		if (rc < 0)
			goto out;
	}
out:
	return rc;
}

#define sja1105_vlan_filtering_enabled(priv) \
	(((struct sja1105_general_params_entry *) \
	((struct sja1105_private *) priv)->static_config. \
	tables[BLK_IDX_GENERAL_PARAMS].entries)->tpid == ETH_P_8021Q)

/* The TPID setting belongs to the General Parameters table,
 * which can only be partially reconfigured at runtime (and not the TPID).
 * So a switch reset is required.
 */
static int sja1105_change_tpid(struct sja1105_private *priv, u16 tpid, u16 tpid2)
{
	struct sja1105_general_params_entry *general_params;
	struct sja1105_table *table;

	table = &priv->static_config.tables[BLK_IDX_GENERAL_PARAMS];
	general_params = table->entries;
	general_params->tpid = tpid;
	general_params->tpid2 = tpid2;
	return sja1105_static_config_reload(priv);
}

static int sja1105_pvid_apply(struct sja1105_private *priv, int port, u16 pvid)
{
	struct sja1105_mac_config_entry *mac;

	mac = priv->static_config.tables[BLK_IDX_MAC_CONFIG].entries;

	mac[port].vlanid = pvid;

	return sja1105_dynamic_config_write(priv, BLK_IDX_MAC_CONFIG, port,
					   &mac[port], true);
}

static int sja1105_is_vlan_configured(struct sja1105_private *priv, u16 vid)
{
	struct sja1105_vlan_lookup_entry *vlan;
	int count, i;

	vlan = priv->static_config.tables[BLK_IDX_VLAN_LOOKUP].entries;
	count = priv->static_config.tables[BLK_IDX_VLAN_LOOKUP].entry_count;

	for (i = 0; i < count; i++)
		if (vlan[i].vlanid == vid)
			return i;

	/* Return an invalid entry index if not found */
	return -1;
}

static int sja1105_vlan_apply(struct sja1105_private *priv, int port, u16 vid,
			      bool enabled, bool untagged)
{
	struct sja1105_vlan_lookup_entry *vlan;
	struct sja1105_table *table;
	bool keep = true;
	int match, rc;

	table = &priv->static_config.tables[BLK_IDX_VLAN_LOOKUP];

	match = sja1105_is_vlan_configured(priv, vid);
	if (match < 0) {
		/* Can't delete a missing entry. */
		if (!enabled)
			return 0;
		rc = sja1105_table_resize(table, table->entry_count + 1);
		if (rc)
			return rc;
		match = table->entry_count - 1;
	}
	/* Assign pointer after the resize (it's new memory) */
	vlan = table->entries;
	vlan[match].vlanid = vid;
	if (enabled) {
		vlan[match].vlan_bc |= BIT(port);
		vlan[match].vmemb_port |= BIT(port);
	} else {
		vlan[match].vlan_bc &= ~BIT(port);
		vlan[match].vmemb_port &= ~BIT(port);
	}
	/* Also unset tag_port if removing this VLAN was requested,
	 * just so we don't have a confusing bitmap (no practical purpose).
	 */
	if (untagged || !enabled)
		vlan[match].tag_port &= ~BIT(port);
	else
		vlan[match].tag_port |= BIT(port);
	/* If there's no port left as member of this VLAN,
	 * it's time for it to go.
	 */
	if (!vlan[match].vmemb_port)
		keep = false;

	dev_err(priv->ds->dev,
		"%s: port %d, vid %llu, broadcast domain 0x%llx, "
		"port members 0x%llx, tagged ports 0x%llx, keep %d\n",
		__func__, port, vlan[match].vlanid, vlan[match].vlan_bc,
		vlan[match].vmemb_port, vlan[match].tag_port, keep);

	rc = sja1105_dynamic_config_write(priv, BLK_IDX_VLAN_LOOKUP, vid,
					 &vlan[match], keep);
	if (rc < 0)
		return rc;

	if (!keep)
		return sja1105_table_delete_entry(table, match);

	return 0;
}

static enum dsa_tag_protocol
sja1105_get_tag_protocol(struct dsa_switch *ds, int port)
{
	return DSA_TAG_PROTO_SJA1105;
}

static int sja1105_set_drop_policy(struct dsa_switch *ds, int port,
				   bool drop_untagged, bool drop_double_tagged)
{
	struct sja1105_private *priv = ds->priv;
	struct sja1105_mac_config_entry *mac;

	mac = priv->static_config.tables[BLK_IDX_MAC_CONFIG].entries;

	mac[port].drpdtag = drop_double_tagged;
	mac[port].drpuntag = drop_untagged;

	return sja1105_dynamic_config_write(priv, BLK_IDX_MAC_CONFIG, port,
					   &mac[port], true);
}

static u16 sja1105_tagging_vid_from_port(struct dsa_switch *ds, int port)
{
	return 400 + port;
}

static int sja1105_tagging_vid_to_port(struct dsa_switch *ds, u16 vid)
{
	return vid - 400;
}

#if 0
static int sja1105_setup_tag_protocol(struct dsa_switch *ds, bool enabled)
{
	struct sja1105_private *priv = ds->priv;
	int rc, port;

	for (port = 0; port < SJA1105_NUM_PORTS; port++) {
		int upstream = dsa_upstream_port(ds, port);
		u16 pvid = sja1105_tag_vid_from_port(port);

		/* CPU port is implicitly configured by configuring the user
		 * ports: we need to add their pvid to its VLAN port
		 * membership. The only specific thing to do for CPU port is
		 * to disallow untagged input if the switch tagging is enabled.
		 */
		if (port == upstream) {
			rc = sja1105_set_drop_policy(priv, upstream, enabled,
						     false);
			if (rc < 0)
				return rc;
			continue;
		}
		/* Add the user port's pvid to the CPU port's trunk
		 * (tagged egress). Or remove it, if "enabled" indicates so.
		 */
		rc = sja1105_vlan_apply(priv, upstream, pvid, enabled, false);
		if (rc < 0) {
			dev_err(ds->dev, "Failed to add tagging vid %d to CPU port: %d\n",
				pvid, rc);
			return rc;
		}
		/* Set up user port as untagged egress */
		rc = sja1105_vlan_apply(priv, port, pvid, enabled, true);
		if (rc < 0) {
			dev_err(ds->dev, "Failed to apply tagging pvid %d to port %d: %d\n",
				pvid, port, rc);
			return rc;
		}
		/* Finally apply a unique pvid to the user port */
		rc = sja1105_pvid_apply(priv, port, enabled ? pvid : 0);
		if (rc < 0) {
			dev_err(ds->dev, "Failed to set %d on port %d as pvid: %d\n",
				pvid, port, rc);
			return rc;
		}
	}
	return 0;
}
#endif

/* This callback needs to be present */
static int sja1105_vlan_prepare(struct dsa_switch *ds, int port,
				const struct switchdev_obj_port_vlan *vlan)
{
	return 0;
}

static int sja1105_vlan_filtering(struct dsa_switch *ds, int port, bool enabled)
{
	struct sja1105_private *priv = ds->priv;
	int rc = 0, i;

	/* On SJA1105, VLAN filtering per se is always enabled in hardware.
	 * The only thing we can do to disable it is lie about what the 802.1Q
	 * EtherType is. So it will still try to apply VLAN filtering, but all
	 * ingress traffic (except frames received with EtherType of ETH_P_EDSA,
	 * which are invalid) will be internally tagged with a distorted VLAN
	 * header where the TPID is ETH_P_EDSA, and the VLAN ID is the port pvid.
	 * So since this operation is global to the switch, we need to handle
	 * the case where multiple bridges span the same switch device and one
	 * of them has a different setting than what is being requested.
	 */
	for (i = 0; i < SJA1105_NUM_PORTS; i++) {
		struct net_device *bridge_dev;

		bridge_dev = dsa_to_port(ds, i)->bridge_dev;
		if (bridge_dev &&
		    bridge_dev != dsa_to_port(ds, port)->bridge_dev &&
		    br_vlan_enabled(bridge_dev) != enabled) {
			netdev_err(bridge_dev,
				   "VLAN filtering is global to the switch!\n");
			return -EINVAL;
		}
	}

	if (enabled && !sja1105_vlan_filtering_enabled(priv))
		/* Enable VLAN filtering. TODO read these from bridge_dev->protocol. */
		rc = sja1105_change_tpid(priv, ETH_P_8021Q, ETH_P_8021AD);
	else if (!enabled && sja1105_vlan_filtering_enabled(priv))
		/* Disable VLAN filtering. TODO: Install a TPID
		 * that also encodes the switch ID (aka ds->index)
		 * so that stacking switch tags will be supported.
		 */
		rc = sja1105_change_tpid(priv, ETH_P_EDSA, ETH_P_EDSA);
	else
		return 0;
	if (rc)
		dev_err(ds->dev, "Failed to change VLAN Ethertype\n");

	return rc;
}

static void sja1105_vlan_add(struct dsa_switch *ds, int port,
			     const struct switchdev_obj_port_vlan *vlan)
{
	u16 vid;
	int rc;

	for (vid = vlan->vid_begin; vid <= vlan->vid_end; vid++) {
		rc = sja1105_vlan_apply(ds->priv, port, vid, true,
					vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED);
		if (rc < 0) {
			dev_err(ds->dev, "Failed to add VLAN %d to port %d: %d\n",
				vid, port, rc);
			return;
		}
		if (vlan->flags & BRIDGE_VLAN_INFO_PVID) {
			rc = sja1105_pvid_apply(ds->priv, port, vid);
			if (rc < 0) {
				dev_err(ds->dev, "Failed to set pvid %d on port %d: %d\n",
					vid, port, rc);
				return;
			}
		}
	}
}

static int sja1105_vlan_del(struct dsa_switch *ds, int port,
			    const struct switchdev_obj_port_vlan *vlan)
{
	u16 vid;
	int rc;

	for (vid = vlan->vid_begin; vid <= vlan->vid_end; vid++) {
		rc = sja1105_vlan_apply(ds->priv, port, vid, false,
					vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED);
		if (rc < 0) {
			dev_err(ds->dev, "Failed to remove VLAN %d from port %d: %d\n",
				vid, port, rc);
			return rc;
		}
	}
	return 0;
}

/* Currently I have no idea how this gets called */
static int sja1105_mirror_apply(struct sja1105_private *priv, int port,
				bool ingress, bool enabled)
{
	struct sja1105_mac_config_entry *mac;

	mac = priv->static_config.tables[BLK_IDX_MAC_CONFIG].entries;

	if (ingress)
		mac[port].ing_mirr = enabled;
	else
		mac[port].egr_mirr = enabled;

	return sja1105_dynamic_config_write(priv, BLK_IDX_MAC_CONFIG, port,
					   &mac[port], true);
}

static int sja1105_mirror_add(struct dsa_switch *ds, int port,
			      struct dsa_mall_mirror_tc_entry *mirror,
			      bool ingress)
{
	pr_err("%s: port %d ingress %d mirror ingress %d to_local_port %d\n", __func__, port, ingress, mirror->ingress, mirror->to_local_port);
	return sja1105_mirror_apply(ds->priv, port, ingress, true);
}

static void sja1105_mirror_del(struct dsa_switch *ds, int port,
		    struct dsa_mall_mirror_tc_entry *mirror)
{
	pr_err("%s: port %d mirror ingress %d to_local_port %d\n", __func__, port, mirror->ingress, mirror->to_local_port);
	sja1105_mirror_apply(ds->priv, port, mirror->ingress, false);
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

	if ((rc = sja1105_parse_dt(priv, ports)) < 0) {
		dev_err(ds->dev, "Failed to parse DT: %d\n", rc);
		return rc;
	}
	/* Create and send configuration down to device */
	if ((rc = sja1105_static_config_load(priv, ports)) < 0) {
		dev_err(ds->dev, "Failed to load static config: %d\n", rc);
		return rc;
	}
	/* Configure the CGU (PHY link modes and speeds) */
	if ((rc = sja1105_clocking_setup(priv)) < 0) {
		dev_err(ds->dev, "Failed to configure MII clocking: %d\n", rc);
		return rc;
	}
	return 0;
}

#include "../../../net/dsa/dsa_priv.h"
/* Deferred work is unfortunately necessary because setting up the management
 * route cannot be done from atomit context (SPI transfer takes a sleepable
 * lock on the bus)
 */
void sja1105_xmit_work_handler(struct work_struct *work)
{
	struct sja1105_port *sp = container_of(work, struct sja1105_port,
						xmit_work);
	struct sja1105_private *priv = sp->dp->ds->priv;
	struct net_device *slave = sp->dp->slave;
	struct net_device *master = dsa_slave_to_master(slave);
	int port = (uintptr_t) (sp - priv->ports);
	struct sk_buff *skb;
	int i, rc;

	while ((i = sja1105_skb_ring_get(&sp->xmit_ring, &skb)) >= 0) {

		struct sja1105_mgmt_entry mgmt_route = { 0 };
		struct ethhdr *hdr;
		int timeout = 10;
		int skb_len;
		u64 dmac;

		skb_len = skb->len;
		hdr = eth_hdr(skb);
		dmac = ether_addr_to_u64(hdr->h_dest);

		dev_err(priv->ds->dev, "xmit: port %d mgmt_route src %pM dst %pM\n", port, hdr->h_source, hdr->h_dest);

		mgmt_route.macaddr = ether_addr_to_u64(hdr->h_dest);
		mgmt_route.destports = BIT(port);
		mgmt_route.enfport = 1;
		mgmt_route.tsreg = 0;
		mgmt_route.takets = true;

		rc = sja1105_dynamic_config_write(priv, BLK_IDX_MGMT_ROUTE,
						  port, &mgmt_route, true);
		if (rc < 0) {
			kfree_skb(skb);
			slave->stats.tx_dropped++;
			continue;
		}

		/* Transfer skb to the host port. */
		skb->dev = master;
		dev_queue_xmit(skb);

		/* Wait until the switch has processed the frame */
		do {
			rc = sja1105_dynamic_config_read(priv, BLK_IDX_MGMT_ROUTE,
							 port, &mgmt_route);
			if (rc < 0) {
				slave->stats.tx_errors++;
				dev_err(priv->ds->dev, "xmit: failed to poll for mgmt route: %d\n", rc);
				continue;
			}

			/* UM10944: The ENFPORT flag of the respective entry is
			 * cleared when a match is found. The host can use this
			 * flag as an acknowledgement.
			 */
			usleep_range(1000, 2000);
		} while (mgmt_route.enfport && --timeout);

		if (!timeout) {
			dev_err(priv->ds->dev, "xmit timed out\n");
			slave->stats.tx_errors++;
			continue;
		}

		slave->stats.tx_packets++;
		slave->stats.tx_bytes += skb_len;
	}
#if 0
	/* TODO setup a dedicated netdev queue for management traffic so that
	 * we can selectively apply backpressure and not be required to stop
	 * the entire traffic when the software skb ring is full. This requires
	 * hooking the ndo_select_queue from DSA and matching on mac_fltres.
	 */
	if (netif_queue_stopped(slave))
		netif_wake_queue(slave);
#endif
}

static int sja1105_port_enable(struct dsa_switch *ds, int port,
			       struct phy_device *phy)
{
	struct sja1105_private *priv = ds->priv;
	struct sja1105_port *sp = &priv->ports[port];

	sp->dp = &ds->ports[port];
	INIT_WORK(&sp->xmit_work, sja1105_xmit_work_handler);
	return 0;
}

static void sja1105_port_disable(struct dsa_switch *ds, int port)
{
	struct sja1105_private *priv = ds->priv;
	struct sja1105_port *sp = &priv->ports[port];
	struct sk_buff *skb;

	cancel_work_sync(&sp->xmit_work);
	while (sja1105_skb_ring_get(&sp->xmit_ring, &skb) >= 0)
		kfree_skb(skb);
}

static const struct dsa_switch_ops sja1105_switch_ops = {
	.get_tag_protocol	= sja1105_get_tag_protocol,
	.setup			= sja1105_setup,
	.adjust_link		= sja1105_adjust_link,
	.get_strings		= sja1105_get_strings,
	.get_ethtool_stats	= sja1105_get_ethtool_stats,
	.get_sset_count		= sja1105_get_sset_count,
	.port_fdb_dump		= sja1105_fdb_dump,
	.port_fdb_add		= sja1105_fdb_add,
	.port_fdb_del		= sja1105_fdb_del,
	.port_bridge_join	= sja1105_bridge_join,
	.port_bridge_leave	= sja1105_bridge_leave,
	.port_stp_state_set	= sja1105_bridge_stp_state_set,
	.port_vlan_prepare	= sja1105_vlan_prepare,
	.port_vlan_filtering	= sja1105_vlan_filtering,
	.port_vlan_add		= sja1105_vlan_add,
	.port_vlan_del		= sja1105_vlan_del,
	.port_mdb_prepare	= sja1105_mdb_prepare,
	.port_mdb_add		= sja1105_mdb_add,
	.port_mdb_del		= sja1105_mdb_del,
	.port_mirror_add	= sja1105_mirror_add,
	.port_mirror_del	= sja1105_mirror_del,
	.port_set_drop_policy	= sja1105_set_drop_policy,
	.port_enable		= sja1105_port_enable,
	.port_disable		= sja1105_port_disable,
	.tagging_vid_to_port	= sja1105_tagging_vid_to_port,
	.tagging_vid_from_port	= sja1105_tagging_vid_from_port,
};

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

	ds = dsa_switch_alloc(dev, SJA1105_NUM_PORTS);
	if (!ds)
		return -ENOMEM;

	ds->ops = &sja1105_switch_ops;
	ds->priv = priv;
	priv->ds = ds;

	/* Populate our driver private structure (priv) based on
	 * the device tree node that was probed (spi)
	 */
	priv->spidev = spi;
	spi_set_drvdata(spi, priv);

	/* Configure the SPI bus */
	spi->mode = SPI_CPHA;
	spi->bits_per_word = 8;
	if ((rc = spi_setup(spi)) < 0) {
		dev_err(dev, "Could not init SPI\n");
		return rc;
	}

	/* Configure the optional reset pin and bring up switch */
	priv->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset_gpio))
		dev_dbg(dev, "reset-gpios not defined, ignoring\n");
	else
		sja1105_hw_reset(priv->reset_gpio, 1, 1);

	/* Detect hardware device */
	if ((rc = sja1105_device_id_get(priv)) < 0)
		return rc;

	dev_dbg(dev, "Probed switch chip: %s\n", sja1105_device_id_string_get(
		priv->device_id, priv->part_nr));

	if ((rc = sja1105_dynamic_config_init(priv)) < 0)
		return rc;

	if ((rc = dsa_register_switch(priv->ds)) < 0)
		return rc;

	return 0;
}

static int sja1105_remove(struct spi_device *spi)
{
	struct sja1105_private *priv = spi_get_drvdata(spi);

	dsa_unregister_switch(priv->ds);
	sja1105_static_config_free(&priv->static_config);
	return 0;
}

static const struct of_device_id sja1105_dt_ids[] = {
	{ .compatible = "nxp,sja1105e" },
	{ .compatible = "nxp,sja1105t" },
	{ .compatible = "nxp,sja1105p" },
	{ .compatible = "nxp,sja1105q" },
	{ .compatible = "nxp,sja1105r" },
	{ .compatible = "nxp,sja1105s" },
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
