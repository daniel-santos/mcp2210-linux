/*
 * MCP2210 software IRQ controller
 *
 * Copyright (c) 2013 Daniel Santos <daniel.santos@pobox.com>
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

#include "mcp2210.h"
#include "mcp2210-debug.h"

#ifdef CONFIG_MCP2210_IRQ

#include <linux/irq.h>

static int complete_poll(struct mcp2210_cmd *cmd, void *context);

static void mcp2210_irq_mask(struct irq_data *data) {
	struct mcp2210_device *dev = irq_data_get_irq_chip_data(data);
	dev->irq_mask |= data->mask;
}

static void mcp2210_irq_unmask(struct irq_data *data) {
	struct mcp2210_device *dev = irq_data_get_irq_chip_data(data);
	dev->irq_mask &= ~data->mask;
}

static int mcp2210_irq_set_type(struct irq_data *data, unsigned int flow_type) {
	struct mcp2210_device *dev = irq_data_get_irq_chip_data(data);
	BUG_ON(data->hwirq > 7);
	dev->irq_type[data->hwirq] = flow_type;
	mcp2210_info("irq type set to 0x%02x", flow_type);
	return 0;
}

static struct irq_chip mcp2210_irq_chip = {
	.name		= "mcp2210",
	.irq_mask	= mcp2210_irq_mask,
	.irq_unmask	= mcp2210_irq_unmask,
	.irq_set_type	= mcp2210_irq_set_type,
};

/******************************************************************************
 * probe & remove
 */
int mcp2210_irq_probe(struct mcp2210_device *dev)
{
	uint i;
	int ret;

	mcp2210_info();
	mutex_init(&dev->irq_lock);

	dev->nr_irqs = 0;
	dev->poll_intr = 0;
	dev->poll_gpio = 0;

	for (i = 0; i < MCP2210_NUM_PINS; ++i) {
		const struct mcp2210_pin_config *pin = &dev->config->pins[i];

		if (pin->mode == MCP2210_PIN_SPI || !pin->has_irq)
			continue;

		++dev->nr_irqs;
		BUG_ON(dev->irq_revmap[i]);
		dev->irq_revmap[i] = pin->irq;

		if (pin->mode == MCP2210_PIN_DEDICATED)
			dev->poll_intr = 1;
		else if (pin->mode == MCP2210_PIN_GPIO) {
			dev->poll_gpio = 1;
			dev->irq_type[i] = pin->irq_type;
		}
	}

	if (!dev->nr_irqs)
		return 0;

	ret = irq_alloc_descs(-1, 0, dev->nr_irqs, 0);
	if (ret < 0) {
		/* CONFIG_SPARSE_IRQ needed? */
		mcp2210_err("Failed to allocate %u irq descriptors: %d", dev->nr_irqs, ret);
		return ret;
	}
	dev->irq_base = ret;

	for (i = 0; i < dev->nr_irqs; ++i) {
		int virq = dev->irq_base + i;

		dev->irq_descs[i] = irq_to_desc(virq);
		BUG_ON(!dev->irq_descs[i]);
		irq_set_chip_data(virq, dev);
		irq_set_chip(virq, &mcp2210_irq_chip);
#if 0
		/* nested not finished */
		if (pin->irq_threaded)
			irq_set_nested_thread(irq, true);
#endif

#ifdef CONFIG_ARM
		set_irq_flags(virq, IRQF_VALID);
#else
		irq_set_noprobe(virq);
#endif
	}

#ifdef CONFIG_MCP2210_GPIO
	if (dev->poll_gpio) {
		ctl_cmd_init(dev, &dev->cmd_poll_gpio,
			     MCP2210_CMD_GET_PIN_VALUE, 0, NULL, 0, false);
		dev->cmd_poll_gpio.head.complete = complete_poll;
		mcp2210_add_cmd(&dev->cmd_poll_gpio.head, false);
	}
#endif /* CONFIG_MCP2210_GPIO */

	if (dev->poll_intr) {
		/* read and then reset */
		ctl_cmd_init(dev, &dev->cmd_poll_intr,
			     MCP2210_CMD_GET_INTERRUPTS, 0, NULL, 0, false);
		dev->cmd_poll_intr.head.complete = complete_poll;
		mcp2210_add_cmd(&dev->cmd_poll_intr.head, false);
	}

	dev->is_irq_probed = 1;
	dev->suppress_poll_warn = 0;

	return 0;
}

void mcp2210_irq_disable(struct mcp2210_device *dev)
{
	mcp2210_info();
	if (dev->is_irq_probed) {
		const int virq_end = dev->irq_base + dev->nr_irqs;
		int virq;

		for (virq = dev->irq_base; virq < virq_end; ++virq) {
			irq_set_status_flags(virq, IRQ_NOREQUEST);
			irq_set_chip_and_handler(virq, NULL, NULL);
			synchronize_irq(virq);
		}
	}
}

