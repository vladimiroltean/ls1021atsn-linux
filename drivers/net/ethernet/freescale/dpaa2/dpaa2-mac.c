// SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause)
/* Copyright 2015 Freescale Semiconductor Inc.
 * Copyright 2018-2019 NXP
 */
#include <linux/module.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/msi.h>
#include <linux/rtnetlink.h>
#include <linux/if_vlan.h>

#include <uapi/linux/if_bridge.h>
#include <net/netlink.h>

#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phylink.h>
#include <linux/notifier.h>

#include <linux/fsl/mc.h>

#include "dpmac.h"
#include "dpmac-cmd.h"

struct dpaa2_mac_priv {
	struct net_device		*netdev;
	struct fsl_mc_device		*mc_dev;
	struct phylink			*phylink;
	struct dpmac_attr		attr;
	struct dpmac_link_state		old_state;
	u16				dpmac_ver_major;
	u16				dpmac_ver_minor;
	struct notifier_block phylink_nb;
};

/*
 * This must be kept in sync with enum dpmac_eth_if.
 */
static phy_interface_t dpaa2_mac_iface_mode[] =  {
	PHY_INTERFACE_MODE_MII,		/* DPMAC_ETH_IF_MII */
	PHY_INTERFACE_MODE_RMII,	/* DPMAC_ETH_IF_RMII */
	PHY_INTERFACE_MODE_SMII,	/* DPMAC_ETH_IF_SMII */
	PHY_INTERFACE_MODE_GMII,	/* DPMAC_ETH_IF_GMII */
	PHY_INTERFACE_MODE_RGMII,	/* DPMAC_ETH_IF_RGMII */
	PHY_INTERFACE_MODE_SGMII,	/* DPMAC_ETH_IF_SGMII */
	PHY_INTERFACE_MODE_QSGMII,	/* DPMAC_ETH_IF_QSGMII */
	PHY_INTERFACE_MODE_XAUI,	/* DPMAC_ETH_IF_XAUI */
	PHY_INTERFACE_MODE_10GKR,	/* DPMAC_ETH_IF_XFI */
	PHY_INTERFACE_MODE_XGMII,	/* DPMAC_ETH_IF_CAUI */
	PHY_INTERFACE_MODE_1000BASEX,	/* DPMAC_ETH_IF_1000BASEX */
	PHY_INTERFACE_MODE_XGMII,	/* DPMAC_ETH_IF_USXGMII */
};

static int cmp_dpmac_ver(struct dpaa2_mac_priv *priv,
			 u16 ver_major, u16 ver_minor)
{
	if (priv->dpmac_ver_major == ver_major)
		return priv->dpmac_ver_minor - ver_minor;
	return priv->dpmac_ver_major - ver_major;
}

#define DPMAC_LINK_AUTONEG_VER_MAJOR		4
#define DPMAC_LINK_AUTONEG_VER_MINOR		3

struct dpaa2_mac_link_mode_map {
	u64 dpmac_lm;
	u64 ethtool_lm;
};

static const struct dpaa2_mac_link_mode_map dpaa2_mac_lm_map[] = {
	{DPMAC_ADVERTISED_10BASET_FULL, ETHTOOL_LINK_MODE_10baseT_Full_BIT},
	{DPMAC_ADVERTISED_100BASET_FULL, ETHTOOL_LINK_MODE_100baseT_Full_BIT},
	{DPMAC_ADVERTISED_1000BASET_FULL, ETHTOOL_LINK_MODE_1000baseT_Full_BIT},
	{DPMAC_ADVERTISED_10000BASET_FULL, ETHTOOL_LINK_MODE_10000baseT_Full_BIT},
	{DPMAC_ADVERTISED_2500BASEX_FULL, ETHTOOL_LINK_MODE_2500baseT_Full_BIT},
	{DPMAC_ADVERTISED_AUTONEG, ETHTOOL_LINK_MODE_Autoneg_BIT},
};

static void link_mode_dpmac2phydev(u64 dpmac_lm, unsigned long *phydev_lm)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dpaa2_mac_lm_map); i++) {
		if (dpmac_lm & dpaa2_mac_lm_map[i].dpmac_lm) {
			linkmode_set_bit(dpaa2_mac_lm_map[i].ethtool_lm, phydev_lm);
			printk(KERN_ERR "%s[%d]: %lld\n", __func__, __LINE__, dpaa2_mac_lm_map[i].dpmac_lm);
		}
	}
}

