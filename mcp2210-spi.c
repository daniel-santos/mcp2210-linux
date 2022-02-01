/*
 *  MCP2210 driver spi layer
 *
 *  Copyright (c) 2013 Mathew King <mking@trilithic.com> for Trilithic, Inc
 *		  2013-2017 Daniel Santos <daniel.santos@pobox.com>
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

#include <linux/spi/spi.h>
#include <linux/version.h>
#include <linux/kernel.h>

#include "mcp2210.h"
#include "mcp2210-debug.h"

#ifdef CONFIG_MCP2210_SPI

/* The non-queued mechanism will supposedly be phased out in the future.
 * However, we don't get any benefit from the new API since we just queue
 * a command (in our own queue) when we get a new message anyway. Thus, when/if
 * the old SPI transfer() mechanism is phased out, this version check should be
 * modified to force use of the new queued mechanism.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(99,99,0)
# define USE_SPI_QUEUE 1
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,8,0)
# define HAVE_SPI_CS_GPIO 1
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,12,0)
# define HAVE_SPI_MASTER_SPEED 1
#endif

static int spi_submit_prepare(struct mcp2210_cmd *cmd_head);
static int spi_complete_urb(struct mcp2210_cmd *cmd_head);
static int spi_mcp_error(struct mcp2210_cmd *cmd_head);
static int spi_complete_cmd(struct mcp2210_cmd *cmd_head, void *context);

const struct mcp2210_cmd_type mcp2210_cmd_type_spi = {
	.id = MCP2210_CMD_TYPE_SPI,
	.submit_prepare = spi_submit_prepare,
	.complete_urb = spi_complete_urb,
	.mcp_error = spi_mcp_error,
	.dump = dump_cmd_spi,
	.desc = "spi"
};

static inline struct mcp2210_device *mcp2210_spi2dev(struct spi_device *spi)
{
	return *((struct mcp2210_device **)spi_master_get_devdata(spi->master));
}

static inline struct mcp2210_device *mcp2210_spi_master2dev(struct spi_master *master)
{
	return *((struct mcp2210_device **)spi_master_get_devdata(master));
}

/******************************************************************************
 * SPI Master funtions
 */

static void mcp2210_spi_cleanup(struct spi_device *spi);
static int mcp2210_spi_setup(struct spi_device *spi);
#ifndef USE_SPI_QUEUE
static int transfer(struct spi_device *spi, struct spi_message *msg);
#else
static int prepare_transfer_hardware(struct spi_master *master);
static int transfer_one_message(struct spi_master *master, struct spi_message *msg);
static int unprepare_transfer_hardware(struct spi_master *master);
#endif /* USE_SPI_QUEUE */

static void mcp2210_spi_probe_async(struct work_struct *work);

struct async_spi_probe {
	struct work_struct work;
	struct mcp2210_device *dev;
};

/**
 * mcp2210_spi_probe -
 * @dev:
 */
