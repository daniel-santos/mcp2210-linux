/*
 *  MCP2210 driver for linux
 *
 *  Copyright (c) 2013-2017 Daniel Santos <daniel.santos@pobox.com>
 *                2013 Mathew King <mking@trilithic.com> for Trilithic, Inc
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
 *
 */

#ifndef _MCP2210_H
#define _MCP2210_H

#ifndef __GNUC__
# error mcp2210 driver only supports gcc as compiler
#endif

#ifndef __KERNEL__
# include "out-of-tree-autoconf.h"
#endif

#ifdef __KERNEL__
# include <linux/kernel.h>
# include <linux/version.h>
# include <linux/types.h>
# include <linux/device.h>
# include <linux/mutex.h>
# include <linux/module.h>
# include <linux/bug.h>
# include <linux/usb.h>
# include <linux/spi/spi.h>
# include <linux/gpio.h>
# include <linux/timer.h>
# include <linux/jiffies.h>

# if LINUX_VERSION_CODE >= KERNEL_VERSION(3,1,0)
#  include <linux/kconfig.h>
# endif

/* BUILD_BUG_ON broken somewhere prior to 3.0 */
# if LINUX_VERSION_CODE < KERNEL_VERSION(3,0,0) && defined(BUILD_BUG_ON)
#  undef BUILD_BUG_ON
# endif

/* spin_is_locked broken somewhere prior to 2.6.34 */
# include <linux/spinlock.h>
# if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,34) && defined(spin_is_locked)
#  undef spin_is_locked
# endif