static void link_mode_phydev2dpmac(unsigned long *phydev_lm, u64 *dpni_lm)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dpaa2_mac_lm_map); i++) {
		if (linkmode_test_bit(dpaa2_mac_lm_map[i].ethtool_lm, phydev_lm))
			*dpni_lm |= dpaa2_mac_lm_map[i].dpmac_lm;
	}
}

static irqreturn_t dpaa2_mac_irq_handler(int irq_num, void *arg)
{
	struct device *dev = (struct device *)arg;
	struct fsl_mc_device *mc_dev = to_fsl_mc_device(dev);
	struct dpaa2_mac_priv *priv = dev_get_drvdata(dev);
	struct dpmac_link_cfg link_cfg = { 0 };
	u32 status;
	int err;

	err = dpmac_get_irq_status(mc_dev->mc_io, 0, mc_dev->mc_handle,
	DPMAC_IRQ_INDEX, &status);
	if (unlikely(err || !status))
	return IRQ_NONE;

	/* DPNI-initiated link configuration; 'ifconfig up' also calls this */
	if (status & DPMAC_IRQ_EVENT_LINK_CFG_REQ) {
		rtnl_lock();
		phylink_stop(priv->phylink);
		phylink_start(priv->phylink);
		rtnl_unlock();
	}

	dpmac_clear_irq_status(mc_dev->mc_io, 0, mc_dev->mc_handle,
                      	      DPMAC_IRQ_INDEX, status);

	printk(KERN_ERR "%s %d\n", __func__, __LINE__);

	return IRQ_HANDLED;
}

static int setup_irqs(struct fsl_mc_device *mc_dev)
{
	int err = 0;
	struct fsl_mc_device_irq *irq;

	err = fsl_mc_allocate_irqs(mc_dev);
	if (err) {
		dev_err(&mc_dev->dev, "fsl_mc_allocate_irqs err %d\n", err);
		return err;
	}

	irq = mc_dev->irqs[0];
	err = devm_request_threaded_irq(&mc_dev->dev, irq->msi_desc->irq,
					NULL, &dpaa2_mac_irq_handler,
					IRQF_NO_SUSPEND | IRQF_ONESHOT,
					dev_name(&mc_dev->dev), &mc_dev->dev);
	if (err) {
		dev_err(&mc_dev->dev, "devm_request_threaded_irq err %d\n",
			err);
		goto free_irq;
	}

	err = dpmac_set_irq_mask(mc_dev->mc_io, 0, mc_dev->mc_handle,
				 DPMAC_IRQ_INDEX, DPMAC_IRQ_EVENT_LINK_CFG_REQ);
	if (err) {
		dev_err(&mc_dev->dev, "dpmac_set_irq_mask err %d\n", err);
		goto free_irq;
	}
	err = dpmac_set_irq_enable(mc_dev->mc_io, 0, mc_dev->mc_handle,
				   DPMAC_IRQ_INDEX, 1);
	if (err) {
		dev_err(&mc_dev->dev, "dpmac_set_irq_enable err %d\n", err);
		goto free_irq;
	}

	return 0;

free_irq:
	fsl_mc_free_irqs(mc_dev);

	return err;
}

static void teardown_irqs(struct fsl_mc_device *mc_dev)
{
	int err;

	err = dpmac_set_irq_enable(mc_dev->mc_io, 0, mc_dev->mc_handle,
				   DPMAC_IRQ_INDEX, 0);
	if (err)
		dev_err(&mc_dev->dev, "dpmac_set_irq_enable err %d\n", err);

	fsl_mc_free_irqs(mc_dev);
}

static struct device_node *find_dpmac_node(struct device *dev, u16 dpmac_id)
{
	struct device_node *dpmacs, *dpmac = NULL;
	struct device_node *mc_node = dev->of_node;
	u32 id;
	int err;

	dpmacs = of_find_node_by_name(mc_node, "dpmacs");
	if (!dpmacs) {
		dev_err(dev, "No dpmacs subnode in device-tree\n");
		return NULL;
	}

	while ((dpmac = of_get_next_child(dpmacs, dpmac))) {
		err = of_property_read_u32(dpmac, "reg", &id);
		if (err)
			continue;
		if (id == dpmac_id)
			return dpmac;
	}

	return NULL;
}