// may sleep
int mcp2210_spi_probe(struct mcp2210_device *dev) {
	struct spi_master *master; /* just a local for code brievity */
	int ret;
	struct async_spi_probe *async_probe;

	mcp2210_info();

	BUG_ON(!dev);
	BUG_ON(!dev->config);
	BUG_ON(dev->spi_master);

	if (!dev || !dev->config || dev->spi_master)
		return -EINVAL;

	async_probe = kzalloc(sizeof(*async_probe), GFP_KERNEL);
	if (!async_probe)
		return -ENOMEM;

	INIT_WORK(&async_probe->work, mcp2210_spi_probe_async);
	async_probe->dev = dev;

	master = spi_alloc_master(&dev->udev->dev, sizeof(void*));
	if (!master)
		return -ENOMEM;

	/* we only need a pointer to the struct mcp2210_device */
	*((struct mcp2210_device **)spi_master_get_devdata(master)) = dev;

#ifdef HAVE_SPI_MASTER_SPEED
	master->max_speed_hz = MCP2210_MAX_SPEED;
	master->min_speed_hz = MCP2210_MIN_SPEED;
#endif
	master->bus_num = -1;
	master->num_chipselect = MCP2210_NUM_PINS;
	/* unused: master->dma_alignment */
	master->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH | SPI_LSB_FIRST; // | SPI_3WIRE | SPI_NO_CS;
	/* TODO: what's bits_per_word_mask for? */
	master->flags = 0;
	/* unused: master->bus_lock_spinlock
	 * unused: master->bus_lock_mutex
	 * unused: master->bus_lock_flag
	 */
	master->setup = mcp2210_spi_setup;
	master->cleanup = mcp2210_spi_cleanup;
#ifndef USE_SPI_QUEUE
	master->transfer = transfer;
#else
	master->transfer = NULL;
	master->prepare_transfer_hardware = prepare_transfer_hardware;
	master->transfer_one_message = transfer_one_message;
	master->unprepare_transfer_hardware = unprepare_transfer_hardware;
#endif

	memset(dev->chips, 0, sizeof(dev->chips));

	ret = spi_register_master(master);

	if (ret) {
		spi_master_put(master);
		dev->spi_master = NULL;

		return ret;
	}

	dev->spi_master = master;
	dev->is_spi_probed = 1;
	schedule_work(&async_probe->work);

	return 0;
}

/* Manage probing SPI protocol drivers, which must be done in a context that
 * can sleep. */
static void mcp2210_spi_probe_async(struct work_struct *work) {
	struct async_spi_probe *async_probe = (void*)work;
	struct mcp2210_device *dev = async_probe->dev;
	struct spi_master *master = dev->spi_master;
	int ret;
	unsigned i;

	mcp2210_info();

	for (i = 0; i < MCP2210_NUM_PINS; ++i) {
		struct mcp2210_pin_config *cfg = &dev->config->pins[i];
		struct spi_device *chip;
		const char *modalias;

		if (cfg->mode != MCP2210_PIN_SPI)
			continue;

		chip = spi_alloc_device(master);
		if (!chip) {
			ret = -ENOMEM;
			goto error;
		}

		chip->max_speed_hz  = cfg->spi.max_speed_hz;
		chip->chip_select   = i;
		chip->mode	    = cfg->spi.mode;
		chip->bits_per_word = cfg->spi.bits_per_word;

#ifdef HAVE_SPI_CS_GPIO
# ifdef CONFIG_MCP2210_GPIO
		if (cfg->spi.use_cs_gpio)
			chip->cs_gpio = dev->gpio.base + cfg->spi.cs_gpio;
		else
# endif
			chip->cs_gpio = -EINVAL;
#endif /* HAVE_SPI_CS_GPIO */

#ifdef CONFIG_MCP2210_IRQ
		if (cfg->has_irq)
			chip->irq = dev->irq_base + cfg->irq;
		else
#endif
			chip->irq = -1;

		/* unused: chip->controller_state
		 * unused: chip->controller_data
		 */
		modalias = cfg->modalias ? cfg->modalias : "spidev";

		WARN_ON(strlen(modalias) >= sizeof(chip->modalias));
		strncpy(chip->modalias, modalias, sizeof(chip->modalias));

		dev->chips[i] = chip;
		ret = spi_add_device(chip);
		if (ret < 0) {
			/* FIXME: almost certainly a race condition */
			dev->chips[i] = NULL;
			spi_dev_put(chip);
			goto error;
		}
	}

	mcp2210_info("spi device probe completed");
	kfree(async_probe);
	return;// 0;

error:
	mcp2210_err("SPI device failed to probe: %d\n", ret);
	dev->spi_master = NULL;
	memset(dev->chips, 0, sizeof(dev->chips));
	spi_unregister_master(master);
	kfree(async_probe);

	return;// ret;
}