#else
# include <stdint.h>
# include <stddef.h>
# include <stdlib.h>
# include <stdio.h>
# include <string.h>
# include <errno.h>
# include <sys/types.h>
# include <sys/ioctl.h>
# include <assert.h>
#endif /* __KERNEL__ */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* hack for IS_ENABLED macro for both userspace and pre 3.1 kernels */
#ifndef IS_ENABLED
# define __ARG_PLACEHOLDER_1 0,
# define config_enabled(cfg) _config_enabled(cfg)
# define _config_enabled(value) __config_enabled(__ARG_PLACEHOLDER_##value)
# define __config_enabled(arg1_or_junk) ___config_enabled(arg1_or_junk 1, 0)
# define ___config_enabled(__ignored, val, ...) val
# define IS_ENABLED(option) \
	(config_enabled(option) || config_enabled(option##_MODULE))
#endif /* IS_ENABLED */


/* if no good BUILD_BUG_ON() then just use BUG_BUG */
#ifndef BUILD_BUG_ON
# define BUILD_BUG_ON(cond) BUG_ON(cond)
#endif

#ifndef spin_is_locked
# define spin_is_locked(spinlock) 0
#endif

#ifndef GCC_VERSION
# define GCC_VERSION (__GNUC__ * 10000 \
		    + __GNUC_MINOR__ * 100 \
		    + __GNUC_PATCHLEVEL__)
#endif

#ifndef __compiletime_warning
# if GCC_VERSION  >= 40300
#  define __compiletime_warning(msg) __attribute__((warning(msg)))
# else
#  define __compiletime_warning(msg)
# endif
#endif

#ifndef __KERNEL__
/* macros, typedefs & hacks for userspace compliation */
# define BUG()			assert(0)
# define BUG_ON(cond)		assert(!(cond))
# define EXPORT_SYMBOL_GPL(symbol)
# define le32_to_cpu(v)		(v)
# define cpu_to_le32(v)		(v)
# define printk(fmt, ...)	fprintf(stderr, fmt, ##__VA_ARGS__)
# define KERN_ERR		""
# define KERN_WARNING		""
# define KERN_INFO		""
# define KERN_DEBUG		""
# define GFP_KERNEL		0
# define likely(cond)		(cond)
# define unlikely(cond)		(cond)
# define IS_ERR_VALUE(x) unlikely((unsigned long)(x) >= (unsigned long)-200)
# ifndef __always_inline
#  define __always_inline	inline __attribute__((always_inline))
# endif
	/* userspace programs need a few typedefs */
	typedef int8_t   s8;
	typedef int16_t  s16;
	typedef int32_t  s32;
	typedef int64_t  s64;
	typedef uint8_t  u8;
	typedef uint16_t u16;
	typedef uint32_t u32;
	typedef uint64_t u64;
#ifndef __cplusplus
	typedef _Bool    bool;
#endif

	typedef unsigned gfp_t;

/* more userspace hacks */


static inline void *kzalloc(size_t size, unsigned flags) {
	void *ret = malloc(size);
	if (ret)
		memset(ret, 0, size);
	return ret;
}

static inline void *kmalloc(size_t size, unsigned flags){return malloc(size);}
static inline void kfree(void *ptr)			{free(ptr);}
static inline void *ERR_PTR(long error)			{return (void *) error;}
static inline long PTR_ERR(const void *ptr)		{return (long) ptr;}
static inline long IS_ERR(const void *ptr)		{return IS_ERR_VALUE((unsigned long)ptr);}
static inline long IS_ERR_OR_NULL(const void *ptr)	{return !ptr || IS_ERR_VALUE((unsigned long)ptr);}

#endif /* __KERNEL__ */

#define USB_VENDOR_ID_MICROCHIP	0x04d8
#define USB_DEVICE_ID_MCP2210	0x00de

#define MCP2210_BUFFER_SIZE	64
#define MCP2210_MAX_SPEED	(12 * 1000 * 1000)
#define MCP2210_MIN_SPEED	1500
#define MCP2210_EEPROM_SIZE	256
#define MCP2210_NUM_PINS	9

static const char * const urb_dir_str[] = {"out", "in"};


/****************************************************************************
 * enums
 */


/**
 * enum mcp2210_cmd_code - valid command codes for MCP2210
 *
 * Listed in order of their appearance in the datasheet with section numbers in
 * comments.
 */
enum mcp2210_cmd_code {
	MCP2210_CMD_SET_NVRAM		= 0x60,	/* 3.1.1 - 3.1.5 */
	MCP2210_CMD_GET_NVRAM		= 0x61,	/* 3.1.6 - 3.1.10 */
	MCP2210_CMD_SEND_PASSWD		= 0x70,	/* 3.1.11 */
	MCP2210_CMD_GET_SPI_CONFIG	= 0x41,	/* 3.2.1 */
	MCP2210_CMD_SET_SPI_CONFIG	= 0x40,	/* 3.2.2 */
	MCP2210_CMD_GET_CHIP_CONFIG	= 0x20,	/* 3.2.3 */
	MCP2210_CMD_SET_CHIP_CONFIG	= 0x21,	/* 3.2.4 */
	MCP2210_CMD_GET_PIN_DIR		= 0x33,	/* 3.2.5 */
	MCP2210_CMD_SET_PIN_DIR		= 0x32,	/* 3.2.6 */
	MCP2210_CMD_GET_PIN_VALUE	= 0x31,	/* 3.2.7 */
	MCP2210_CMD_SET_PIN_VALUE	= 0x30,	/* 3.2.8 */
	MCP2210_CMD_READ_EEPROM		= 0x50,	/* 3.3.1 */
	MCP2210_CMD_WRITE_EEPROM	= 0x51,	/* 3.3.2 */
	MCP2210_CMD_GET_INTERRUPTS	= 0x12,	/* 3.4.1 */
	MCP2210_CMD_SPI_TRANSFER	= 0x42,	/* 3.5.1 */
	MCP2210_CMD_SPI_CANCEL		= 0x11,	/* 3.5.2 */
	MCP2210_CMD_SPI_RELEASE		= 0x80,	/* 3.5.3 */
	MCP2210_CMD_GET_STATUS		= 0x10	/* 3.6.1 */
};

/**
 * enum mcp2210_nvram_subcmd_code - valid subcommand codes for
 *
 */
enum mcp2210_nvram_subcmd_code {
	MCP2210_NVRAM_CHIP		= 0x20,
	MCP2210_NVRAM_SPI		= 0x10,
	MCP2210_NVRAM_KEY_PARAMS	= 0x30,
	MCP2210_NVRAM_MFG		= 0x50,
	MCP2210_NVRAM_PROD		= 0x40
};

/**
 * enum mcp2210_status - status codes retruned by MCP2210
 *
 * @MCP2210_STATUS_SUCCESS:	  No error
 * @MCP2210_STATUS_SPI_NOT_OWNED: The SPI bus is not currently owned
 * @MCP2210_STATUS_BUSY:	  Either the SPI bus or USB interface is busy.
 * @MCP2210_STATUS_UNKNOWN_CMD:	  You sent an invalid command code.
 * @MCP2210_STATUS_WRITE_FAIL:	  Write to EEPROM failed (uh-oh)
 * @MCP2210_STATUS_NO_ACCESS:	  Either the device is permanently locked or is
 * 				  password-protected and you haven't supplied
 * 				  the magic word.  If you get this with
 * 				  MCP2210_CMD_SEND_PASSWD it just means that
 * 				  you've tried too many times and it wont let
 * 				  you try again until you power cycle the
 * 				  device.
 * @MCP2210_STATUS_PERM_LOCKED:	  The device is permemently locked.
 * @MCP2210_STATUS_BAD_PASSWD:	  You've given a bad password, but can try
 * 				  again.
 */
enum mcp2210_status {
	MCP2210_STATUS_SUCCESS		= 0x00,
	MCP2210_STATUS_SPI_NOT_OWNED	= 0xf7,
	MCP2210_STATUS_BUSY		= 0xf8,
	MCP2210_STATUS_UNKNOWN_CMD	= 0xf9,
	MCP2210_STATUS_WRITE_FAIL	= 0xfa,
	MCP2210_STATUS_NO_ACCESS	= 0xfb,
	MCP2210_STATUS_PERM_LOCKED	= 0xfc,
	MCP2210_STATUS_BAD_PASSWD	= 0xfd,
};

enum mcp2210_pin_mode {
	MCP2210_PIN_GPIO	= 0,
	MCP2210_PIN_SPI		= 1,
	MCP2210_PIN_DEDICATED	= 2,
};

/* Sadly, the datasheet contains no documentation for the meaning of this bit
 * and had to be discovered by trial and error. Zero is out and one is in.
 */
enum mcp2210_gpio_direction {
	MCP2210_GPIO_NO_CHANGE	= -1,
	MCP2210_GPIO_OUTPUT	= 0,
	MCP2210_GPIO_INPUT	= 1
};

/**
 * enum mcp2210_other_settings
 *
 * Represents byte 17 of chip settings message (section 3.1.1, 3.1.2, etc)
 *
 * These values are generally ORed together except that only one
 * MCP2210_INTERRUPT_* value may be chosen (they do not OR together). To disable
 * any of these options, exclude them (zero is "disabled" for all options).
 * See table 3-1 in the datasheet for more information.
 */
enum mcp2210_other_settings {
	MCP2210_SPI_BUS_RELEASE_DISABLED = 0x01,
	MCP2210_INTERRUPT_HIGH_PULSE	 = 0x08,
	MCP2210_INTERRUPT_LOW_PULSE	 = 0x06,
	MCP2210_INTERRUPT_RISING_EDGE	 = 0x04,
	MCP2210_INTERRUPT_FALLING_EDGE	 = 0x02,
	MCP2210_REMOTE_WAKEUP_ENABLED	 = 0x80,
};

enum mcp2210_eeprom_status {
	MCP2210_EEPROM_UNREAD	    = 0,
	MCP2210_EEPROM_READ_PENDING = 1,
	MCP2210_EEPROM_READ	    = 2,
	MCP2210_EEPROM_DIRTY	    = 3
};

enum mcp2210_cmd_type_id {
	MCP2210_CMD_TYPE_CTL,
	MCP2210_CMD_TYPE_SPI,
	MCP2210_CMD_TYPE_EEPROM,
	MCP2210_CMD_TYPE_MAX
};

/**
 * enum mcp2210_state - represents the state of both URBs and commands
 */
enum mcp2210_urb_cmd_state {
	MCP2210_STATE_NEW,
	MCP2210_STATE_SUBMITTED,
	MCP2210_STATE_COMPLETE,
	MCP2210_STATE_DEAD
};

enum mcp2210_endpoints {
	EP_OUT = 0,
	EP_IN  = 1
};

/******************************************************************************
 * Packed structs
 *
 * These structs represent portions of the MCP2210's wire protocol. Endianness
 * is set to little prior to transmission in ctl_submit_prepare() and back to
 * the native endianess in ctl_complete_urb().
 */

#if 0
#define STRUCT_PREFIX mcp2210
#include "mcp2210-structs.h"
#undef STRUCT_PREFIX

#define STRUCT_PREFIX packed_mcp2210
#pragma pack(1)
#include "mcp2210-structs.h"
#pragma pack()
#endif

/**
 * mcp2210_msg - A USB message sent to or received from an MCP2210
 *
 * @body.chip	Body for "Chip Settings" (3.1.7, 3.1.1, 3.2.3, and 3.2.4).
 * @body.gpio	Body for "GPIO Current Pin Direction" (3.2.5, 3.2.6) and "GIPO
 * 		Current Pin Value" (3.2.7, 3.2.8)
 * @body.spi		Body for "SPI Transfer Settings" (3.1.6, 3.1.2, 3.1.2, and
 * 		3.2.2).
 * @body.get_usb_params	Body for "USB Power-Up Key Parameters" (3.1.8, 3.1.3)
 * @body.set_usb_params	Body for "USB Power-Up Key Parameters" (3.1.8, 3.1.3)
 * @body.usb_string	Body for "USB Manufacturer Name" and "USB Product Name" (3.1.9,
 * 		3.1.4, 3.1.10, 3.1.5)
 * @body.password	Body for "Send Access Password" (3.1.11)
 * @body.data	Body for SPI data tx & rx
 *
 * The entire mcp2210_msg should be zeroed prior to populating fields. Prior to
 * submitting endianness of all fields (over one byte in size) should be'
 * converted from CPU to little endian and after receiving, the reverse.
 *
 * @see http://ww1.microchip.com/downloads/en/DeviceDoc/22288A.pdf
 *
 */
#pragma pack(1)
struct mcp2210_msg {
	u8 cmd;

	union {
		/* Request headers */
		union {
			/* Request header for all "normal" set and get commands
			 * (excepting MCP2210_CMD_GET_INTERRUPTS and
			 * MCP2210_CMD_GET_STATUS) */
			struct {
				u8 sub_cmd;
				u16 reserved;
			} xet;

			/* Request header for reads & writes to EEPROM */
			struct {
				u8 addr;
				u8 value;
				u8 reserved;
			} eeprom;

			/* Request header for SPI operations */
			struct {
				u8 size;
				u16 reserved;
			} spi;

			/* External Interrupt Pin (GP6) Event Status */
			struct {
				u8 reset;
				u16 reserved;
			} intr;
		} req;

		/* Response headers */
		union {
			u8 status;

			/* Request header for all "normal" set and get commands
			 * (excepting MCP2210_CMD_GET_INTERRUPTS and
			 * MCP2210_CMD_GET_STATUS) */
			struct {
				u8 status;
				u8 sub_cmd;
				u8 reserved;
			} xet;

			/* Response header for reads & writes to EEPROM */
			struct {
				u8 status;
				u8 addr;
				u8 value;
			} eeprom;

			/* Response header for SPI messages */
			struct {
				u8 status;
				u8 size;
				u8 spi_status;
			} spi;

			/* Response header for SPI cancel & status */
			struct {
				u8 status;
				u8 release_external_req_status;
				u8 current_bus_owner;
			} spi_status;
		} rep;

		u8 raw[3];
	} head;

/**
 * struct mcp2210_msg_body
 *
 * See section 3.1.1 of datasheet for full details.
 *
 * @pin_mode:		  MCP2210_PIN_GPIO, _SPI or _DEDICATED for each pin.
 * @gpio_value:		  Bits 0-8 specify the value of GPIO pins (bits are
 * 			  ignored when a pin is not configured for GPIO).
 * @gpio_direction:	  Bits 0-8 specify the direction of GPIO pins (0 = out,
 * 			  1 = in).
 * @other_settings	  Use enum mcp2210_other_settings values ORed together,
 * 			  but do not OR together any MCP_INTERRUPT_ values with
 * 			  each other.
 * @nvram_access_control: 0 for no protection, 0x40 for password protection,
 * 			  0x80 to PERMANENTLY lock the chip settings.
 * @password:		  8 byte password.
 */
	union mcp2210_msg_body {
		struct mcp2210_chip_settings {
			u8 pin_mode[MCP2210_NUM_PINS];
			u16 gpio_value;
			u16 gpio_direction;
			u8 other_settings;
			u8 nvram_access_control;
			u8 password[8];
			/* u8 reserved[37]; */
		} chip;

		u16 gpio;

/**
 * struct mcp2210_spi_xfer_settings
 *
 * @bitrate:		   Bits per second.  A valid value is between 1500
 * 			   (0x5dc) and 12,000,000 (0xb71b00).
 * @idle_cs:		   The idle chip select values for pins 0-8:
 * 			   x x x x x x x 8  7 6 5 4 3 2 1 0
 * @active_cs:		   The active chip select values for pins 0-8:
 * 			   x x x x x x x 8  7 6 5 4 3 2 1 0
 * @delay_cs_to_data:	   Delay between CS assertion and first data byte in
 * 			   multiples of 100 uS (from zero to 6.5536 seconds).
 * @delay_last_byte_to_cs: Delay between last byte and CS de-assertion in
 * 			   multiples of 100 uS.
 * @delay_between_bytes:   Delay between data bytes in multiples of 100 uS.
 * @bytes_per_trans:	   Number of bytes to transfer per-transaction.
 * @mode:		   SPI Mode 0-3.
 *
 * When representing the values for SPI communications (either sending or
 * receiving the SPI transfer settings), the idle_cs and active_cs are as
 * advertised -- what the values of each pin should be when inactive and
 * active.
 */
		struct mcp2210_spi_xfer_settings {
			u32 bitrate;
			u16 idle_cs;
			u16 active_cs;
			u16 cs_to_data_delay;
			u16 last_byte_to_cs_delay;
			u16 delay_between_bytes;
			u16 bytes_per_trans;
			u8 mode;
			/* u8 reserved[43]; */
		} spi;

/**
 * struct mcp2210_usb_key_params
 *
 * See section 3.1.3 of datasheet for full details.
 *
 * @vid:		Vendor ID
 * @pid:		Product ID
 * @chip_power_option:	10xxxxxx Host-powered
 * 			01xxxxxx Self-powered
 * 			xx1xxxxx Remote wake-up capable
 * 			xxx00000 Lower 5 bits should always be zero
 * @requested_power:	Requested amount of power from USB host in quanta of
 * 			2mA.
 */
		struct mcp2210_usb_key_params {
			u16 vid;
			u16 pid;
			u8 chip_power_option;
			u8 requested_power;
			/* u8 reserved[54]; */
		} set_usb_params;

		u8 password[8];
		u16 interrupt_event_counter;
		u8 raw[60];

		struct {
			u8 reserved0[8];
			u16 vid;
			u16 pid;
			u8 reserved1[13];
			u8 chip_power_option;
			u8 requested_power;
			/* u8 reserved2[33]; */
		} get_usb_params;

		struct {
			u8 length;
			u8 id;
			u8 data[58];
		} usb_string;

		struct {
			u8 num_pwd_guesses;
			u8 pwd_guessed;
		} spi_status;

	} body;

};
#pragma pack()

/**
 * mcp2210_pin_config - Configuration for a single multi-purpose pin output
 *
 * @mode:	  How this pin will be used.  Should be one of
 * 		  enum mcp2210_pin_mode: MCP2210_PIN_GPIO, _SPI or _DEDICATED.
 * @has_irq:	  See description below.
 * @irq:	  The IRQ number relative to the chip's base.
 * @irq_threaded: One for a threaded irq.
 * @irq_type:	  See description below.
 * @spi:	  SPI settings, if this pin is configured for SPI.
 * @name:	  SPI: The part number of the device connected to this PIN.
 * @modalias: 	  SPI: A more descriptive name of the device
 *
 * The MCP2210 has 9 pins that can be used for either General Purpose I/O
 * (GPIO), SPI Chip Select (CS) or some dedicated function specific to the pin.
 * This struct represents the hardware configuration of that pin as well as
 * information about how this pin is translated into virtual interrupts.
 *
 * The meaning of the has_irq bit depends upon the mode of the pin.
 * If the pins mode is dedicated or gpio then has_irq tells the driver
 * to generate interrups. If the mode is spi, then it tells the driver
 * that the spi device's driver will consume an interrupt generated by this
 * this device. (There is curently no support for configuring it
 * to consume interrupts generated elsewhere.) Note that has_irq is only valid
 * for pin 6 in the dedicated mode.
 *
 * If has_irq is set on either the dedicated GP6 or gpio pin, then the field
 * .irq specifies the interrupt number relative to the chip's base and the field
 * .irq_type is ignored. If the
 * pin is GP6 in dedicated mode then the interrupt trigger type is configured
 * via struct mcp2210_chip_settings::other_settings as described in section
 * 3.1.2 of the product datasheet. If the pin type is gpio, then the field
 * .irq_type is set to a bitmask of IRQ_TYPE_ values (see include/linux/irq.h
 * from the kernel tree). If the type of the pin is spi, then the field .irq_type
 * is also ignored (is this the right way to do this?)
 */
struct mcp2210_pin_config {
	u8 mode:2;
	u8 has_irq:1;
	u8 irq:3;
	u8 irq_threaded:1;
	u8 reserved:1;
	u8 irq_type;

/**
 * struct mcp2210_pin_config_spi
 *
 * @max_speed_hz:	SPI: Max bits per second (must be 1500-12000000)
 * @max_speed_hz:	SPI: Max bits per second (must be 1500-12000000)
 * @mode: 		the struct spi_device mode
 * @manual_cs:		chip select will be managed vi gpio
 * @bits_per_word:	better be 8
 * @use_cs_gpio:	Use a gpio for chip select insetad of the device's
 * 			built-in mechanism
 * @cs_gpio:		Only when use_cs_gpio is set.  The gpio (offset on this
 * 			MCP2210) to use for chip select.
 * @cs_to_data_delay	SPI: Chip select to data start delay as a quanta of
 * 			100 us (i.e., in hundreds of microseconds)
 * @last_byte_to_cs_delay SPI: Delay between last byte of data and CS
 * 			deactivation (x 100us)
 * @delay_between_bytes	SPI: Delay between data bytes (x 100us)
 * @delay_between_xfers Delay between transfers (x 100us)
 */
	struct mcp2210_pin_config_spi {
		u32 max_speed_hz;
		u32 min_speed_hz;
		u8 mode;
		u8 bits_per_word;
		u8 use_cs_gpio:1;
		u8 cs_gpio;
		u16 cs_to_data_delay;
		u16 last_byte_to_cs_delay;
		u16 delay_between_bytes;
		u16 delay_between_xfers;
	} spi;

	const char *name;
	const char *modalias;
	/*const char *desc; */
};

/**
 * struct mcp2210_board_config
 *
 * @dev_name:
 * @pins:		Configuration for each pin.
 * @poll_gpio_usecs:	The interval for polling GPIO state in microseconds or
 * 			zero if no polling is desired.
 * @stale_gpio_usecs:	Should be zero if poll_gpio_usecs is zero.  Otherwise,
 * 			the number of microseconds that a cached GPIO value
 * 			will be used.  If a call is made to gpio_get_value()
 * 			when a cached value is older than this, the call will
 * 			block while the device is queried for an update.
 * @poll_intr_usecs:	The interval for polling for interrupts from GP6 in its
 * 			dedicated mode or zero if no polling is desired.
 * @stale_intr_usecs:	See stale_gpio_usecs
 * @_3wire_capable:
 * @_3wire_tx_enable_active_high:
 * @_3wire_tx_enable_pin:
 * @strings_size:
 */
struct mcp2210_board_config {
	const char *dev_name;
	struct mcp2210_pin_config pins[MCP2210_NUM_PINS];
	u32 poll_gpio_usecs;
	u32 stale_gpio_usecs;
	u32 poll_intr_usecs;
	u32 stale_intr_usecs;
	u8 _3wire_capable:1;
	u8 _3wire_tx_enable_active_high:1;
	u8 _3wire_tx_enable_pin;
	size_t strings_size;
#ifndef __cplusplus
	/* const invokes gcc bug #57977 */
	const
#endif
	char strings[0];
};

/**
 * struct mcp2210_state - represents the state of the (hardware) device
 *
 * @have_chip_settings:		If chip_settings is populated
 * @have_power_up_chip_settings:If power_up_chip_settings is populated
 * @have_spi_settings:		If spi_settings is populated
 * @have_power_up_spi_settings:	If power_up_spi_settings is populated
 * @have_usb_key_params:	(self explanitory)
 * @chip_settings:
 * @power_up_chip_settings:
 * @spi_settings:
 * @power_up_spi_settings:
 * @usb_key_params:
 * @cur_spi_config:		Index specifying the CS pin of the SPI
 * 				peripheral the device is currently configured
 * 				to communicate with or -1 if none.
 * @idle_cs:			The current idle chip-select value
 * @active_cs:			The current active chip-select value
 * @spi_delay_per_kb:		The number of microseconds we would expect the
 * 				device to transmit a 1kB chunk of data to the
 * 				current SPI peripheral if it had a 1kB buffer.
 */
struct mcp2210_state {
	u8 have_chip_settings:1;
	u8 have_power_up_chip_settings:1;
	u8 have_spi_settings:1;
	u8 have_power_up_spi_settings:1;
	u8 have_usb_key_params:1;
//	u8 chip_settings_dirty:1;
	struct mcp2210_chip_settings chip_settings;
	struct mcp2210_chip_settings power_up_chip_settings;
	struct mcp2210_spi_xfer_settings spi_settings;
	struct mcp2210_spi_xfer_settings power_up_spi_settings;
	struct mcp2210_usb_key_params usb_key_params;
	int cur_spi_config;
	u16 idle_cs;
	u16 active_cs;
	unsigned long spi_delay_per_kb;
	//u16 gpio;
};


#ifdef __KERNEL__

/* module parameters */
extern int debug_level;
extern int creek_enabled;
extern int dump_urbs;
extern int dump_cmds;
extern int dump_spi;
extern uint pending_bytes_wait_threshold;
extern uint poll_delay_warn_secs;


struct mcp2210_cmd;
struct mcp2210_cmd_type {
	enum mcp2210_cmd_type_id id;
	int (*submit_prepare)(struct mcp2210_cmd *cmd_head);
	int (*complete_urb)(struct mcp2210_cmd *cmd_head);
	int (*mcp_error)(struct mcp2210_cmd *cmd_head);
	void (*dump)(const char *level, unsigned indent, const char *start,
		     const struct mcp2210_cmd *cmd_head);
	const char *desc;
};

typedef int (*mcp2210_complete_t)(struct mcp2210_cmd *cmd, void *context);


/** struct mcp2210_cmd
 * @dev:
 * @type:		The type of command or NULL if this is a general-
 * 			purpose (non-USB transaction) command that will execute
 * 			when it complete() is called.
 * @node:		private
 * @time_queued:	Time (in jiffies) command was queued
 * @time_started:	Time (in jiffies) command processing began
 * @delay_until:	Time (in jiffies) this command should be executed
 * 			(ignored unless delayed is set)
 * @status:		Status code (zero or a negative error)
 * @mcp_status:		The MCP status code (initially zero)
 * @state:		State of the command (enum mcp2210_urb_cmd_state)
 * @kill:		True if this command is being killed
 * @can_retry:		True if this command has no side-effects (like querying
 * 			the chip's settings) and can be retried if the USB
 * 			transaction fails due to IO errors or shitty drivers.
 * @delayed:		True if this delay_until should be respected.
 * @nonatomic:		True if this command must be executed in a non-atomic
 * 			context.
 * @repeat_count:	Number of times this command has been repeated.
 * @complete:		A complete function
 * @context:		The complete function's context
 *
 */
struct mcp2210_cmd {
	struct mcp2210_device *dev;
	const struct mcp2210_cmd_type *type;
	struct list_head node;
	unsigned long time_queued;
	unsigned long time_started;
	unsigned long delay_until;
	int status;
	u8 mcp_status;
	u8 state:2;
	u8 can_retry:1;
	u8 delayed:1;
	u8 nonatomic:1;
	u8 non_preemptable:1;
	uint repeat_count;
	mcp2210_complete_t complete;
	void *context;
	u8 data[0];
};

struct mcp2210_cmd_ctl {
	struct mcp2210_cmd head;
	struct mcp2210_msg req;
	u8 pin;
	u8 is_mcp_endianness:1;
};

/**
 * struct mcp2210_cmd_spi_msg -
 * @spi:		The spi_device
 * @msg:		The spi_message
 * @xfer:		The current spi_transfer
 * @pos:		The number of bytes successfully xfered to/from the
 * 			peripheral.
 * @pending_bytes:	The number of bytes sent to the MCP2210 (acked and
 * 			unacked), but not yet transferred to the SPI device.
 * @pending_unacked:	The number of un-acked bytes sent to the MCP2210.
 * @busy_count:		The number of times the device has given us 0xf8
 * 			sending this message
 * @spi_in_flight:	Set if this command is doing an SPI transfer (not a
 * 			dupliate of from dev->spi_in_flight!)
 * @ctl_cmd:		A pointer to dev->ctl_cmd if we are either changing the
 * 			SPI transfer settings for or canceling a previously
 * 			failed SPI message.
 */
struct mcp2210_cmd_spi_msg {
	struct mcp2210_cmd head;
	struct spi_device *spi;
	struct spi_message *msg;
	struct spi_transfer *xfer;
	uint pos;
	u16 pending_bytes;
	u16 pending_unacked;
	uint busy_count;
	u8 spi_in_flight:1;
	struct mcp2210_cmd_ctl *ctl_cmd;
};

struct mcp2210_cmd_eeprom {
	struct mcp2210_cmd head;
	u8 op;
	u8 addr;
	u8 zero_tail;
	u16 size;
};

union mcp2210_cmd_any {
	struct mcp2210_cmd *head;
	struct mcp2210_cmd_ctl *ctl;
	struct mcp2210_cmd_spi_msg *spi;
	struct mcp2210_cmd_eeprom *eeprom;
};


/**
 * struct mcp2210_endpoint - a struct for comm with an endpoint
 *
 * @ep:		 endpoint struct
 * @urb:	 The URB that is re-used for each request.
 * @buffer:	 Coherent DMA-able buffer
 * @submit_time: time URB was submitted (in jiffies)
 * @state:
 * @kill:	 non-zero if URB is being killed
 * @retry_count: used only with CONFIG_MCP2210_USB_QUIRKS
 * @is_dir_in:	The direction of this endpoint
 */
struct mcp2210_endpoint {
	const struct usb_host_endpoint *ep;
	struct urb *urb;
	struct mcp2210_msg *buffer;
	unsigned long submit_time;
	atomic_t unlink_in_process;
	u8 state;
	u8 is_mcp_endianness:1;
	u8 kill:1;
	u8 is_dir_in:1;
	u8 retry_count;
};

/**
 * struct mcp2210_device -
 *
 * @intf:
 * @udev:
 * @int_in_ep:
 * @int_out_ep:
 *
 * @io_mutex:		Lock for current USB I/O operations.  Lock for access to cur_cmd and reqest_buffer.
 * @cur_cmd:		The current command in-progress
 * @requeust_buffer:	rx/tx buffer
 *
 * @cmd_queue:		FIFO command queue
 * @cmd_queue_mutex:	mutex for manipulating cmd_queue
 *
 * @dead:		We've encountered an error and we're just giving up on everything (for now)....

 * @pi_in_flight:	An SPI message is in process (or even hung)
 * @ctl_cmd:		A re-usable control command struct (can be used by cur_cmd only)

 * @have_chip_settings:	@chip_settings accurately reflects the state of the device
 * @have_spi_settings:	@spi_settings accurately reflects the state of the device
 * @chip_settings:	Current chip settings (native endianness)
 * @spi_settings: 	The current SPI transfer settings with values stored in native endianness
 * @config:		(known) pin configuration data for this device
 * @cur_spi_config:	The current SPI device that we are configured to communicate with.  Should be 0-8 or -1 for none.
 * @idle_cs:		calculated value for when all chips are idle
 *
 * @eeprom_state:	A 2 bit map of the state of each byte in eeprom_cache. 00 = unread, 01 = read pending (unread), 10 = read, 11 = dirty (write pending).
 * @eeprom_cache:	Cached EEPROM values.
 *
 * Unfortunately, since reads & writes of the 256 bytes of user EEPROM are a
 * byte at a time, we have to track the state of each byte separately in
 * eeprom_state.
 *
 * Lock order:
 * dev_spinlock -> queue_spinlock
 */
struct mcp2210_device {
	struct usb_device *udev;
	struct usb_interface *intf;

	spinlock_t dev_spinlock;
	spinlock_t queue_spinlock;
#ifdef CONFIG_MCP2210_IOCTL
	struct mutex io_mutex;
#endif
	struct kref kref;
#ifdef CONFIG_MCP2210_DEBUG
	atomic_t manager_running;
#endif

	struct list_head cmd_queue;
	struct mcp2210_cmd *cur_cmd;
	struct list_head delayed_list;
	struct mcp2210_cmd *delayed_cmd;
	struct timer_list timer;
	struct mcp2210_endpoint eps[2];

	struct delayed_work delayed_work;

	int dead;
	u8 debug_chatter_count;
	u8 spi_in_flight:1;
	u8 have_config:1;
	u8 is_spi_probed:1;
	u8 is_gpio_probed:1;
	u8 is_irq_probed:1;
	u8 poll_intr:1;
	u8 poll_gpio:1;

	struct mcp2210_state s;
	struct mcp2210_board_config *config;
	struct mcp2210_cmd_ctl ctl_cmd;
#ifdef CONFIG_MCP2210_EEPROM
	spinlock_t eeprom_spinlock;
	int eeprom_retry_count;
	u8 eeprom_state[64];
	u8 eeprom_cache[256];
#endif
	const char *names[MCP2210_NUM_PINS];
#ifdef CONFIG_MCP2210_GPIO
	struct gpio_chip gpio;
#endif

#ifdef CONFIG_MCP2210_SPI
	struct spi_master *spi_master;
	struct spi_device *chips[MCP2210_NUM_PINS];
#else
	char spi_master; /* HACK: spi_master still referenced when disabeld */
#endif

#ifdef CONFIG_MCP2210_IRQ
	struct mutex irq_lock;
	uint nr_irqs;
	int irq_base;
	int irq_revmap[8];	/* not actually getting used, maybe remove */
	int irq_type[8];
	struct irq_desc *irq_descs[8];
	u8 irq_mask;
	unsigned long last_poll_gpio;
	unsigned long last_poll_intr;
	unsigned long suppress_poll_warn;
	u16 interrupt_event_counter;
# ifdef CONFIG_MCP2210_GPIO
	struct mcp2210_cmd_ctl cmd_poll_gpio;
# endif
	struct mcp2210_cmd_ctl cmd_poll_intr;
#endif /* CONFIG_MCP2210_IRQ */
};


/* exported functions */

/*****************************************************************************
 * mcp2210-core.c
 */
int mcp2210_update_settings(struct mcp2210_device *dev);
#define mcp2210_alloc_cmd_type(dev, type, fns, gfp_flags) \
	((type*) mcp2210_alloc_cmd((dev), fns, sizeof(type), gfp_flags))
struct mcp2210_cmd *mcp2210_alloc_cmd(struct mcp2210_device *dev,
				      const struct mcp2210_cmd_type *type,
				      size_t size, gfp_t gfp_flags);
int mcp2210_add_cmd(struct mcp2210_cmd *cmd, bool free_if_dead);
int process_commands(struct mcp2210_device *dev, const bool lock_held, const bool can_sleep);
int mcp2210_set_pin_config(struct mcp2210_device *dev, unsigned pin,
			   const struct mcp2210_pin_config *config);
int mcp2210_configure(struct mcp2210_device *dev,
		      struct mcp2210_board_config *new_config);


/*****************************************************************************
 * mcp2210-ctl.c
 */
void ctl_cmd_init(struct mcp2210_device *dev, struct mcp2210_cmd_ctl *cmd,
		u8 cmd_code, u8 subcmd_code, void *body, size_t body_size,
		u8 is_mcp_endianness);
struct mcp2210_cmd_ctl *mcp2210_alloc_ctl_cmd(struct mcp2210_device *dev,
		u8 cmd_code, u8 subcmd_code, void *body, size_t body_size,
		u8 is_mcp_endianness, gfp_t gfp_flags);
void calculate_spi_settings(struct mcp2210_spi_xfer_settings *dest,
			    const struct mcp2210_device *dev,
			    const struct spi_device *spi,
			    const struct spi_message *msg,
			    const struct spi_transfer *xfer, u8 pin);


/*****************************************************************************
 * mcp2210-ioctl.c
 */
#ifdef CONFIG_MCP2210_IOCTL
long mcp2210_ioctl(struct file *file, unsigned int request, unsigned long arg);
#endif


/*****************************************************************************
 * mcp2210-spi.c
 */
#ifdef CONFIG_MCP2210_SPI
int  mcp2210_spi_probe (struct mcp2210_device *dev);
void mcp2210_spi_remove(struct mcp2210_device *dev);
#else
static inline int  mcp2210_spi_probe (struct mcp2210_device *dev) {return 0;}
static inline void mcp2210_spi_remove(struct mcp2210_device *dev) {}
#endif /* CONFIG_MCP2210_SPI */


/*****************************************************************************
 * mcp2210-gpio.c
 */
#ifdef CONFIG_MCP2210_GPIO
int  mcp2210_gpio_probe (struct mcp2210_device *dev);
void mcp2210_gpio_remove(struct mcp2210_device *dev);
#else
static inline int  mcp2210_gpio_probe (struct mcp2210_device *dev) {return 0;}
static inline void mcp2210_gpio_remove(struct mcp2210_device *dev) {}
#endif /* CONFIG_MCP2210_GPIO */


/*****************************************************************************
 * mcp2210-eeprom.c
 */
#ifdef CONFIG_MCP2210_EEPROM
/* locks dev->eeprom_spinlock */
int
mcp2210_eeprom_read(struct mcp2210_device *dev, u8 *dest, u8 addr, u16 size,
		    mcp2210_complete_t complete, void *context,
		    gfp_t gfp_flags);
int
mcp2210_eeprom_write(struct mcp2210_device *dev, const u8 *src, u8 addr,
		     u16 size, mcp2210_complete_t complete, void *context,
		     gfp_t gfp_flags);
#else
static inline int
mcp2210_eeprom_read(struct mcp2210_device *dev, u8 *dest, u8 addr, u16 size,
		    mcp2210_complete_t complete, void *context,
		    gfp_t gfp_flags) {return 0;}
static inline int
mcp2210_eeprom_write(struct mcp2210_device *dev, const u8 *src, u8 addr,
		     u16 size, mcp2210_complete_t complete, void *context,
		     gfp_t gfp_flags) {return 0;}
#endif /* CONFIG_MCP2210_EEPROM */

/*****************************************************************************
 * mcp2210-irq.c
 */
#ifdef CONFIG_MCP2210_IRQ
int  mcp2210_irq_probe (struct mcp2210_device *dev);
void mcp2210_irq_disable(struct mcp2210_device *dev);
void mcp2210_irq_remove(struct mcp2210_device *dev);
void mcp2210_irq_do_gpio(struct mcp2210_device *dev, u16 old_val, u16 new_val);
void _mcp2210_irq_do_intr_counter(struct mcp2210_device *dev, u16 count);

static inline void mcp2210_irq_do_intr_counter(struct mcp2210_device *dev,
					       u16 count)
{
	if (count)
		_mcp2210_irq_do_intr_counter(dev, count);
}

static __maybe_unused int mcp2210_is_pin_irq_producer(struct mcp2210_device *dev, unsigned num)
{
	const struct mcp2210_pin_config *pin = &dev->config->pins[num];

	if (!pin->has_irq || pin->mode == MCP2210_PIN_SPI)
		return 0;

	if (pin->mode == MCP2210_PIN_DEDICATED && num != 6)
		return 0;
	return 1;
}
#else
static inline int  mcp2210_irq_probe (struct mcp2210_device *dev) {return 0;}
static inline void mcp2210_irq_disable(struct mcp2210_device *dev) {}
static inline void mcp2210_irq_remove(struct mcp2210_device *dev) {}
static inline void mcp2210_irq_do_gpio(struct mcp2210_device *dev,
				       u16 old_val, u16 new_val) {}
static inline void mcp2210_irq_do_intr_counter(struct mcp2210_device *dev,
					       u16 count) {}
#endif /* CONFIG_MCP2210_IRQ */

/*****************************************************************************
 * inlines
 */

static inline long jiffdiff(unsigned long a, unsigned long b)
{
	return (long)a - (long)b;
}

/* mcp2210_init_msg - initialize an MCP2210 message
 *
 * As long as body_size and zero_tail are compile-time constants, this is a
 * more efficient mechanism to cleanly init a message than zeroing the whole
 * thing and then overwriting the values you need.
 */
static inline void mcp2210_init_msg(struct mcp2210_msg *msg, u8 cmd,
				    u8 sub_cmd_addr_size, u8 value,
				    const void *body, size_t body_size,
				    int zero_tail) {
	/* init header */
	msg->cmd = cmd;
	msg->head.raw[0] = sub_cmd_addr_size;
	msg->head.raw[1] = value;
	msg->head.raw[2] = 0;

	/* init body */
	if(__builtin_constant_p(body_size))
		BUILD_BUG_ON(body_size > sizeof(msg->body.raw));
	else
		BUG_ON(body_size > sizeof(msg->body.raw));

	if (body)
		memcpy(msg->body.raw, body, body_size);
	else if (body_size)
		BUG();

	/* zero remainder */
	if (zero_tail)
		memset(msg->body.raw + body_size, 0,
		       sizeof(msg->body.raw) - body_size);
}

static inline int compare_spi_settings(
		const struct mcp2210_spi_xfer_settings *a,
		const struct mcp2210_spi_xfer_settings *b)
{
	return memcmp(a, b, sizeof(*a));
}

static inline int mcp2210_add_ctl_cmd(struct mcp2210_device *dev, u8 cmd_code,
				      u8 subcmd_code, void *body,
				      size_t body_size, u8 is_mcp_endianness,
				      gfp_t gfp_flags)
{
	struct mcp2210_cmd_ctl *cmd = mcp2210_alloc_ctl_cmd(dev, cmd_code,
			subcmd_code, body, body_size, is_mcp_endianness,
			gfp_flags);
	return mcp2210_add_cmd((void *)cmd, true);
}

#endif /* __KERNEL__ */

/*****************************************************************************
 * mcp2210-lib.c
 */
struct mcp2210_board_config *copy_board_config(
		struct mcp2210_board_config *dest,
		const struct mcp2210_board_config *src,
		gfp_t gfp_flags);
int validate_board_config(const struct mcp2210_board_config *src,
			  const struct mcp2210_chip_settings *chip);

#define MCP2210_IOCTL_MAGIC 0xba
#define READ_IOCTL  _IOR(MCP2210_IOCTL_MAGIC, 0, int)
#define WRITE_IOCTL _IOW(MCP2210_IOCTL_MAGIC, 1, int)

enum mcp2210_ioctl_cmd {
	MCP2210_IOCTL_CMD,
	MCP2210_IOCTL_EEPROM,
	MCP2210_IOCTL_CONFIG_GET,
	MCP2210_IOCTL_CONFIG_SET,
	MCP2210_IOCTL_MAX
};

static const unsigned int mcp2210_ioctl_map[] = {
    _IOWR(MCP2210_IOCTL_MAGIC, MCP2210_IOCTL_CMD, void *),
    _IOWR(MCP2210_IOCTL_MAGIC, MCP2210_IOCTL_EEPROM, void *),
    _IOWR(MCP2210_IOCTL_MAGIC, MCP2210_IOCTL_CONFIG_GET, void *),
    _IOW (MCP2210_IOCTL_MAGIC, MCP2210_IOCTL_CONFIG_SET, void *),
};

struct mcp2210_ioctl_data {
	u32 struct_size;
	union mcp2210_ioctl_data_body {
		struct mcp2210_ioctl_data_eeprom {
			u8 addr;
			u16 size:9;
			u16 is_read:1;
			u8 data[0];
		} eeprom;
		struct mcp2210_ioctl_data_config {
			u8 wait:1;
			u8 have_config:1;
			struct mcp2210_state state;
			struct mcp2210_board_config config;
		} config;
		struct mcp2210_ioctl_data_cmd {
			int status;
			u8 mcp_status;
			struct mcp2210_msg req;
			struct mcp2210_msg rep;
			u8 data[0];
		} cmd;
	} body;
};

#define IOCTL_DATA_EEPROM_SIZE offsetof( \
			struct mcp2210_ioctl_data, body.eeprom.data)
#define IOCTL_DATA_CONFIG_SIZE offsetof( \
			struct mcp2210_ioctl_data, body.config.config.strings)
#define IOCTL_DATA_CMD_SIZE  offsetof( \
			struct mcp2210_ioctl_data, body.config.config.strings)

struct ioctl_result {
	enum mcp2210_ioctl_cmd ioctl_cmd;
	struct completion completion;
	__user struct mcp2210_ioctl_data *user_data;
	union mcp2210_cmd_any *cmd;
	int status;
	u8 mcp_status;
	unsigned num_cmds;
	unsigned num_cmds_finished;
	struct mcp2210_board_config *new_config;
	struct mcp2210_ioctl_data payload[0];
};

long mcp2210_ioctl_cmd(struct mcp2210_device *dev, struct ioctl_result *result);
long mcp2210_ioctl_config_set(struct mcp2210_device *dev, struct ioctl_result *result);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* _MCP2210_H */
