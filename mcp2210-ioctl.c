/*
 * ioctl interface for MCP2210 driver
 *
 * Copyright (c) 2013-2017 Daniel Santos <daniel.santos@pobox.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifdef CONFIG_MCP2210_IOCTL

#include <linux/uaccess.h>
#include <linux/completion.h>

#include "mcp2210.h"
#include "mcp2210-debug.h"

static long mcp2210_ioctl_eeprom(struct mcp2210_device *dev, struct ioctl_result *result);
static long mcp2210_ioctl_config_get(struct mcp2210_device *dev, struct ioctl_result *result);

static struct ioctl_cmds {
	long (*func)(struct mcp2210_device *dev, struct ioctl_result *result);
	u32 min_size;
} ioctl_cmds [MCP2210_IOCTL_MAX] = {
	{mcp2210_ioctl_cmd,	   IOCTL_DATA_CMD_SIZE,		},
	{mcp2210_ioctl_eeprom,	   IOCTL_DATA_EEPROM_SIZE + 1,	},
	{mcp2210_ioctl_config_get, IOCTL_DATA_CONFIG_SIZE,	},
	{mcp2210_ioctl_config_set, IOCTL_DATA_CONFIG_SIZE,	},
};

long mcp2210_ioctl(struct file *file, unsigned int request, unsigned long arg)
{
	struct mcp2210_device *dev = file->private_data;
	enum mcp2210_ioctl_cmd ioctl_cmd = _IOC_NR(request);
	int is_rw = _IOC_DIR(request) & _IOC_WRITE;
	__user struct mcp2210_ioctl_data *user_data = (void __user *)arg;
	u32 struct_size;
	size_t result_size;
	struct ioctl_result *result;
	int ret;

	mcp2210_debug("request: 0x%08x, io_dir: %d, is_rw: %d, ioctl_cmd: %u, magic: 0x%02x\n",
		      request,  _IOC_DIR(request), is_rw, ioctl_cmd, _IOC_TYPE(request));

	BUG_ON(!dev);

	if (_IOC_TYPE(request) != MCP2210_IOCTL_MAGIC)
		return -ENOTTY;

	/* we don't have any requests that do not read data */
	if (!(_IOC_DIR(request) & (_IOC_READ | _IOC_WRITE)))
		return -ENOTTY;

	if (ioctl_cmd < 0 || ioctl_cmd >= MCP2210_IOCTL_MAX) {
		mcp2210_warn("Invalid ioctl command number %d", ioctl_cmd);
		return -ENOTTY;
	}

	if ((ret = get_user(struct_size, &user_data->struct_size)))
		return -EFAULT;

	//mcp2210_debug("struct_size = %u, ioctl_cmds[ioctl_cmd].min_size = %u", struct_size, ioctl_cmds[ioctl_cmd].min_size);

	/* minimum valid size */
	if (unlikely(struct_size < ioctl_cmds[ioctl_cmd].min_size)) {

		return -EOVERFLOW;
	}

	/* max size */
	if (unlikely(struct_size > 0x4000)) {
		mcp2210_warn("request too large: %u", struct_size);
		return -EINVAL;
	}

	result_size = sizeof(struct ioctl_result) + struct_size;
	if (!(result = kzalloc(result_size, GFP_KERNEL))) {
		mcp2210_err("Failled to allocate %u bytes\n", (uint)result_size);
		return -ENOMEM;
	}

	init_completion(&result->completion);
	result->ioctl_cmd = ioctl_cmd;
	result->user_data = user_data;
	if ((ret = copy_from_user(result->payload, user_data, struct_size))) {
		ret = -EFAULT;
		goto exit_free;
	}

	/* just a sanity check */
	BUG_ON(result->payload->struct_size != struct_size);

	if ((ret = ioctl_cmds[ioctl_cmd].func(dev, result)))
		goto exit_free;

	//print_hex_dump(KERN_DEBUG, "ret: ", DUMP_PREFIX_OFFSET, 16, 1,
	//	       result->payload, struct_size, true);
	if (is_rw && copy_to_user(user_data, result->payload, struct_size))
		ret = -EFAULT;