void mcp2210_spi_remove(struct mcp2210_device *dev)
{
	if (!dev || !dev->spi_master)
		return;

	mcp2210_info();
	spi_unregister_master(dev->spi_master);

	dev->spi_master = NULL;
}


/* cleanup for this SPI device (not the SPI master) */
static void mcp2210_spi_cleanup(struct spi_device *spi)
{
	struct mcp2210_device *dev = mcp2210_spi2dev(spi);

	mcp2210_info("pin %d", spi->chip_select);

	/* Do we have any cleanup to do? */
}

#ifdef HAVE_SPI_MASTER_SPEED
static inline int validate_speed(struct mcp2210_device *dev, u32 bitrate)
{
	return 0;
}
#else
static int validate_speed(struct mcp2210_device *dev, u32 bitrate)
{
	if (bitrate < MCP2210_MIN_SPEED || bitrate > MCP2210_MAX_SPEED) {
		mcp2210_err("The requested speed of %uHz is not supported "
			    "(must be in the range of 1500Hz and 12MHz)",
			    bitrate);
		return -EINVAL;
	}

	return 0;
}
#endif

/* must hold dev_spinlock */
/**
 * is_spi_in_flight - determine if a non-failed SPI message is in progress on
 * 		      the specified pin
 *
 * This will not return true if an SPI transfer is "in progress" or may be in
 * progress on the chip due to some communication failure, but only if a real,
 * live non-failed transfer is in progress.
 */
static int is_spi_in_flight(const struct mcp2210_device *dev, u8 pin)
{
	const struct mcp2210_cmd *cmd_head = dev->cur_cmd;

	if (cmd_head && cmd_head->type == &mcp2210_cmd_type_spi) {
		const struct mcp2210_cmd_spi_msg *cmd = (void *)cmd_head;

		return cmd->spi_in_flight && cmd->spi->chip_select == pin;
	}
	return 0;
}

/* may sleep */
static int mcp2210_spi_setup(struct spi_device *spi)
{
	struct mcp2210_device *dev = mcp2210_spi2dev(spi);
	unsigned long irqflags;
	u8 pin = spi->chip_select;
	int ret = 0;

	mcp2210_info("spi_in_flight: %d, cur_spi_config: %d",
		     dev->spi_in_flight, dev->s.cur_spi_config);

	if (dump_cmds)
		dump_spi_device(KERN_INFO, 0, "mcp2210_spi_setup: "
				"spi_device = ", spi);

	if (validate_speed(dev, spi->max_speed_hz))
		return -EINVAL;

	if (spi->bits_per_word != 8) {
		mcp2210_err("MCP2210 only supports 8 bits per word, got "
			    "request for %hhu", spi->bits_per_word);
		return -EINVAL;
	}

	if (pin > 8) {
		mcp2210_err("spi->chip_select must be in the range of 0 - 8, "
			    "got %hu", pin);
		return -EINVAL;
	}

	spin_lock_irqsave(&dev->dev_spinlock, irqflags);

	/* We must error if a transfer is in progress on the same SPI device */
	if (is_spi_in_flight(dev, pin)) {
		mcp2210_err("SPI message in progress");
		ret = -EBUSY;
	}

	spin_unlock_irqrestore(&dev->dev_spinlock, irqflags);
	return ret;
}

