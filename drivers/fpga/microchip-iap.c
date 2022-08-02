// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip Polarfire{, SoC} "IAP" FPGA reprogramming.
 *
 * Copyright (c) 2022 Microchip Corporation. All rights reserved.
 *
 * Author: Conor Dooley <conor.dooley@microchip.com>
 *
 */

#include "linux/of_platform.h"
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/fpga/fpga-mgr.h>
#include <soc/microchip/mpfs.h>

#define DEFAULT_MBOX_OFFSET		0u
#define DEFAULT_RESP_OFFSET		0u

#define FEATURE_CMD_OPCODE		0x05
#define FEATURE_CMD_DATA_SIZE		0u
#define FEATURE_RESP_SIZE		33u //its 9 on mpf! 33 on mpfs
#define FEATURE_CMD_DATA		NULL
#define FEATURE_IAP_MASK		(BIT(5) | BIT(1))

#define IAP_VERIFY_CMD_OPCODE		0x22
#define IAP_VERIFY_CMD_DATA_SIZE	0u
#define IAP_VERIFY_RESP_SIZE		1u
#define IAP_VERIFY_CMD_DATA		NULL

#define IAP_PROGRAM_CMD_OPCODE		0x42
#define IAP_PROGRAM_CMD_DATA_SIZE	0u
#define IAP_PROGRAM_RESP_SIZE		1u
#define IAP_PROGRAM_CMD_DATA		NULL

#define IAP_IMAGE_INDEX			2u

struct mpf_iap_priv {
	struct mpfs_sys_controller *sys_controller;
	struct device *dev;
	struct fpga_region *region;
};

//Query Security Service Mailbox - > do i need to check a load of things?
static enum fpga_mgr_states mpf_iap_state(struct fpga_manager *mgr)
{
	struct mpf_iap_priv *priv = mgr->priv;
	u32 response_msg[FEATURE_RESP_SIZE];
	int ret;

	struct mpfs_mss_response response = {
		.resp_status = 0U,
		.resp_msg = response_msg,
		.resp_size = FEATURE_RESP_SIZE
	};

	struct mpfs_mss_msg msg = {
		.cmd_opcode = FEATURE_CMD_OPCODE,
		.cmd_data_size = FEATURE_CMD_DATA_SIZE,
		.response = &response,
		.cmd_data = FEATURE_CMD_DATA,
		.mbox_offset = DEFAULT_MBOX_OFFSET,
		.resp_offset = DEFAULT_RESP_OFFSET
	};

	ret = mpfs_blocking_transaction(priv->sys_controller, &msg);
	if (ret | response.resp_status)
		return FPGA_MGR_STATE_UNKNOWN;
	
	//need to check byte1, bit 5 to see if iap is possible
	if (!(response_msg[1] & FEATURE_IAP_MASK))
		return FPGA_MGR_STATE_OPERATING;

	return FPGA_MGR_STATE_WRITE_INIT_ERR; //iap is disabled
}

static int mpf_iap_write_init(struct fpga_manager *mgr, struct fpga_image_info *info,
					 const char *buf, size_t count)
{
	//check that we can actually write to the spi?
	return 0;
}

static int mpf_iap_write(struct fpga_manager *mgr, const char *buf, size_t count)
{
	struct mpf_iap_priv *priv = mgr->priv;
	struct spi_device *spi_dev = mpfs_sys_controller_get_flash(priv->sys_controller);
	if (!spi_dev)
		dev_warn(priv->dev, "No flash device found, running IAP anyway...\n");

	return !!spi_dev;
}