void mcp2210_irq_remove(struct mcp2210_device *dev)
{
	mcp2210_info();
	if (dev->is_irq_probed) {
		const int virq_end = dev->irq_base + dev->nr_irqs;
		int virq;

		for (virq = dev->irq_base; virq < virq_end; ++virq) {
			irq_free_desc(virq);
			dev->irq_descs[virq] = NULL;
		}
		dev->is_irq_probed = 0;
	}
}

/******************************************************************************
 * polling and virtual IRQ trigger functions
 */

void _mcp2210_irq_do_gpio(struct mcp2210_device *dev, u16 old_val, u16 new_val)
{
	const int up_mask   = IRQ_TYPE_EDGE_RISING | IRQ_TYPE_LEVEL_HIGH;
	const int down_mask = IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_LEVEL_LOW;

	uint i;
	for (i = 0; i < MCP2210_NUM_PINS; ++i) {
		struct mcp2210_pin_config *pin = &dev->config->pins[i];
		u8 old_pin_val;
		u8 new_pin_val;

		if (!pin->has_irq || !pin->mode == MCP2210_PIN_GPIO)
			continue;

		old_pin_val = 1 & (old_val >> i);
		new_pin_val = 1 & (new_val >> i);

		if (old_pin_val == new_pin_val)
			continue;

		if (pin->irq_type & (new_pin_val ? up_mask : down_mask)) {
			struct irq_desc *desc = dev->irq_descs[pin->irq];
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,3,0)
			handle_simple_irq(desc);
#else
			int virq = dev->irq_base + pin->irq;
			handle_simple_irq(virq, desc);
#endif
		}
	}
}

void _mcp2210_irq_do_intr_counter(struct mcp2210_device *dev, u16 count)
{
	struct mcp2210_pin_config *pin = &dev->config->pins[6];
	struct irq_desc *desc = dev->irq_descs[pin->irq];

	if (dev->s.chip_settings.pin_mode[6] != MCP2210_PIN_DEDICATED)
		return;

	/* We're discarding count and just letting handlers know that at least
	 * one interrupt occured. Maybe needs a mechanism to let consumers
	 * know the count? */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,3,0)
	handle_simple_irq(desc);
#else
	handle_simple_irq(dev->irq_base + pin->irq, desc);
#endif
}

static void warn_poll_past_due(struct mcp2210_device *dev, unsigned long now,
			       long how_late)
{
	mcp2210_warn("Next poll is past due by %ums.\n",
		     jiffies_to_msecs(how_late));

	if (!poll_delay_warn_secs)
		return;

	/* If result ends up zero then it'll just repeat once */
	dev->suppress_poll_warn = now + msecs_to_jiffies(1000
			          * poll_delay_warn_secs);
	mcp2210_warn("...suppressing for %u seconds.\n", poll_delay_warn_secs);
}

static int complete_poll(struct mcp2210_cmd *cmd_head, void *context)
{
	struct mcp2210_device *dev = cmd_head->dev;
	struct mcp2210_cmd_ctl *cmd = (void*)cmd_head;
	unsigned long now = jiffies;
	int enabled;
	unsigned long interval;
	unsigned long interval_j;
	unsigned long next;
	long next_diff;

	mcp2210_debug();

	if (dev->dead)
		return -EINPROGRESS;

	if (cmd->req.cmd == MCP2210_CMD_GET_PIN_VALUE) {
		enabled = dev->poll_gpio;
		interval = dev->config->poll_gpio_usecs;
		dev->last_poll_gpio = now;
	} else {
		enabled = dev->poll_intr;
		interval = dev->config->poll_intr_usecs;
		dev->last_poll_intr = now;
	}

	if (dev->suppress_poll_warn && jiffdiff(dev->suppress_poll_warn, now) <= 0)
		dev->suppress_poll_warn = 0;

	if (!enabled)
		return -EINPROGRESS;

	interval_j = usecs_to_jiffies(interval);
	next = dev->eps[EP_OUT].submit_time + interval_j;
	next_diff = jiffdiff(next, now);
	cmd->head.delayed = 1;

	if (next_diff < 0) {
		next = now + interval_j;
		if (!dev->suppress_poll_warn)
			warn_poll_past_due(dev, now, -next_diff);
	}
	cmd->head.delay_until = next;

#if 0
	mcp2210_debug("interval_j: %lu, submit_time: %lu, next: %lu, jiffies: %lu",
		      interval_j, dev->eps[EP_OUT].submit_time, next, jiffies);
#endif

	cmd->head.state = MCP2210_STATE_NEW;

	mcp2210_add_cmd(cmd_head, false);

	return -EINPROGRESS; /* tell process_commands not to free us */
}

#endif /* CONFIG_MCP2210_IRQ */