static int queue_msg(struct mcp2210_device *dev, struct spi_message *msg,
		     bool can_sleep)
{
	u8 pin = msg->spi->chip_select;
	struct mcp2210_cmd_spi_msg *cmd;
	struct list_head *pos;
	struct mcp2210_pin_config *pin_config = &dev->config->pins[pin];
	struct spi_transfer *first_xfer = list_entry(msg->transfers.next,
						     struct spi_transfer,
						     transfer_list);
	struct spi_transfer *last_xfer = list_entry(msg->transfers.prev,
						     struct spi_transfer,
						     transfer_list);
	gfp_t gfp_flags = can_sleep ? GFP_KERNEL : GFP_ATOMIC;
	uint xfer_chain_size = 0;
	int ret;

	mcp2210_debug("Start new transfer (pin %d)\n", pin);

	if (dev->dead)
		return -ESHUTDOWN;

	/* debug-only sanity checks */
	if (IS_ENABLED(CONFIG_MCP2210_DEBUG)) {
		if (pin_config->mode != MCP2210_PIN_SPI) {
			mcp2210_err("Attempt to SPI on non-spi pin!");
			return -EINVAL;
		}

		if (list_empty(&msg->transfers)) {
			mcp2210_err("empty transfer list");
			return -EINVAL;
		}
	}

	if (last_xfer->cs_change) {
		mcp2210_err("Unsupported: cs_change set on last transfer in "
			    "message.");
		return -EINVAL;
	}

	list_for_each(pos, &msg->transfers) {
		struct spi_transfer *xfer = list_entry(
				pos,
				struct spi_transfer,
				transfer_list);

		/* Validate that an individual transfer or chain isn't too
		 * large for this poor thing to handle (if you really need this
		 * and are using gpio for cs then you can hack this driver to
		 * make it happen). */
		xfer_chain_size += xfer->len;

		if (xfer->len > 0xffff) {
			mcp2210_err("SPI transfer (or chain) too large for "
				    "MCP2210 (%u bytes)", xfer_chain_size);
			return -EINVAL;
		}

		if (xfer->cs_change && xfer != last_xfer)
			xfer_chain_size = 0;

		BUG_ON(!xfer->tx_buf);

		if (xfer->bits_per_word && xfer->bits_per_word != 8) {
			mcp2210_warn("unsupported: spi_transfer.bits_per_word "
				     "= %hhu", xfer->bits_per_word);
			return -EINVAL;
		}

		if (xfer->speed_hz && validate_speed(dev, xfer->speed_hz))
			return -EINVAL;
	}


	cmd = mcp2210_alloc_cmd_type(dev, struct mcp2210_cmd_spi_msg,
				     &mcp2210_cmd_type_spi, gfp_flags);

	if (!cmd)
		return -ENOMEM;

	cmd->head.can_retry = 1;
	cmd->head.complete = spi_complete_cmd;
	cmd->head.context = NULL;
	cmd->spi = msg->spi;
	cmd->msg = msg;
	cmd->xfer = first_xfer;
	cmd->head.non_preemptable = 1;
	/* not needed (kzalloc)
	cmd->pos = 0;
	cmd->pending_unacked = 0;
	cmd->pending_unacked = 0;
	cmd->pending_bytes = 0;
	cmd->busy_count = 0
	cmd->spi_in_flight = 0
	cmd->ctl_cmd = NULL;
	*/

	ret = mcp2210_add_cmd(&cmd->head, true);
	if (ret)
		return ret;

	return process_commands(dev, false, can_sleep);
}

#ifndef USE_SPI_QUEUE
/* will not sleep */
static int transfer(struct spi_device *spi, struct spi_message *msg)
{
	return queue_msg(mcp2210_spi2dev(spi), msg, false);
}

#else

static int prepare_transfer_hardware(struct spi_master *master)
{
	struct mcp2210_device *dev = mcp2210_spi_master2dev(master);
	mcp2210_info();

	/* if we knew the size of the message, we could actually prepare */

	return 0;
}

/* may sleep */
static int transfer_one_message(struct spi_master *master,
				struct spi_message *msg)
{
	int ret;

	ret = queue_msg(mcp2210_spi_master2dev(master), msg, true);
	if (ret) {
		msg->status = ret;
		spi_finalize_current_message(master);
	}

	return ret;
}

static int unprepare_transfer_hardware(struct spi_master *master)
{
	struct mcp2210_device *dev = mcp2210_spi_master2dev(master);
	mcp2210_info();

	return 0;
}
#endif /* USE_SPI_QUEUE */