static int mpf_iap_write_complete(struct fpga_manager *mgr, struct fpga_image_info *info)
{
	struct mpf_iap_priv *priv = mgr->priv;
	int ret;

	u32 rc;

	struct mpfs_mss_response response = {
		.resp_status = 0U,
		.resp_msg = &rc,
		.resp_size = IAP_VERIFY_RESP_SIZE
	};

	struct mpfs_mss_msg msg = {
		.cmd_opcode = IAP_VERIFY_CMD_OPCODE,
		.cmd_data_size = IAP_VERIFY_CMD_DATA_SIZE,
		.response = &response,
		.cmd_data = IAP_VERIFY_CMD_DATA,
		.mbox_offset = IAP_IMAGE_INDEX,
		.resp_offset = DEFAULT_RESP_OFFSET
	};

	pr_info("ran IAP_VERIFY_RESP_SIZE\n");
	ret = mpfs_blocking_transaction(priv->sys_controller, &msg);
	if (ret | response.resp_status)
		return FPGA_MGR_STATE_WRITE_COMPLETE_ERR;

	//then write it
	struct mpfs_mss_response response2 = {
		.resp_status = 0U,
		.resp_msg = rc,
		.resp_size = IAP_PROGRAM_RESP_SIZE
	};

	struct mpfs_mss_msg msg2 = {
		.cmd_opcode = IAP_PROGRAM_CMD_OPCODE,
		.cmd_data_size = IAP_PROGRAM_CMD_DATA_SIZE,
		.response = &response2,
		.cmd_data = IAP_PROGRAM_CMD_DATA,
		.mbox_offset = IAP_IMAGE_INDEX,
		.resp_offset = DEFAULT_RESP_OFFSET
	};

	ret = mpfs_blocking_transaction(priv->sys_controller, &msg2);
	if (ret | response.resp_status)
		return FPGA_MGR_STATE_WRITE_COMPLETE_ERR;
	pr_info("ran IAP_PROGRAM_CMD_OPCODE\n");

	//i think we get force rebooted here?
	return FPGA_MGR_STATE_WRITE_COMPLETE_ERR;
}

static const struct fpga_manager_ops mpf_iap_ops = {
	.state = mpf_iap_state,
	.write_init = mpf_iap_write_init,
	.write = mpf_iap_write,
	.write_complete = mpf_iap_write_complete,
};

static int test_it(struct device *dev)
{
	struct fpga_manager *mgr;
	struct fpga_image_info *info;
	int ret;

	printk("starting to test the fpga manager\n");

	mgr = fpga_mgr_get(dev);
	info = fpga_image_info_alloc(dev);

	info->firmware_name = devm_kstrdup(dev, "pf_bitstream.fw", GFP_KERNEL);
	/* Get exclusive control of FPGA manager */
	ret = fpga_mgr_lock(mgr);
	if (ret) {
		dev_err(dev, "couldnt lock the manager\n");
		return ret;
	}

	ret = fpga_mgr_load(mgr, info);
	if (ret) {
		dev_err(dev, "couldnt load the firmware\n");
		return ret;
	}

	/* Release the FPGA manager */
	fpga_mgr_unlock(mgr);
	fpga_mgr_put(mgr);

	/* Deallocate the image info if you're done with it */
	fpga_image_info_free(info);

	printk("test complete\n");

	return ret;
}

static int mpf_iap_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct platform_device *flash;
	struct mpf_iap_priv *priv;
	struct fpga_manager *mgr;
	struct device_node *np;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	
	priv->sys_controller = mpfs_sys_controller_get(dev);
	if (IS_ERR(priv->sys_controller))
		return dev_err_probe(dev, PTR_ERR(priv->sys_controller),
				     "Could not register as a sub device of the system controller\n");

	priv->dev = dev;

	platform_set_drvdata(pdev, priv);

	mgr = devm_fpga_mgr_register(dev, "Microchip MPF(S) IAP FPGA Manager",
				     &mpf_iap_ops, priv);
	if (IS_ERR(mgr))
		return dev_err_probe(dev, PTR_ERR(mgr),
				     "Could not register FPGA manager.\n");

	enum fpga_mgr_states state = mpf_iap_state(mgr);
	ret = test_it(dev);
	if (ret)
		dev_err_probe(dev, ret, "foo");
	
	dev_info(dev, "Registered Microchip MPF(S) IAP FPGA Manager %u\n", state);

	return 0;
}

static const struct of_device_id mpf_iap_of_match[] = {
	{ .compatible = "microchip,mpf-iap" },
	{ .compatible = "microchip,mpfs-iap" },
	{}
};
MODULE_DEVICE_TABLE(of, mpf_iap_of_ids);

static struct platform_driver mpf_iap_driver = {
	.driver = {
		.name = "mpfs-iap",
		.of_match_table = mpf_iap_of_match,
	},
	.probe = mpf_iap_probe,
};
module_platform_driver(mpf_iap_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Conor Dooley <conor.dooley@microchip.com>");
MODULE_DESCRIPTION("PolarFire{, SoC} IAP FPGA reprogramming");