// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip Polarfire SoC "Auto Update" FPGA reprogramming.
 *
 * Copyright (c) 2022 Microchip Corporation. All rights reserved.
 *
 * Author: Conor Dooley <conor.dooley@microchip.com>
 */
#include <linux/debugfs.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/of_device.h>

#include <soc/microchip/mpfs.h>

#define AUTO_UPDATE_DEFAULT_MBOX_OFFSET		0u
#define AUTO_UPDATE_DEFAULT_RESP_OFFSET		0u

#define AUTO_UPDATE_FEATURE_CMD_OPCODE		0x05u
#define AUTO_UPDATE_FEATURE_CMD_DATA_SIZE	0u
#define AUTO_UPDATE_FEATURE_RESP_SIZE		33u
#define AUTO_UPDATE_FEATURE_CMD_DATA		NULL
#define AUTO_UPDATE_FEATURE_ENABLED		BIT(5)

#define AUTO_UPDATE_AUTHENTICATE_CMD_OPCODE	0x22u
#define AUTO_UPDATE_AUTHENTICATE_CMD_DATA_SIZE	0u
#define AUTO_UPDATE_AUTHENTICATE_RESP_SIZE	1u
#define AUTO_UPDATE_AUTHENTICATE_CMD_DATA	NULL

#define AUTO_UPDATE_PROGRAM_CMD_OPCODE		0x46u
#define AUTO_UPDATE_PROGRAM_CMD_DATA_SIZE	0u
#define AUTO_UPDATE_PROGRAM_RESP_SIZE		1u
#define AUTO_UPDATE_PROGRAM_CMD_DATA		NULL

#define AUTO_UPDATE_DIRECTORY_SIZE		0x400u
#define AUTO_UPDATE_DIRECTORY_WIDTH		4u
#define AUTO_UPDATE_GOLDEN_INDEX		0u
//#define AUTO_UPDATE_UPGRADE_INDEX		1u
#define AUTO_UPDATE_UPGRADE_INDEX		0u
#define AUTO_UPDATE_GOLDEN_DIRECTORY		0u
#define AUTO_UPDATE_UPGRADE_DIRECTORY		(AUTO_UPDATE_DIRECTORY_WIDTH * AUTO_UPDATE_UPGRADE_INDEX)
#define AUTO_UPDATE_ADDRESS_OFFSET		0xA00000u

struct mpfs_auto_update_config {
	u8 feature_response_size;
};

struct mpfs_auto_update_priv {
	struct mpfs_sys_controller *sys_controller;
	struct device *dev;
	struct fpga_region *region;
	struct mpfs_auto_update_config *config;
	struct mtd_info *flash;
	struct dentry *debugfs_dir;
	u32 image_address;
};

static struct device *mpfs_auto_update_debug_dev;

static enum fpga_mgr_states mpfs_auto_update_state(struct fpga_manager *mgr)
{
	struct mpfs_auto_update_priv *priv = mgr->priv;
	struct mpfs_mss_response *response;
	struct mpfs_mss_msg *message;
	u32 *response_msg;
	int ret;
	enum fpga_mgr_states rc = FPGA_MGR_STATE_WRITE_INIT_ERR;