/******************************************************************************
 * SPI Message command functions
 */

static int spi_ctl_cmd_submit_prepare(struct mcp2210_cmd_spi_msg *cmd)
{
	struct mcp2210_device *dev = cmd->head.dev;
	struct mcp2210_cmd *ctl_cmd_head = &dev->ctl_cmd.head;

	cmd->ctl_cmd = &dev->ctl_cmd;
	cmd->head.can_retry = 1;

	mcp2210_info("----SUBMITING CONTROL COMMAND----");
	return ctl_cmd_head->type->submit_prepare(ctl_cmd_head);
}

static int spi_prepare_device(struct mcp2210_cmd_spi_msg *cmd)
{
	struct mcp2210_device *dev = cmd->head.dev;
	struct mcp2210_spi_xfer_settings needed;
	u8 pin = cmd->spi->chip_select;

	 /* If we have an active control command it probably means that this is
	  * a retry, (failed URB or the device was busy) so we need to retry. */
	if (cmd->ctl_cmd)
		return spi_ctl_cmd_submit_prepare(cmd);

	/* Check if the chip is in the middle of a failed SPI transfer and if
	 * so, cancel it. */
	if (unlikely(dev->spi_in_flight)) {
		mcp2210_warn("*** old SPI message still in-flight, killing it "
			     "***");
		ctl_cmd_init(dev, &dev->ctl_cmd, MCP2210_CMD_SPI_CANCEL, 0,
			     NULL, 0, false);
		return spi_ctl_cmd_submit_prepare(cmd);
	}

	if (cmd->spi->mode | SPI_3WIRE) {
		/* TODO: setup gpio controlling mosi */
	}

	if (MCP2210_LOG_UNLIKELY(debug_level >= KERN_DEBUG[1] - '0')) {
		mcp2210_debug("dev->s.cur_spi_config = %d",
			      dev->s.cur_spi_config);
		dump_spi_xfer_settings(KERN_DEBUG, 0, "dev->s.spi_settings = ",
				       &dev->s.spi_settings);
	}

	calculate_spi_settings(&needed, dev, cmd->spi, cmd->msg, cmd->xfer, pin);

	if (needed.bytes_per_trans == 0) {
		mcp2210_err("Invalid: cannot send a zero-sized message");
		return -EINVAL;
	}

	/* If this is the pin we are currently configured to SPI on, then let's
	 * see if there's any difference in the SPI settings */
	if (pin == dev->s.cur_spi_config) {
		/* if nothing changed, then then do the SPI xfer */
		if (!compare_spi_settings(&needed, &dev->s.spi_settings))
			return 0;

		if (MCP2210_LOG_UNLIKELY(debug_level >= KERN_DEBUG[1] - '0')) {
			mcp2210_debug("SPI transfer settings didn't match");
			dump_spi_xfer_settings(KERN_DEBUG, 0,
					       "needed = ", &needed);
			dump_spi_xfer_settings(KERN_DEBUG, 0,
					       "dev->spi_settings = ",
					       &dev->s.spi_settings);
		}
	}

	/* TODO: still missing cmd->spi->mode & (~(SPI_MODE_3 | SPI_CS_HIGH)) */
	mcp2210_info("Settings SPI Transfer Settings");
	ctl_cmd_init(dev, &dev->ctl_cmd, MCP2210_CMD_SET_SPI_CONFIG, 0,
		     &needed, sizeof(needed), false);
	dev->ctl_cmd.pin = pin;

	return spi_ctl_cmd_submit_prepare(cmd);
}

