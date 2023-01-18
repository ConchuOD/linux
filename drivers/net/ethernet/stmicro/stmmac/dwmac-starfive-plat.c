// SPDX-License-Identifier: GPL-2.0+
/*
 * StarFive DWMAC platform driver
 *
 * Copyright(C) 2022 StarFive Technology Co., Ltd.
 *
 */

#include <linux/of_device.h>

#include "stmmac_platform.h"

struct starfive_dwmac {
	struct device *dev;
	struct clk *clk_tx;
	struct clk *clk_gtx;
	struct clk *clk_gtxc;
};

static void starfive_eth_plat_fix_mac_speed(void *priv, unsigned int speed)
{
	struct starfive_dwmac *dwmac = priv;
	unsigned long rate;
	int err;

	rate = clk_get_rate(dwmac->clk_gtx);

	switch (speed) {
	case SPEED_1000:
		rate = 125000000;
		break;
	case SPEED_100:
		rate = 25000000;
		break;
	case SPEED_10:
		rate = 2500000;
		break;
	default:
		dev_err(dwmac->dev, "invalid speed %u\n", speed);
		break;
	}

	err = clk_set_rate(dwmac->clk_gtx, rate);
	if (err)
		dev_err(dwmac->dev, "failed to set tx rate %lu\n", rate);
}

static int starfive_eth_plat_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct starfive_dwmac *dwmac;
	int err;

	err = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (err)
		return err;

	plat_dat = stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat)) {
		dev_err(&pdev->dev, "dt configuration failed\n");
		return PTR_ERR(plat_dat);
	}

	dwmac = devm_kzalloc(&pdev->dev, sizeof(*dwmac), GFP_KERNEL);
	if (!dwmac)
		return -ENOMEM;

	dwmac->clk_tx = devm_clk_get_enabled(&pdev->dev, "tx");
	if (IS_ERR(dwmac->clk_tx))
		return dev_err_probe(&pdev->dev, PTR_ERR(dwmac->clk_tx),
						"error getting tx clock\n");

	dwmac->clk_gtx = devm_clk_get_enabled(&pdev->dev, "gtx");
	if (IS_ERR(dwmac->clk_gtx))
		return dev_err_probe(&pdev->dev, PTR_ERR(dwmac->clk_gtx),
						"error getting gtx clock\n");

	dwmac->clk_gtxc = devm_clk_get_enabled(&pdev->dev, "gtxc");
	if (IS_ERR(dwmac->clk_gtxc))
		return dev_err_probe(&pdev->dev, PTR_ERR(dwmac->clk_gtxc),
						"error getting gtxc clock\n");

	dwmac->dev = &pdev->dev;
	plat_dat->fix_mac_speed = starfive_eth_plat_fix_mac_speed;
	plat_dat->init = NULL;
	plat_dat->bsp_priv = dwmac;
	plat_dat->dma_cfg->dche = true;

	err = stmmac_dvr_probe(&pdev->dev, plat_dat, &stmmac_res);
	if (err) {
		stmmac_remove_config_dt(pdev, plat_dat);
		return err;
	}

	return 0;
}

static const struct of_device_id starfive_eth_plat_match[] = {
	{ .compatible = "starfive,jh7110-dwmac"	},
	{ }
};

static struct platform_driver starfive_eth_plat_driver = {
	.probe  = starfive_eth_plat_probe,
	.remove = stmmac_pltfr_remove,
	.driver = {
		.name = "starfive-eth-plat",
		.pm = &stmmac_pltfr_pm_ops,
		.of_match_table = starfive_eth_plat_match,
	},
};

module_platform_driver(starfive_eth_plat_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("StarFive DWMAC platform driver");
MODULE_AUTHOR("Yanhong Wang <yanhong.wang@starfivetech.com>");