static void dpaa2_mac_validate(struct dpaa2_mac_priv *priv,
			       unsigned long *supported,
			       struct phylink_link_state *state)
{
	__ETHTOOL_DECLARE_LINK_MODE_MASK(mask) = { 0, };
	struct fsl_mc_device *mc_dev = priv->mc_dev;
	struct dpmac_link_cfg cfg;
	int err, i;

	err = dpmac_get_link_cfg_v2(mc_dev->mc_io, 0,
				    mc_dev->mc_handle,
				    &cfg);
	printk(KERN_ERR "options = %llu\n", cfg.options);
	printk(KERN_ERR "options = %llu\n", cfg.advertising);
	printk(KERN_ERR "options = %d\n", cfg.rate);


	/* TODO cfg not set before interrupt __I think __*/
	if (cfg.advertising) {
		link_mode_dpmac2phydev(cfg.advertising, mask);
	} else {
		for (i = 0; i < ARRAY_SIZE(dpaa2_mac_lm_map); i++) {
			linkmode_set_bit(dpaa2_mac_lm_map[i].ethtool_lm, mask);
			printk(KERN_ERR "%s[%d]: %lld\n", __func__, __LINE__, dpaa2_mac_lm_map[i].dpmac_lm);
		}
	}

	if (cfg.options & DPMAC_LINK_OPT_AUTONEG) {
		printk(KERN_ERR "%s[%d]: autoneg\n", __func__, __LINE__);
		phylink_set(mask, Autoneg);
	}
	phylink_set_port_modes(mask);
	if (cfg.options & DPMAC_LINK_OPT_PAUSE) {
		printk(KERN_ERR "%s[%d]: pause\n", __func__, __LINE__);
		phylink_set(mask, Pause);
	}
	if (cfg.options & DPMAC_LINK_OPT_ASYM_PAUSE) {
		printk(KERN_ERR "%s[%d]: asym pause\n", __func__, __LINE__);
		phylink_set(mask, Asym_Pause);
	}

	bitmap_and(supported, supported, mask,
		   __ETHTOOL_LINK_MODE_MASK_NBITS);
	bitmap_and(state->advertising, state->advertising, mask,
		   __ETHTOOL_LINK_MODE_MASK_NBITS);
}

static int dpaa2_mac_link_state(struct dpaa2_mac_priv *priv,
				struct phylink_link_state *state)
{
	struct dpmac_link_cfg link_cfg = { 0 };
	struct fsl_mc_device *mc_dev = priv->mc_dev;
	int err;

	err = dpmac_get_link_cfg_v2(mc_dev->mc_io, 0,
				    mc_dev->mc_handle,
				    &link_cfg);

	state->speed = link_cfg.rate;
	state->duplex  = !!(link_cfg.options & DPMAC_LINK_OPT_HALF_DUPLEX);

	printk(KERN_ERR "%s %d: id = %d\n", __func__, __LINE__, priv->attr.id);
	return 0;
}

static void dpaa2_mac_an_restart(struct dpaa2_mac_priv *priv)
{
	printk(KERN_ERR "%s %d: id = %d\n", __func__, __LINE__, priv->attr.id);
}

static void dpaa2_mac_config(struct dpaa2_mac_priv *priv, unsigned int mode,
			     const struct phylink_link_state *state)
{
	struct fsl_mc_device *mc_dev = priv->mc_dev;
	struct dpmac_link_state mac_state = { 0 };
	int err;

	mac_state.up = state->link;
	if (state->link) {
		mac_state.rate = state->speed;

		if (!state->duplex)
			mac_state.options |= DPMAC_LINK_OPT_HALF_DUPLEX;
		if (!state->an_enabled)
			mac_state.options |= DPMAC_LINK_OPT_AUTONEG;
		if (state->pause && MLO_PAUSE_SYM && phylink_test(state->advertising, Pause))
			mac_state.options |= DPMAC_LINK_OPT_PAUSE;
		if (state->pause && MLO_PAUSE_ASYM && phylink_test(state->advertising, Asym_Pause))
			mac_state.options |= DPMAC_LINK_OPT_PAUSE;
	}

	if (priv->old_state.up != mac_state.up ||
	    priv->old_state.rate != mac_state.rate ||
	    priv->old_state.options != mac_state.rate) {
		priv->old_state = mac_state;
	}

	link_mode_phydev2dpmac(state->advertising, &mac_state.advertising);
	/* this should be got from the link cfg... I think */
	link_mode_phydev2dpmac(state->advertising, &mac_state.supported);
	err = dpmac_set_link_state_v2(priv->mc_dev->mc_io, 0,
				priv->mc_dev->mc_handle, &mac_state);

	if (unlikely(err))
		dev_err(&priv->mc_dev->dev, "dpmac_set_link_state: %d\n", err);

	printk(KERN_ERR "%s %d: id = %d\n", __func__, __LINE__, priv->attr.id);
	printk(KERN_ERR "%s %d: id = %d | speed = %d\n", __func__, __LINE__, priv->attr.id, state->speed);
	printk(KERN_ERR "%s %d: id = %d | duplex = %d\n", __func__, __LINE__, priv->attr.id, state->duplex);
	printk(KERN_ERR "%s %d: id = %d | pause = %d\n", __func__, __LINE__, priv->attr.id, state->pause);
}