static int spi_submit_prepare(struct mcp2210_cmd *cmd_head)
{
	struct mcp2210_cmd_spi_msg *cmd = (void *)cmd_head;
	struct mcp2210_device *dev;
	struct mcp2210_msg *req;
	struct spi_transfer *xfer;
	unsigned len;
	const void *start;
	u8 pin;

	BUG_ON(!cmd_head->dev);
	dev = cmd_head->dev;
	pin = cmd->spi->chip_select;
	req = dev->eps[EP_OUT].buffer;

	mcp2210_debug("pin %hhu\n", pin);

	BUG_ON(pin > 8);
	BUG_ON(dev->config->pins[pin].mode != MCP2210_PIN_SPI);

	/* If we're in the middle of an SPI transfer, we can't do control
	 * commands. Thus, we should never have a control command going on at
	 * the saime time we're in the middle of an SPI transfer.  Also, SPI
	 * transfers cannot be retried if there's a failed or stalled URB at
	 * the USB level.  This mostly applies to broken usb drivers, but can
	 * also with hardware failures, etc.
	 */
	if (cmd->spi_in_flight)
		BUG_ON(cmd->ctl_cmd);
	else {
		int ret = spi_prepare_device(cmd);

		/* cmd->ctl_cmd will be non-null if we're submitting a control
		 * command. */
		if (ret || cmd->ctl_cmd)
			return ret;
	}

	xfer = cmd->xfer;
	/* Dump SPI TX on very first xfer of message. */
	if (IS_ENABLED(CONFIG_MCP2210_DEBUG)
	    && cmd->pos + cmd->pending_bytes == 0 && dump_spi) {
		char msg_prefix[32];
		BUG_ON(!xfer->tx_buf);
		snprintf(msg_prefix, sizeof(msg_prefix), "%s -->: ",
			 dev_name(&cmd->spi->dev));
		print_hex_dump(KERN_INFO, msg_prefix, DUMP_PREFIX_OFFSET, 16, 1,
			       xfer->tx_buf, xfer->len, true);
	}

	/* Write spi transfer command */
	if (cmd->pos + cmd->pending_bytes < xfer->len) {
		len = xfer->len - cmd->pos - cmd->pending_bytes;

		/* don't try to send more than the buffer will hold */
		if (len > MCP2210_BUFFER_SIZE - cmd->pending_bytes) {
			len = MCP2210_BUFFER_SIZE - cmd->pending_bytes;
			if (!len)
				goto buffer_full;
		}

		if (len > MCP2210_BUFFER_SIZE - 4)
			len = MCP2210_BUFFER_SIZE - 4;

		/* for NULL tx buffer means we just send zeros.  Unfortunately,
		 * we can't do 3-wire and have MOSI in high-z with this device,
		 * so you'll have to use a gpio and external component to
		 * enable that */
		start = xfer->tx_buf
			? xfer->tx_buf + cmd->pos + cmd->pending_bytes
			: NULL;

		cmd->pending_unacked = len;
		cmd->pending_bytes  += len;
	} else {
buffer_full:
		len = 0;
		start = NULL;
	}

	mcp2210_init_msg(req, MCP2210_CMD_SPI_TRANSFER, len, 0, start, len,
			 true);

	if (len)
		mcp2210_debug("sending %u bytes", len);
	else
		mcp2210_debug("requesting final data");

	mcp2210_debug("len: %u, cmd->pending_bytes: %hu\n", len,
		      cmd->pending_bytes);
	dev->spi_in_flight = 1;
	cmd->spi_in_flight = 1;
	cmd->head.can_retry = 0;

	/* FIXME: dump message params */
	return 0;
}

static void spi_complete_ctl_cmd(struct mcp2210_cmd_spi_msg *cmd)
{
	struct mcp2210_device *dev = cmd->head.dev;
	struct mcp2210_cmd *cc = &cmd->ctl_cmd->head;

	/* always returns zero, so ignoring return value */
	cc->type->complete_urb(cc);

	if (IS_ENABLED(CONFIG_MCP2210_DEBUG) && dump_cmds) {
		mcp2210_debug("----CONTROL COMMAND RESPONSED----");

		cc->state = MCP2210_STATE_COMPLETE;
		/* get the dump function to print the response */
		mcp2210_debug("dev->s.cur_spi_config = %d",
				dev->s.cur_spi_config);
		dump_spi_xfer_settings(KERN_INFO, 0, "spi settings now: ",
				       &dev->s.spi_settings);
	}

	cmd->ctl_cmd = NULL;
}