exit_free:
	kfree(result);
	return ret;
}

/**
 * mcp2210_ioctl_complete - callback for driver to complete ioctls
 *
 * Returns -EINPROGRESS if somebody else will free the command later
 */
static int mcp2210_ioctl_complete(struct mcp2210_cmd *cmd_head, void *context)
{
	struct ioctl_result *r = context;
	struct mcp2210_device *dev = cmd_head->dev;
	int cmd_status;
	int result_has_status = r->status && r->status != -EINPROGRESS;
	int ret = 0;

	mcp2210_info();

	/* run configure general-purpose command */
	if (r->ioctl_cmd == MCP2210_IOCTL_CONFIG_SET && !cmd_head->type) {
		if (result_has_status) {
			mcp2210_err("not configuring due to previous errors");
		} else {
			BUG_ON(dev->config);
			BUG_ON(!r->new_config);

			cmd_head->status = mcp2210_configure(dev, r->new_config);
			if (cmd_head->status) {
				mcp2210_err("failed to configure %d", ret);
				kfree(r->new_config);
			}
		}
		r->new_config = NULL;
	}

	cmd_status = cmd_head->status;
	if (!result_has_status && cmd_status) {
		r->status = cmd_status;
		r->mcp_status = cmd_head->mcp_status;
	}

	switch (r->ioctl_cmd) {
	case MCP2210_IOCTL_CMD: {
		//struct mcp2210_cmd_ctl *cmd = (void *)cmd_head;
		struct mcp2210_ioctl_data_cmd *idc = &r->payload[0].body.cmd;

		BUG_ON(!cmd_head->type);
		BUG_ON(cmd_head->type->id != MCP2210_CMD_TYPE_CTL);

		/* if the command completed, copy the response msg */
		if (!cmd_status)
			memcpy(&idc->rep, dev->eps[EP_IN].buffer, 64);
	}

#ifdef CONFIG_MCP2210_EEPROM
	case MCP2210_IOCTL_EEPROM: {
		struct mcp2210_cmd_eeprom *cmd = (void *)cmd_head;

		BUG_ON(!cmd_head->type);
		BUG_ON(cmd_head->type->id != MCP2210_CMD_TYPE_EEPROM);

		if (cmd->op == MCP2210_CMD_READ_EEPROM) {
			struct mcp2210_ioctl_data_eeprom *ide;

			ide = &r->payload[0].body.eeprom;
			BUG_ON(cmd->addr + cmd->size > MCP2210_EEPROM_SIZE);
			BUG_ON(cmd->size != ide->size);
//			mcp2210_debug("in my mind, ide: %p, ide->data: %p, cmd->addr: %hhu, cmd->size: %hu", ide, ide->data, cmd->addr, cmd->size);
//			mcp2210_debug("dev->eeprom_cache: %02x %02x...", dev->eeprom_cache[0], dev->eeprom_cache[1]);
			memcpy(ide->data, &dev->eeprom_cache[cmd->addr], cmd->size);
		}
		break;
	}
#endif /* CONFIG_MCP2210_EEPROM */

	case MCP2210_IOCTL_CONFIG_GET:
		/* this ioctl doesn't require a callback */
		BUG();
		break;

	case MCP2210_IOCTL_CONFIG_SET:
		++r->num_cmds_finished;

		/* don't complete until all commands are done */
		if (r->num_cmds_finished < r->num_cmds)
			return 0;

		break;

	default:
		BUG();
		break;
	};

//exit:
	r->cmd = (ret == -EINPROGRESS) ? (void*)cmd_head : NULL;
	complete_all(&r->completion);
	return ret;
}