static void dpaa2_mac_link_down(struct dpaa2_mac_priv *priv, unsigned int mode,
				phy_interface_t interface)
{
	struct dpmac_link_state state;

	state = priv->old_state;
	state.up = 0;
	state.state_valid = 1;

	dpmac_set_link_state_v2(priv->mc_dev->mc_io, 0,
				priv->mc_dev->mc_handle, &state);

	printk(KERN_ERR "%s %d: id = %d\n", __func__, __LINE__, priv->attr.id);
}

static void dpaa2_mac_link_up(struct dpaa2_mac_priv *priv, unsigned int mode,
			      phy_interface_t interface, struct phy_device *phy)
{

	struct dpmac_link_state state;

	state = priv->old_state;
	state.up = 1;
	state.state_valid = 1;

	dpmac_set_link_state_v2(priv->mc_dev->mc_io, 0,
				priv->mc_dev->mc_handle, &state);

	printk(KERN_ERR "%s %d: id = %d\n", __func__, __LINE__, priv->attr.id);
}

static int dpaa2_mac_phylink_event(struct notifier_block *nb,
				   unsigned long event, void *ptr)
{
	struct dpaa2_mac_priv *priv = container_of(nb, struct dpaa2_mac_priv, phylink_nb);
	struct phylink_notifier_info *info = ptr;

	switch (event) {
	case PHYLINK_VALIDATE:
		dpaa2_mac_validate(priv, info->supported, info->state);
		break;
	case PHYLINK_MAC_LINK_STATE:
		dpaa2_mac_link_state(priv, info->state);
	case PHYLINK_MAC_AN_RESTART:
		dpaa2_mac_an_restart(priv);
		break;
	case PHYLINK_MAC_CONFIG:
		dpaa2_mac_config(priv, info->link_an_mode, info->state);
		break;
	case PHYLINK_MAC_LINK_DOWN:
		dpaa2_mac_link_down(priv, info->link_an_mode, info->interface);
		break;
	case PHYLINK_MAC_LINK_UP:
		dpaa2_mac_link_up(priv, info->link_an_mode, info->interface,
				  info->phydev);
		break;
	default:
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}

static int dpaa2_mac_probe(struct fsl_mc_device *mc_dev)
{
	struct device		*dev;
	struct dpaa2_mac_priv	*priv = NULL;
	struct device_node *dpmac_node;
	int			if_mode;
	int			err = 0;
	struct phylink *phylink;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	dev = &mc_dev->dev;
	priv->mc_dev = mc_dev;
	dev_set_drvdata(dev, priv);

	/* We may need to issue MC commands while in atomic context */
	err = fsl_mc_portal_allocate(mc_dev, FSL_MC_IO_ATOMIC_CONTEXT_PORTAL,
				     &mc_dev->mc_io);
	if (err || !mc_dev->mc_io) {
		dev_dbg(dev, "fsl_mc_portal_allocate error: %d\n", err);
		err = -EPROBE_DEFER;
		goto err_exit;
	}

	err = dpmac_open(mc_dev->mc_io, 0, mc_dev->obj_desc.id,
			 &mc_dev->mc_handle);
	if (err || !mc_dev->mc_handle) {
		dev_err(dev, "dpmac_open error: %d\n", err);
		err = -ENODEV;
		goto err_free_mcp;
	}

	err = dpmac_get_api_version(mc_dev->mc_io, 0, &priv->dpmac_ver_major,
				    &priv->dpmac_ver_minor);
	if (err) {
		dev_err(dev, "dpmac_get_api_version failed\n");
		goto err_version;
	}

	if (cmp_dpmac_ver(priv, DPMAC_VER_MAJOR, DPMAC_VER_MINOR) < 0) {
		dev_err(dev, "DPMAC version %u.%u lower than supported %u.%u\n",
			priv->dpmac_ver_major, priv->dpmac_ver_minor,
			DPMAC_VER_MAJOR, DPMAC_VER_MINOR);
		err = -ENOTSUPP;
		goto err_version;
	}

	err = dpmac_get_attributes(mc_dev->mc_io, 0,
				   mc_dev->mc_handle, &priv->attr);
	if (err) {
		dev_err(dev, "dpmac_get_attributes err %d\n", err);
		err = -EINVAL;
		goto err_close;
	}

	/* Look up the DPMAC node in the device-tree. */
	dpmac_node = find_dpmac_node(dev, priv->attr.id);
	if (!dpmac_node) {
		dev_err(dev, "No dpmac@%d subnode found.\n", priv->attr.id);
		err = -ENODEV;
		goto err_close;
	}

	err = setup_irqs(mc_dev);
	if (err) {
		err = -EFAULT;
		goto err_close;
	}

	/* get the interface mode from the dpmac of node or from the MC attributes */
	if_mode = of_get_phy_mode(dpmac_node);
	if (if_mode >= 0) {
		dev_dbg(dev, "\tusing if mode %s for eth_if %d\n",
			phy_modes(if_mode), priv->attr.eth_if);
		goto operation_mode;
	}

	if (priv->attr.eth_if < ARRAY_SIZE(dpaa2_mac_iface_mode)) {
		if_mode = dpaa2_mac_iface_mode[priv->attr.eth_if];
		dev_dbg(dev, "\tusing if mode %s for eth_if %d\n",
			phy_modes(if_mode), priv->attr.eth_if);
	} else {
		dev_err(dev, "Unexpected interface mode %d\n",
			priv->attr.eth_if);
		err = -EINVAL;
		goto err_no_if_mode;
	}

operation_mode:
	if (priv->attr.link_type == DPMAC_LINK_TYPE_FIXED)
		goto setup_done;

	priv->phylink_nb.notifier_call = dpaa2_mac_phylink_event;
	phylink = phylink_create_raw(&priv->phylink_nb,
				     of_fwnode_handle(dpmac_node), if_mode);
	if (IS_ERR(phylink)) {
		err = PTR_ERR(phylink);
		goto err_phylink_create;
	}
	priv->phylink = phylink;

	err = phylink_of_phy_connect(priv->phylink, dpmac_node, 0);
	if (err) {
		pr_err("phylink_of_phy_connect() = %d\n", err);
		goto err_phylink_connect;
	}

setup_done:

	return 0;

err_phylink_connect:
	/* TODO disconnect */
err_phylink_create:
err_no_if_mode:
	teardown_irqs(mc_dev);
err_version:
err_close:
	dpmac_close(mc_dev->mc_io, 0, mc_dev->mc_handle);
err_free_mcp:
	fsl_mc_portal_free(mc_dev->mc_io);
err_exit:
	return err;
}

static int dpaa2_mac_remove(struct fsl_mc_device *mc_dev)
{
	struct device		*dev = &mc_dev->dev;
	struct dpaa2_mac_priv	*priv = dev_get_drvdata(dev);

	phylink_stop(priv->phylink);
	teardown_irqs(priv->mc_dev);
	dpmac_close(priv->mc_dev->mc_io, 0, priv->mc_dev->mc_handle);
	fsl_mc_portal_free(priv->mc_dev->mc_io);

	kfree(priv);
	dev_set_drvdata(dev, NULL);

	return 0;
}

static const struct fsl_mc_device_id dpaa2_mac_match_id_table[] = {
	{
		.vendor = FSL_MC_VENDOR_FREESCALE,
		.obj_type = "dpmac",
	},
	{ .vendor = 0x0 }
};
MODULE_DEVICE_TABLE(fslmc, dpaa2_mac_match_id_table);

static struct fsl_mc_driver dpaa2_mac_drv = {
	.driver = {
		.name		= KBUILD_MODNAME,
		.owner		= THIS_MODULE,
	},
	.probe		= dpaa2_mac_probe,
	.remove		= dpaa2_mac_remove,
	.match_id_table = dpaa2_mac_match_id_table,
};

module_fsl_mc_driver(dpaa2_mac_drv);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DPAA2 PHY proxy interface driver");