static int spi_complete_urb(struct mcp2210_cmd *cmd_head)
{
	struct mcp2210_cmd_spi_msg *cmd = (void *)cmd_head;
	struct mcp2210_device *dev = cmd_head->dev;
	struct mcp2210_msg *rep;
	const struct mcp2210_pin_config_spi *cfg;
	unsigned long now = jiffies;
	struct spi_transfer *xfer;
//	long urb_duration = jiffdiff(now, dev->eps[EP_OUT].submit_time);
//	int bytes_pending;
	long expected_time_usec = 0;
	u8 pin = cmd->spi->chip_select;
	u8 len;

	if (IS_ENABLED(CONFIG_MCP2210_DEBUG)) {
		BUG_ON(!cmd_head->dev);
		BUG_ON(pin > 8);
		BUG_ON(dev->config->pins[pin].mode != MCP2210_PIN_SPI);
	}

	mcp2210_debug();

	if(cmd->ctl_cmd) {
		spi_complete_ctl_cmd(cmd);
		return -EAGAIN;
	}

	rep = dev->eps[EP_IN].buffer;
	len = rep->head.rep.spi.size;
	cfg = &dev->config->pins[pin].spi;


	/* len is the number of bytes successfully txed and rxed to/from the
	 * SPI peripheral */
	if (cmd->pending_bytes < len) {
		mcp2210_err("Device returned (and transmitted) more bytes "
			    "than expected.");
		return -EOVERFLOW;
	}

	cmd->pending_bytes -= len;
	cmd->pending_unacked = 0;

	mcp2210_debug("pin %hhu\n", pin);
	//mcp2210_debug("len: %hhu, cmd->pending_bytes: %hu, URB duration %uus (%lu jiffies)\n", len, cmd->pending_bytes, jiffies_to_usecs(urb_duration), urb_duration);
	//dump_msg(cmd->head.dev, KERN_DEBUG, "SPI response: ", rep);

	xfer = cmd->xfer;
	/* See if there is data to receive */
	if (len) {
#if 0
		/* bounds check first */
		if (unlikely(cmd->pos + len > xfer->len)) {
			mcp2210_err("received more data from device than "
				    "expected.");
			return -EOVERFLOW;
		}
#endif

		if(xfer->rx_buf)
			memcpy(xfer->rx_buf + cmd->pos, rep->body.raw,
			       len);

		cmd->pos += len;
		cmd->msg->actual_length += len;
	}

	/* check if xfer is finished */
	if(cmd->pos == xfer->len) {
		struct list_head *next = xfer->transfer_list.next;

		BUG_ON(cmd->pending_bytes != 0); /* sanity check */

		/* Transfer is done move next */
		mcp2210_info("%u byte xfer complete (all rx bytes read)",
			     xfer->len);

		if (IS_ENABLED(CONFIG_MCP2210_DEBUG) && dump_spi) {
			char msg_prefix[32];
			snprintf(msg_prefix, sizeof(msg_prefix), "%s <--: ",
				 dev_name(&cmd->spi->dev));
			if (xfer->rx_buf)
				print_hex_dump(KERN_INFO, msg_prefix,
					       DUMP_PREFIX_OFFSET, 16, 1,
					       xfer->rx_buf, xfer->len, true);
			else
				printk(KERN_INFO "%s(rx_buf is NULL)",
				       msg_prefix);
		}

		/* if cs_change, then clearing cmd->spi_in_flight will trigger
		 * a re-check of spi transfer settings in the next call to
		 * spi_submit_prepare() */
		if (xfer->cs_change)
			dev->spi_in_flight = cmd->spi_in_flight = 0;

		if (list_is_last(&xfer->transfer_list,
				 &cmd->msg->transfers)) {
			mcp2210_info("Command complete");
			dev->spi_in_flight = cmd->spi_in_flight = 0;
			cmd->msg->status = cmd->head.status = 0;
			return 0;
		}

		mcp2210_info("Starting next spi_transfer...");
		cmd->xfer = list_entry(next, struct spi_transfer,
				  transfer_list);
		cmd->pos = 0;
		cmd->pending_bytes = 0;
		cmd->busy_count = 0;
		cmd->ctl_cmd = NULL;

		/* honor delay between xfers here */
		expected_time_usec = 100ul * cfg->delay_between_xfers;

	} else if (cmd->pending_bytes >= pending_bytes_wait_threshold) {
		/* if the MCP2210's tiny buffer has at least
		 * pending_bytes_wait_threshold bytes in it then we'll give it
		 * some more time before sneding more */

		mcp2210_debug("cmd->pending_bytes: %u @ %uHz\n", cmd->pending_bytes, dev->s.spi_settings.bitrate);
		expected_time_usec = cmd->pending_bytes
				   * dev->s.spi_delay_per_kb / 1024ul;

		/* Account for last byte to CS delay if applicable */
		if (xfer->cs_change && list_is_last(
						&xfer->transfer_list,
						&cmd->msg->transfers)) {
			expected_time_usec += 100ul * cfg->last_byte_to_cs_delay;
 		}

 		/* We only get this at the start of a transfer, so this should
		 * be correct */
		if (rep->head.rep.spi.spi_status == 0x20)
			expected_time_usec += 100ul * cfg->cs_to_data_delay;
	}

	mcp2210_debug("expected_time_usec: %lu (%lu jiffies)\n",
		      expected_time_usec, usecs_to_jiffies(expected_time_usec));

	if (expected_time_usec) {
		cmd->head.delay_until = now + usecs_to_jiffies(expected_time_usec);
		cmd->head.delayed = 1;
	} else
		cmd->head.delayed = 0;

	return -EAGAIN;
}