long mcp2210_ioctl_cmd(struct mcp2210_device *dev, struct ioctl_result *result)
{
	struct mcp2210_ioctl_data_cmd *idc = &result->payload->body.cmd;
	struct mcp2210_cmd_ctl *cmd;
	int ret;

	mcp2210_debug();

	/* commands we don't allow because they will screw with things */
	switch (idc->req.cmd) {
	case MCP2210_CMD_SPI_TRANSFER:
	case MCP2210_CMD_READ_EEPROM:
	case MCP2210_CMD_WRITE_EEPROM:
	case MCP2210_CMD_SET_SPI_CONFIG:
	case MCP2210_CMD_SET_CHIP_CONFIG:
		mcp2210_err("Command 0x%hhx not permitted or supported via this interface", idc->req.cmd);
		return -EPERM;

	default:
		break;
	}

	cmd = mcp2210_alloc_ctl_cmd(dev,
				    idc->req.cmd,
				    idc->req.head.req.xet.sub_cmd,
				    idc->req.body.raw,
				    sizeof(idc->req.body.raw),
				    false, GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	cmd->head.complete = mcp2210_ioctl_complete;
	cmd->head.context = result;

	if (!(ret = mcp2210_add_cmd((void *)cmd, true)))
		return ret;

	mcp2210_info("waiting...");
	wait_for_completion(&result->completion);
	return result->status;
}

#ifndef CONFIG_MCP2210_EEPROM
static long mcp2210_ioctl_eeprom(struct mcp2210_device *dev, struct ioctl_result *result)
{
	mcp2210_warn("EEPROM support unavailable.");
	return -EPERM;
}
#else
static long mcp2210_ioctl_eeprom(struct mcp2210_device *dev, struct ioctl_result *result)
{
	struct mcp2210_ioctl_data_eeprom *ide = &result->payload->body.eeprom;
	int ret;

	mcp2210_info();

	/* make sure the buffer is really large enough */
	if (result->payload->struct_size < IOCTL_DATA_EEPROM_SIZE + ide->size)
		return -EOVERFLOW;

	/* I hope the compiler only generates the setup once */
	if (ide->is_read)
		ret = mcp2210_eeprom_read(dev, ide->data, ide->addr, ide->size,
					  mcp2210_ioctl_complete, result,
					  GFP_KERNEL);
	else
		ret = mcp2210_eeprom_write(dev, ide->data, ide->addr, ide->size,
					   mcp2210_ioctl_complete, result,
					   GFP_KERNEL);

	/* zero return value means that we're already done */
	if (!ret)
		goto complete_ioctl;
	else if (ret != -EINPROGRESS)
		return ret;

	mcp2210_info("waiting...");
	wait_for_completion(&result->completion);

complete_ioctl:
	return result->status;
}
#endif /* CONFIG_MCP2210_EEPROM */

static void reset_string_addr_single(const char **str, const char *min, const char *max, long diff)
{
	const void *orig = *str;

	if (*str < min || *str > max)
		*str = NULL;
	else
		*str -= diff;

	printk(KERN_DEBUG "str: %p --> %p\n", orig, *str);
}

static void reset_string_addr(struct mcp2210_board_config *bc, const __user struct mcp2210_board_config *user_ptr)
{
	long diff = (long)bc - (long)user_ptr;
	const char *min = bc->strings;
	const char *max = &bc->strings[bc->strings_size - 1];
	u8 i;

	//printk(KERN_DEBUG "bc = %p (min = %p, max = %p), user_ptr = %p, diff = 0x%08x\n", bc, min, max, user_ptr, diff);
	/* reset addresses of strings and NULL out any that don't point to a string within the struct */
	for (i = 0; i < MCP2210_NUM_PINS; ++i) {
		struct mcp2210_pin_config *pin = &bc->pins[i];

		reset_string_addr_single(&pin->name, min, max, diff);
		reset_string_addr_single(&pin->modalias, min, max, diff);
	}
}

static long mcp2210_ioctl_config_get(struct mcp2210_device *dev, struct ioctl_result *result)
{
	struct mcp2210_ioctl_data *id = result->payload;
	struct mcp2210_ioctl_data_config *idc = &id->body.config;
	struct mcp2210_state *idcs = &idc->state;
	struct mcp2210_state *devs = &dev->s;
	unsigned long irqflags;
	int ret;

	mcp2210_info();

	spin_lock_irqsave(&dev->dev_spinlock, irqflags);
	idcs->have_chip_settings	  = devs->have_chip_settings;
	idcs->have_power_up_chip_settings = devs->have_power_up_chip_settings;
	idcs->have_spi_settings		  = devs->have_spi_settings;
	idcs->have_power_up_spi_settings  = devs->have_power_up_spi_settings;
	idcs->have_usb_key_params	  = devs->have_usb_key_params;
	dev->have_config		  = !!dev->config;
	dev->is_spi_probed		  = !!dev->spi_master;
	dev->is_gpio_probed		  = dev->is_gpio_probed;

	if (devs->have_chip_settings)
		memcpy(&idcs->chip_settings,
		       &devs->chip_settings,
		       sizeof(idcs->chip_settings));

	if (devs->have_power_up_chip_settings)
		memcpy(&idcs->power_up_chip_settings,
		       &devs->power_up_chip_settings,
		       sizeof(idcs->power_up_chip_settings));

	if (devs->have_spi_settings)
		memcpy(&idcs->spi_settings,
		       &devs->spi_settings,
		       sizeof(idcs->spi_settings));

	if (devs->have_power_up_spi_settings)
		memcpy(&idcs->power_up_spi_settings,
		       &devs->power_up_spi_settings,
		       sizeof(idcs->power_up_spi_settings));

	if (devs->have_usb_key_params)
		memcpy(&idcs->usb_key_params,
		       &devs->usb_key_params,
		       sizeof(idcs->usb_key_params));

	if (dev->config) {
		void *vret;
		idc->config.strings_size = id->struct_size - IOCTL_DATA_CONFIG_SIZE;
		vret = copy_board_config(&idc->config, dev->config, 0);
		if (IS_ERR(vret)) {
			ret = PTR_ERR(vret);
			goto exit_unlock;
		}
		reset_string_addr(&idc->config, &result->user_data->body.config.config);
	}

	spin_unlock_irqrestore(&dev->dev_spinlock, irqflags);

	if (0) {
		printk(KERN_DEBUG "is_spi_probed:  %hhu\n", dev->is_spi_probed);
		printk(KERN_DEBUG "is_gpio_probed: %hhu\n", dev->is_gpio_probed);
		printk(KERN_DEBUG "have_config: %hhu\n", dev->have_config);
		dump_chip_settings(KERN_DEBUG, 0, ".chip_settings = ", &idcs->chip_settings);
		dump_chip_settings(KERN_DEBUG, 0, ".power_up_chip_settings = ", &idcs->power_up_chip_settings);
		if (dev->have_config)
			dump_board_config(KERN_DEBUG, 0, ".config = ", &idc->config);
	}

	return 0;

exit_unlock:
	spin_unlock_irqrestore(&dev->dev_spinlock, irqflags);
	return ret;
}

long mcp2210_ioctl_config_set(struct mcp2210_device *dev, struct ioctl_result *result)
{
	struct mcp2210_ioctl_data *id = result->payload;
	struct mcp2210_ioctl_data_config *idc = &id->body.config;
	unsigned long irqflags;
	int ret = 0;
	LIST_HEAD(cmds);

	mcp2210_info();

	if (dev->config && idc->have_config) {
		mcp2210_err("already configured (can only set board_config once)");
		return -EPERM;
	}

	if (idc->have_config && !(idc->state.have_chip_settings
			|| dev->s.have_chip_settings)) {
		mcp2210_err("need chip settings");
		return -EPERM;
	}

	if (idc->state.have_chip_settings) {
		struct mcp2210_cmd_ctl *cmd = mcp2210_alloc_ctl_cmd(
				dev, MCP2210_CMD_SET_CHIP_CONFIG, 0,
				&idc->state.chip_settings,
				sizeof(idc->state.chip_settings),
				false, GFP_KERNEL);
		if (!cmd)
			goto exit_nomem;
		list_add_tail(&cmd->head.node, &cmds);
	}

	if (idc->state.have_power_up_chip_settings) {
		struct mcp2210_cmd_ctl *cmd = mcp2210_alloc_ctl_cmd(
				dev, MCP2210_CMD_SET_NVRAM, MCP2210_NVRAM_CHIP,
				&idc->state.power_up_chip_settings,
				sizeof(idc->state.power_up_chip_settings),
				false, GFP_KERNEL);
		if (!cmd)
			goto exit_nomem;
		list_add_tail(&cmd->head.node, &cmds);
	}

	if (idc->state.have_spi_settings) {
		struct mcp2210_cmd_ctl *cmd = mcp2210_alloc_ctl_cmd(
				dev, MCP2210_CMD_SET_SPI_CONFIG, 0,
				&idc->state.spi_settings,
				sizeof(idc->state.spi_settings),
				false, GFP_KERNEL);
		if (!cmd)
			goto exit_nomem;
		list_add_tail(&cmd->head.node, &cmds);
	}

	if (idc->state.have_power_up_spi_settings) {
		struct mcp2210_cmd_ctl *cmd = mcp2210_alloc_ctl_cmd(
				dev, MCP2210_CMD_SET_NVRAM, MCP2210_NVRAM_SPI,
				&idc->state.power_up_spi_settings,
				sizeof(idc->state.power_up_spi_settings),
				false, GFP_KERNEL);
		if (!cmd)
			goto exit_nomem;
		list_add_tail(&cmd->head.node, &cmds);
	}

	if (idc->state.have_usb_key_params) {
		struct mcp2210_cmd_ctl *cmd = mcp2210_alloc_ctl_cmd(
				dev, MCP2210_CMD_SET_NVRAM, MCP2210_NVRAM_KEY_PARAMS,
				&idc->state.usb_key_params,
				sizeof(idc->state.usb_key_params),
				false, GFP_KERNEL);
		if (!cmd)
			goto exit_nomem;
		list_add_tail(&cmd->head.node, &cmds);
	}

	if (idc->have_config) {
		struct mcp2210_cmd *cmd;

		result->new_config = copy_board_config(NULL, &idc->config, GFP_KERNEL);
		if (!result->new_config)
			goto exit_nomem;

		cmd = mcp2210_alloc_cmd(dev, NULL, sizeof(struct mcp2210_cmd), GFP_KERNEL);
		if (!cmd) {
			kfree(result->new_config);
			result->new_config = NULL;
			goto exit_nomem;
		}

		cmd->nonatomic = 1;
		list_add_tail(&cmd->node, &cmds);
	}

	/* if they didn't specify anything... */
	if (list_empty(&cmds)) {
		mcp2210_err("no configuration parameters selected");
		return -EINVAL;
	}

	/* once creation of all commands succeeds, we add to queue */

	spin_lock_irqsave(&dev->dev_spinlock, irqflags);
	spin_lock(&dev->queue_spinlock);

	if (dev->dead)
		ret = -ERESTARTSYS;
	else {
		while (!list_empty(&cmds)) {
			struct mcp2210_cmd *cmd;

			cmd = list_first_entry(&cmds, struct mcp2210_cmd, node);
			list_del(&cmd->node);
			cmd->complete = mcp2210_ioctl_complete;
			cmd->context = result;
			++result->num_cmds;
			list_add_tail(&cmd->node, &dev->cmd_queue);
			dump_cmd(KERN_DEBUG, 0, "adding cmd: ", cmd);
		}
	}

	spin_unlock(&dev->queue_spinlock);
	if (!ret)
		process_commands(dev, true, false);
	spin_unlock_irqrestore(&dev->dev_spinlock, irqflags);

	/* help command execute more quickly? */
	schedule();

	if (!ret) {
		mcp2210_info("waiting for completion...");
		wait_for_completion(&result->completion);
	} else
		result->status = ret;

	return result->status;

exit_nomem:
	while (!list_empty(&cmds)) {
		struct mcp2210_cmd *cmd = list_first_entry(&cmds,
							   struct mcp2210_cmd,
							   node);
		list_del(cmds.next);
		kfree(cmd);
	}

	return -ENOMEM;
}
#endif /* CONFIG_MCP2210_IOCTL */