	response_msg = devm_kzalloc(priv->dev,
				    AUTO_UPDATE_FEATURE_RESP_SIZE * sizeof(response_msg),
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
	 * To verify that Auto Update is possible, the "Query Security Service Request"
	 * is performed. Bit 5 of byte 1 is "UL_Auto Update" & if it is set, Auto Update is not
	 * possible.
	 * This service has no command data & does not overload mbox_offset.
	 * The size of the response varies between PolarFire & PolarFire SoC.
	 */
	response->resp_msg = response_msg;
	response->resp_size = AUTO_UPDATE_FEATURE_RESP_SIZE;
	message->cmd_opcode = AUTO_UPDATE_FEATURE_CMD_OPCODE;
	message->cmd_data_size = AUTO_UPDATE_FEATURE_CMD_DATA_SIZE;
	message->response = response;
	message->cmd_data = AUTO_UPDATE_FEATURE_CMD_DATA;
	message->mbox_offset = AUTO_UPDATE_DEFAULT_MBOX_OFFSET;
	message->resp_offset = AUTO_UPDATE_DEFAULT_RESP_OFFSET;

	ret = mpfs_blocking_transaction(priv->sys_controller, message);
	if (ret | response->resp_status) {
		rc = FPGA_MGR_STATE_UNKNOWN;
		goto out;
	}

	if (!(response_msg[1] & AUTO_UPDATE_FEATURE_ENABLED))
		rc = FPGA_MGR_STATE_OPERATING;

out:
	devm_kfree(priv->dev, response_msg);
	devm_kfree(priv->dev, response);
	devm_kfree(priv->dev, message);

	return rc;
}

static int mpfs_auto_update_write_init(struct fpga_manager *mgr, struct fpga_image_info *info,
			      const char *buf, size_t count)
{
	/*
	 * Verifying the Golden Image is idealistic. It will be evaluated
	 * against the currently programmed image and thus fail - due to either
	 * rollback protection (if its an older version than that in use) or
	 * or if the version is the same as that of the in-use image.
	 */
	return 0;

//	struct mpfs_auto_update_priv *priv = mgr->priv;
//	struct mpfs_mss_response *response;
//	struct mpfs_mss_msg *message;
//	u32 *response_msg;
//	int ret = 0;
//
//	response_msg = devm_kzalloc(priv->dev,
//				    AUTO_UPDATE_FEATURE_RESP_SIZE * sizeof(response_msg),
//				    GFP_KERNEL);
//	if (!response_msg)
//		return -ENOMEM;
//
//	response = devm_kzalloc(priv->dev, sizeof(struct mpfs_mss_response), GFP_KERNEL);
//	if (!response) {
//		devm_kfree(priv->dev, response_msg);
//		return -ENOMEM;
//	}
//
//	message = devm_kzalloc(priv->dev, sizeof(struct mpfs_mss_msg), GFP_KERNEL);
//	if (!response) {
//		devm_kfree(priv->dev, response_msg);
//		devm_kfree(priv->dev, response);
//		return -ENOMEM;
//	}
//
//	/*
//	 * The system controller can verify that an image in the flash is valid.
//	 * Rather than duplicate the check in this driver, call the relevant
//	 * service from the system controller instead.
//	 * This service has no command data and no response data. It overloads
//	 * mbox_offset with the image index in the flash's SPI directory where
//	 * the bitstream is located.
//	 * If the Golden Image is not valid, do not allow an auto upgrade.
//	 */
//	response->resp_msg = response_msg;
//	response->resp_size = AUTO_UPDATE_AUTHENTICATE_RESP_SIZE;
//	message->cmd_opcode = AUTO_UPDATE_AUTHENTICATE_CMD_OPCODE;
//	message->cmd_data_size = AUTO_UPDATE_AUTHENTICATE_CMD_DATA_SIZE;
//	message->response = response;
//	message->cmd_data = AUTO_UPDATE_AUTHENTICATE_CMD_DATA;
//	message->mbox_offset = AUTO_UPDATE_GOLDEN_INDEX;
//	message->resp_offset = AUTO_UPDATE_DEFAULT_RESP_OFFSET;
//
//	dev_info(priv->dev, "Verifying golden image\n");
//	ret = mpfs_blocking_transaction(priv->sys_controller, message);
//	if (response->resp_status)
//		ret = -EBADMSG;
//
//	devm_kfree(priv->dev, response_msg);
//	devm_kfree(priv->dev, response);
//	devm_kfree(priv->dev, message);
//
//	return ret;
}

static int mpfs_auto_update_write(struct fpga_manager *mgr, const char *buf, size_t count)
{
	/*
	 * No parsing etc of the bitstream is required. The system controller
	 * will do all of that itself - including verifying that the bitstream
	 * is valid.
	 */
	struct mpfs_auto_update_priv *priv = mgr->priv;
	struct erase_info erase;
	char *buffer;
	size_t bytes_written = 0, bytes_read = 0;
	loff_t directory_address = AUTO_UPDATE_UPGRADE_DIRECTORY;
	u64 erase_size = AUTO_UPDATE_DIRECTORY_SIZE;
	int ret;

	priv->flash = mpfs_sys_controller_get_flash(priv->sys_controller);
	if (!priv->flash)
		return -EIO;

	erase_size = round_up(erase_size, (u64)priv->flash->erasesize);

	buffer = devm_kzalloc(priv->dev, erase_size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	erase.addr = 0x0;
	erase.len = erase_size;

	/*
	 * We need to write the "SPI DIRECTORY" to the first 1 KiB, telling
	 * the system controller where to find the actual bitstream. Since
	 * this is spi-nor, we have to read the first eraseblock, erase that
	 * portion of the flash, modify the data and then write it back.
	 */
	ret = mtd_read(priv->flash, 0x0, erase_size, &bytes_read, (u_char *) buffer);
	if (ret)
		goto out;

	if (bytes_read != erase_size) {
		ret = -EIO;
		goto out;
	}

	ret = mtd_erase(priv->flash, &erase);
	if (ret)
		goto out;

	/* populate the image address */
	memcpy(buffer + AUTO_UPDATE_UPGRADE_DIRECTORY, &priv->image_address, AUTO_UPDATE_DIRECTORY_WIDTH);

	dev_info(priv->dev, "Writing the image address (%x) to the flash directory (%x)\n", priv->image_address, directory_address);

	ret = mtd_write(priv->flash, 0x0, erase_size, &bytes_written, (u_char *)buffer);
	if (ret)
		goto out;

	if (bytes_written != erase_size) {
		ret = -EIO;
		goto out;
	}

	/*
	 * Now the .spi image itself can be written to the flash. Preservation
	 * of contents here is not important here, unlike the spi "directory"
	 * which must be RMWed.
	 */
	dev_info(priv->dev, "Writing the image to the flash at address (%x)\n", priv->image_address);
	erase.len = round_up(count, (size_t)priv->flash->erasesize);
	erase.addr = AUTO_UPDATE_ADDRESS_OFFSET;

	ret = mtd_erase(priv->flash, &erase);
	if (ret)
		goto out;

	ret = mtd_write(priv->flash, (loff_t) priv->image_address, count, &bytes_written, buf);
	if (ret)
		goto out;

	if (bytes_written != count)
		return -EIO;

out:
	devm_kfree(priv->dev, buffer);
	return ret;
}

static int mpfs_auto_update_write_complete(struct fpga_manager *mgr, struct fpga_image_info *info)
{
	struct mpfs_auto_update_priv *priv = mgr->priv;
	struct mpfs_mss_response *response;
	struct mpfs_mss_msg *message;
	u32 *response_msg;
	int ret = 0;

	response_msg = devm_kzalloc(priv->dev,
				    AUTO_UPDATE_FEATURE_RESP_SIZE * sizeof(response_msg),
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
	response->resp_size = AUTO_UPDATE_AUTHENTICATE_RESP_SIZE;
	message->cmd_opcode = AUTO_UPDATE_AUTHENTICATE_CMD_OPCODE;
	message->cmd_data_size = AUTO_UPDATE_AUTHENTICATE_CMD_DATA_SIZE;
	message->response = response;
	message->cmd_data = AUTO_UPDATE_AUTHENTICATE_CMD_DATA;
	message->mbox_offset = AUTO_UPDATE_UPGRADE_INDEX;
	message->resp_offset = AUTO_UPDATE_DEFAULT_RESP_OFFSET;

	dev_info(priv->dev, "Running verification of upgrade image\n");
	ret = mpfs_blocking_transaction(priv->sys_controller, message);
	if (ret | response->resp_status) {
		ret = ret ? ret : -EBADMSG;
		goto out;
	}

	/*
	 * If the validation has passed, initiate Auto Update.
	 * This service has no command data and no response data. It overloads
	 * mbox_offset with the image index in the flash's SPI directory where
	 * the bitstream is located.
	 * Once we attempt Auto Update either:
	 * - it passes and the board reboots
	 * - it fails and the board reboots to recover
	 * - the system controller aborts and we exit "gracefully".
	 *   "gracefully" since there is no interrupt produced & it just times
	 *   out.
	 */
	response->resp_msg = response_msg;
	response->resp_size = AUTO_UPDATE_PROGRAM_RESP_SIZE;
	message->cmd_opcode = AUTO_UPDATE_PROGRAM_CMD_OPCODE;
	message->cmd_data_size = AUTO_UPDATE_PROGRAM_CMD_DATA_SIZE;
	message->response = response;
	message->cmd_data = AUTO_UPDATE_PROGRAM_CMD_DATA;
	message->mbox_offset = 0; //field is ignored
	message->resp_offset = AUTO_UPDATE_DEFAULT_RESP_OFFSET;

	dev_info(priv->dev, "Running Auto Update command\n");
	ret = mpfs_blocking_transaction(priv->sys_controller, message);
	if (ret && ret != -ETIMEDOUT)
		goto out;

	/* *remove this for auto update*
	 * This return 0 is dead code. Either the Auto Update will fail, or it will pass
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

static const struct fpga_manager_ops mpfs_auto_update_ops = {
	.state = mpfs_auto_update_state,
	.write_init = mpfs_auto_update_write_init,
	.write = mpfs_auto_update_write,
	.write_complete = mpfs_auto_update_write_complete,
};

static int mpfs_auto_update_run(struct device *dev)
{
	struct fpga_manager *mgr;
	struct fpga_image_info *info;
	int ret;

	printk("starting to test the fpga manager\n");

	mgr = fpga_mgr_get(dev);
	info = fpga_image_info_alloc(dev);

	info->firmware_name = devm_kstrdup(dev, "B_V_F_BASE_DESIGN.spi", GFP_KERNEL);

	ret = fpga_mgr_lock(mgr);
	if (ret) {
		dev_err(dev, "couldnt lock the manager\n");
		goto free_info;
	}

	ret = fpga_mgr_load(mgr, info);
	if (ret) {
		dev_err(dev, "couldnt load the firmware\n");
		goto unlock_mgr;
	}

	dev_info(dev, "test complete\n");

unlock_mgr:
	fpga_mgr_unlock(mgr);
free_info:
	fpga_image_info_free(info);
	fpga_mgr_put(mgr);

	return ret;
}

static ssize_t mpfs_auto_update_exec(struct file *file,
				     const char __user *data,
				     size_t count, loff_t *ppos)
{
	int ret;

	ret = mpfs_auto_update_run(mpfs_auto_update_debug_dev);
	if (ret)
		dev_err_probe(mpfs_auto_update_debug_dev, ret, "Auto Update failed");

	return ret;
}

static const struct file_operations mpfs_auto_update_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = mpfs_auto_update_exec
};

static int mpfs_auto_update_debugfs_setup(struct mpfs_auto_update_priv *priv)
{
	priv->debugfs_dir = debugfs_create_dir("fpga", NULL);

	if(IS_ERR(priv->debugfs_dir))
		return PTR_ERR(priv->debugfs_dir);

	debugfs_create_file("microchip_exec_update", 0200,
			    priv->debugfs_dir, NULL, &mpfs_auto_update_fops);

	return 0;
}

static int mpfs_auto_update_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mpfs_auto_update_priv *priv;
	struct fpga_manager *mgr;
	enum fpga_mgr_states state;
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

	mgr = devm_fpga_mgr_register(dev, "Microchip MPFS Auto Update FPGA Manager",
				     &mpfs_auto_update_ops, priv);
	if (IS_ERR(mgr))
		return dev_err_probe(dev, PTR_ERR(mgr),
				     "Could not register FPGA manager.\n");

	state = mpfs_auto_update_state(mgr);

	ret = mpfs_auto_update_debugfs_setup(priv);
	if (ret && ret != -ENODEV)
		return ret;

	mpfs_auto_update_debug_dev = priv->dev;

	priv->image_address = AUTO_UPDATE_ADDRESS_OFFSET;

	dev_info(dev, "Registered Microchip MPFS Auto Update FPGA Manager %u\n", state);

	return 0;
}

static struct platform_driver mpfs_auto_update_driver = {
	.driver = {
		.name = "mpfs-auto-update",
	},
	.probe = mpfs_auto_update_probe,
};
module_platform_driver(mpfs_auto_update_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Conor Dooley <conor.dooley@microchip.com>");
MODULE_DESCRIPTION("PolarFire SoC Auto Update FPGA reprogramming");