static int spi_mcp_error(struct mcp2210_cmd *cmd_head)
{
	struct mcp2210_cmd_spi_msg *cmd = (void *)cmd_head;
	struct mcp2210_device *dev = cmd->head.dev;

	/* remove the rejected bytes from the in-process count */
	BUG_ON(cmd->pending_unacked > cmd->pending_bytes);
	cmd->pending_bytes -= cmd->pending_unacked;
	cmd->pending_unacked = 0;

	if (cmd->head.mcp_status == MCP2210_STATUS_BUSY) {
		mcp2210_warn("cmd->busy_count %u\n", cmd->busy_count);

		++cmd->busy_count;
		if (cmd->busy_count < 64) {
			/* hmm, hopefully shoudn't happen */
			/* FIXME: tweak this somehow */
			cmd->head.delay_until = jiffies + usecs_to_jiffies(750);
			cmd->head.delayed = 1;
			return -EAGAIN;
		} else
			return -EBUSY;
	}
	mcp2210_err("Unexpected return value from MCP2210: 0x%02hhx",
		    cmd->head.mcp_status);
	return -EIO;
}

static int spi_complete_cmd(struct mcp2210_cmd *cmd_head, void *context)
{
	struct mcp2210_cmd_spi_msg *cmd = (void*)cmd_head;
	struct spi_message *msg = cmd->msg;

	msg->status = cmd->head.status;

#ifndef USE_SPI_QUEUE
	if (msg->complete)
		msg->complete(msg->context);
#else
	spi_finalize_current_message(cmd->head.dev->spi_master);
#endif
	return 0;
}

#endif /* CONFIG_MCP2210_SPI */
