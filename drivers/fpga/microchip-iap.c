// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip Polarfire{, SoC} "IAP" FPGA reprogramming.
 *
 * Copyright (c) 2022 Microchip Corporation. All rights reserved.
 *
 * Author: Conor Dooley <conor.dooley@microchip.com>
 */
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/fpga/fpga-mgr.h>
#include <soc/microchip/mpfs.h>

#define IAP_DEFAULT_MBOX_OFFSET		0u
#define IAP_DEFAULT_RESP_OFFSET		0u

#define IAP_FEATURE_CMD_OPCODE		0x05
#define IAP_FEATURE_CMD_DATA_SIZE	0u
#define MPFS_FEATURE_RESP_SIZE		33u
#define MPF_FEATURE_RESP_SIZE		9u
#define IAP_FEATURE_CMD_DATA		NULL
#define IAP_FEATURE_ENABLED		BIT(5)

#define IAP_VERIFY_CMD_OPCODE		0x22
#define IAP_VERIFY_CMD_DATA_SIZE	0u
#define IAP_VERIFY_RESP_SIZE		1u
#define IAP_VERIFY_CMD_DATA		NULL

#define IAP_PROGRAM_CMD_OPCODE		0x42
#define IAP_PROGRAM_CMD_DATA_SIZE	0u
#define IAP_PROGRAM_RESP_SIZE		1u
#define IAP_PROGRAM_CMD_DATA		NULL

#define IAP_IMAGE_INDEX			2u

struct mpf_iap_config {
	u8 feature_response_size;
};

struct mpf_iap_priv {
	struct mpfs_sys_controller *sys_controller;
	struct device *dev;
	struct fpga_region *region;
	struct mpf_iap_config *config;
};

static enum fpga_mgr_states mpf_iap_state(struct fpga_manager *mgr)
{
	struct mpf_iap_priv *priv = mgr->priv;
	struct mpfs_mss_response *response;
	struct mpfs_mss_msg *message;
	u32 *response_msg;
	int ret;
	enum fpga_mgr_states rc = FPGA_MGR_STATE_WRITE_INIT_ERR;

	response_msg = devm_kzalloc(priv->dev,
				    priv->config->feature_response_size * sizeof(response_msg),
				    GFP_KERNEL);
	if (!response_msg)
		return FPGA_MGR_STATE_WRITE_INIT_ERR;

	response = devm_kzalloc(priv->dev, sizeof(struct mpfs_mss_response), GFP_KERNEL);
	if (!response) {
		devm_kfree(priv->dev, response_msg);
		return FPGA_MGR_STATE_WRITE_INIT_ERR;
	}

	message = devm_kzalloc(priv->dev, sizeof(struct mpfs_mss_msg), GFP_KERNEL);
	if (!response) {
		devm_kfree(priv->dev, response_msg);
		devm_kfree(priv->dev, response);
		return FPGA_MGR_STATE_WRITE_INIT_ERR;
	}

	/*
	 * To verify that IAP is possible, the "Query Security Service Request"
	 * is performed. Bit 5 of byte 1 is "UL_IAP" & if it is set, IAP is not
	 * possible.
	 * This service has no command data & does not overload mbox_offset.
	 * The size of the response varies between PolarFire & PolarFire SoC.
	 */
	response->resp_msg = response_msg;
	response->resp_size = priv->config->feature_response_size;
	message->cmd_opcode = IAP_FEATURE_CMD_OPCODE;
	message->cmd_data_size = IAP_FEATURE_CMD_DATA_SIZE;
	message->response = response;
	message->cmd_data = IAP_FEATURE_CMD_DATA;
	message->mbox_offset = IAP_DEFAULT_MBOX_OFFSET;
	message->resp_offset = IAP_DEFAULT_RESP_OFFSET;

	ret = mpfs_blocking_transaction(priv->sys_controller, message);
	if (ret | response->resp_status) {
		rc = FPGA_MGR_STATE_UNKNOWN;
		goto out;
	}
	
	if (!(response_msg[1] & IAP_FEATURE_ENABLED))
		rc = FPGA_MGR_STATE_OPERATING;

out:
	devm_kfree(priv->dev, response_msg);
	devm_kfree(priv->dev, response);
	devm_kfree(priv->dev, message);

	return rc;
}

static int mpf_iap_write_init(struct fpga_manager *mgr, struct fpga_image_info *info,
					 const char *buf, size_t count)
{
	/*
	 * No parsing etc of the bitstream is required. The system controller
	 * will do all of that itself - including verifying that the bitstream
	 * is valid.
	 */
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
	struct mpfs_mss_response *response;
	struct mpfs_mss_msg *message;
	u32 *response_msg;
	int ret = 0;

	response_msg = devm_kzalloc(priv->dev,
				    priv->config->feature_response_size * sizeof(response_msg),
				    GFP_KERNEL);
	if (!response_msg)
		return -ENOMEM;

	response = devm_kzalloc(priv->dev, sizeof(struct mpfs_mss_response), GFP_KERNEL);
	if (!response) {
		devm_kfree(priv->dev, response_msg);
		return -ENOMEM;
	}

	message = devm_kzalloc(priv->dev, sizeof(struct mpfs_mss_msg), GFP_KERNEL);
	if (!response) {
		devm_kfree(priv->dev, response_msg);
		devm_kfree(priv->dev, response);
		return -ENOMEM;
	}

	/*
	 * The system controller can verify that an image in the flash is valid.
	 * Rather than duplicate the check in this driver, call the relevant
	 * service from the system controller instead.
	 * This service has no command data and no response data. It overloads
	 * mbox_offset with the image index in the flash's SPI directory where
	 * the bitstream is located.
	 */
	response->resp_msg = response_msg;
	response->resp_size = IAP_VERIFY_RESP_SIZE;
	message->cmd_opcode = IAP_VERIFY_CMD_OPCODE;
	message->cmd_data_size = IAP_VERIFY_CMD_DATA_SIZE;
	message->response = response;
	message->cmd_data = IAP_VERIFY_CMD_DATA;
	message->mbox_offset = IAP_IMAGE_INDEX;
	message->resp_offset = IAP_DEFAULT_RESP_OFFSET;

	pr_info("ran IAP_VERIFY_RESP_SIZE\n");
	ret = mpfs_blocking_transaction(priv->sys_controller, message);
	if (ret | response->resp_status) {
		ret = ret ? ret : -EBADMSG;
		goto out;
	}

	/*
	 * If the validation has passed, initiate IAP.
	 * This service has no command data and no response data. It overloads
	 * mbox_offset with the image index in the flash's SPI directory where
	 * the bitstream is located.
	 * Once we attempt IAP either:
	 * - it passes and the board reboots
	 * - it fails and the board reboots to recover
	 * - the system controller aborts and we exit "gracefully"
	 * This function will never return 0.
	 */
	response->resp_msg = response_msg;
	response->resp_size = IAP_PROGRAM_RESP_SIZE;
	message->cmd_opcode = IAP_PROGRAM_CMD_OPCODE;
	message->cmd_data_size = IAP_PROGRAM_CMD_DATA_SIZE;
	message->response = response;
	message->cmd_data = IAP_PROGRAM_CMD_DATA;
	message->mbox_offset = IAP_IMAGE_INDEX;
	message->resp_offset = IAP_DEFAULT_RESP_OFFSET;

	pr_info("ran IAP_PROGRAM_CMD_OPCODE\n");
	ret = mpfs_blocking_transaction(priv->sys_controller, message);
	if (ret)
		goto out;

	/*
	 * This return 0 is dead code. Either the IAP will fail, or it will pass
	 * & the FPGA will be rebooted in which case mpfs_blocking_transaction()
	 * will never return and Linux will die.
	 */
	return 0;

out:
	devm_kfree(priv->dev, response_msg);
	devm_kfree(priv->dev, response);
	devm_kfree(priv->dev, message);
	return ret;
}

static const struct fpga_manager_ops mpf_iap_ops = {
	.state = mpf_iap_state,
	.write_init = mpf_iap_write_init,
	.write = mpf_iap_write,
	.write_complete = mpf_iap_write_complete,
};

static int mpf_iap_run(struct device *dev)
{
	struct fpga_manager *mgr;
	struct fpga_image_info *info;
	int ret;

	printk("starting to test the fpga manager\n");

	mgr = fpga_mgr_get(dev);
	info = fpga_image_info_alloc(dev);

	info->firmware_name = devm_kstrdup(dev, "pf_bitstream.fw", GFP_KERNEL);
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

	fpga_mgr_unlock(mgr);
	fpga_mgr_put(mgr);
	fpga_image_info_free(info);

	dev_info(dev, "test complete\n");

	return ret;
}

static const struct mpf_iap_config mpfs_config = {
	.feature_response_size = MPFS_FEATURE_RESP_SIZE,
};

static const struct mpf_iap_config mpf_config = {
	.feature_response_size = MPF_FEATURE_RESP_SIZE,
};

static const struct of_device_id mpf_iap_of_match[] = {
	{ .compatible = "microchip,mpf-iap", .data = &mpf_config},
	{ .compatible = "microchip,mpfs-iap", .data = &mpfs_config},
	{}
};
MODULE_DEVICE_TABLE(of, mpf_iap_of_match);

static int mpf_iap_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct platform_device *flash;
	struct mpf_iap_priv *priv;
	struct fpga_manager *mgr;
	struct device_node *np;
	const struct of_device_id *of_id;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	
	priv->sys_controller = mpfs_sys_controller_get(dev);
	if (IS_ERR(priv->sys_controller))
		return dev_err_probe(dev, PTR_ERR(priv->sys_controller),
				     "Could not register as a sub device of the system controller\n");

	priv->dev = dev;

	of_id = of_match_node(mpf_iap_of_match, dev->of_node);
	if (!of_id)
		return -ENODEV;

	priv->config = (struct mpf_iap_config *)of_id->data;

	platform_set_drvdata(pdev, priv);

	mgr = devm_fpga_mgr_register(dev, "Microchip MPF(S) IAP FPGA Manager",
				     &mpf_iap_ops, priv);
	if (IS_ERR(mgr))
		return dev_err_probe(dev, PTR_ERR(mgr),
				     "Could not register FPGA manager.\n");

	enum fpga_mgr_states state = mpf_iap_state(mgr);
	ret = mpf_iap_run(dev);
	if (ret)
		dev_err_probe(dev, ret, "IAP failed");
	
	dev_info(dev, "Registered Microchip MPF(S) IAP FPGA Manager %u\n", state);

	return 0;
}

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