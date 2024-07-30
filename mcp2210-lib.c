/*
 * MCP2210 kernel-/user-space library
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


#ifdef __KERNEL__
# include <linux/kernel.h>
# include <linux/errno.h>
# include <linux/module.h>
# include <linux/ctype.h>
#else
# include <stddef.h>
# include <stdlib.h>
# include <stdio.h>
# include <string.h>
# include <errno.h>
# include <ctype.h>
#endif /* __KERNEL__ */

#include "mcp2210.h"
#include "mcp2210-debug.h"

#ifdef CONFIG_MCP2210_CREEK
# include "mcp2210-creek.h"
#endif


static void copy_board_config_string(const char *strings, size_t strings_size,
				     size_t *pos, const char **dest,
				     const char *src)
{
	ssize_t buf_size = strings_size - *pos;

	if (!src)
		return;

	BUG_ON(buf_size < 0);

	*dest = (char *)&strings[*pos];
	strncpy((char *)*dest, src, buf_size);
	*pos += strlen(src) + 1;
}

/**
 * copy_board_config - copy a struct mcp2210_board_config object
 *
 * These things are pesky because of the strings and I wasn't in the mood to
 * make static sized strings (maybe I should have?). The main thing that this
 * function does is to collect the strings (regardless of where they are stored
 * in the source struct), copy them into the blob at the end of the struct and
 * correctly set the pointers for each.
 */
struct mcp2210_board_config *copy_board_config(
		struct mcp2210_board_config *dest,
		const struct mcp2210_board_config *src, gfp_t gfp_flags)
{
	struct mcp2210_board_config *ret;
	size_t str_size = 0;
	size_t str_buffer_size;
	size_t pos = 0;
	uint i;

	for (i = 0; i < MCP2210_NUM_PINS; ++i) {
		const struct mcp2210_pin_config *pin = &src->pins[i];
		printk(KERN_DEBUG "src->pins[%u].name = %p (\"%s\")\n",
		       i, pin->name, pin->name ? pin->name : "");
		if (pin->name)
			str_size += strlen(pin->name) + 1;
		printk(KERN_DEBUG "src->pins[%u].modalias = %p (\"%s\")\n",
		       i, pin->modalias, pin->modalias ? pin->modalias : "");
		if (pin->modalias)
			str_size += strlen(pin->modalias) + 1;
	}

	if (dest) {
		if (dest->strings_size < str_size) {
			printk(KERN_ERR "need %u bytes, got %u\n",
			       (uint)str_size, (uint)dest->strings_size);
			return ERR_PTR(-EOVERFLOW);
		}
		str_buffer_size = dest->strings_size;
	} else {
		if (!(dest = kzalloc(sizeof(*ret) + str_size, gfp_flags)))
			return NULL;
		str_buffer_size = str_size;
	}

	memcpy(dest, src, sizeof(*dest));

	/* Newly allocated buffer will be exactly the size needed, supplied
	 * buffer may be larger.  Either way, we ignore the src buffer size */
	dest->strings_size = str_buffer_size;

	for (i = 0; i < MCP2210_NUM_PINS; ++i) {
		const struct mcp2210_pin_config *src_pin = &src->pins[i];
		struct mcp2210_pin_config *dest_pin = &dest->pins[i];

		copy_board_config_string(dest->strings, str_size, &pos,
					 &dest_pin->name,
					 src_pin->name);
		copy_board_config_string(dest->strings, str_size, &pos,
					 &dest_pin->modalias,
					 src_pin->modalias);
	}

	BUG_ON(pos != str_size);

	return dest;
}

int validate_board_config(const struct mcp2210_board_config *src,
			  const struct mcp2210_chip_settings *chip)
{
	uint i;
	uint max_irq = 0;
	uint nr_irqs = 0;
	u8 irq_has_src[8];

	memset(irq_has_src, 0, sizeof(irq_has_src));

	/* validate settings & write irq data */
	for (i = 0; i < MCP2210_NUM_PINS; ++i) {
		const struct mcp2210_pin_config *pin = &src->pins[i];

		/* a few sanity checks on the chip_settings */
		if (chip->pin_mode[i] > MCP2210_PIN_DEDICATED) {
			printk(KERN_ERR "Invalid pin mode in chip_settings\n");
			return -EINVAL;
		}

		if (pin->mode != chip->pin_mode[i]) {
			printk(KERN_ERR "chip_settings don't match "
					"board_config.\n");
			return -EINVAL;
		}

		if (pin->has_irq) {
			/* determine the highest irq used */
			++nr_irqs;
			if (pin->irq > max_irq)
				max_irq = pin->irq;

			if (pin->mode == MCP2210_PIN_DEDICATED && i != 6) {
				printk(KERN_ERR "Invalid: IRQ on dedicated "
						"pin other than 6.");
				return -EINVAL;
			}
		}

		/* should get compiled-out, but leave it here in case we change
		 * the storage of the u8 irq:3 field */
		if (pin->irq >= 7) {
			printk(KERN_ERR "Invalid: irq > 7.");
			return -EINVAL;
		}
	}

	if (nr_irqs > 8) {
		printk(KERN_ERR "Invalid: nr_irqs > 8 (unsupported).");
		return -EINVAL;
	}
#if 0
	/* validate no unused IRQs in allocated range */
	for (i = 0; i < nr_irqs; ++i) {
		uint j;
		for (j = 0; j < MCP2210_NUM_PINS; ++j) {
			const struct mcp2210_pin_config *pin = &src->pins[j];
			if (pin->has_irq && pin->irq == i)
				break;
		}

		if (j != MCP2210_NUM_PINS) {
			printk(KERN_ERR "allocated but unused irq %u", i);
			return -EINVAL;
		}
	}
#endif
	/* validate IRQ consumers match producers */
	for (i = 0; i < MCP2210_NUM_PINS; ++i) {
		const struct mcp2210_pin_config *pin_a = &src->pins[i];
		const struct mcp2210_pin_config *pin_b;
		uint j;

		if (!pin_a->has_irq)
			continue;

		/* verify that we don't have multiple pins producing the same
		 * IRQ */
		switch (pin_a->mode) {
		case MCP2210_PIN_DEDICATED:
			BUG_ON(i != 6);
			/* intentional fall-through */
		case MCP2210_PIN_GPIO:
			if (irq_has_src[pin_a->irq]) {
				printk(KERN_ERR "multiple pins configured to "
				       "produce irq %u", pin_a->irq);
				return -EINVAL;
			}
			irq_has_src[pin_a->irq] = 1;
			continue;

		case MCP2210_PIN_SPI:
			break;
		}

		for (j = 0; j < MCP2210_NUM_PINS; ++j) {
			pin_b = &src->pins[j];
			if (pin_b->mode == MCP2210_PIN_SPI)
				continue;

			if (pin_b->has_irq && pin_b->irq == pin_a->irq)
				break;
		}

		if (j == MCP2210_NUM_PINS) {
			printk(KERN_ERR "Invalid: spi pin %u consumes IRQ "
					"offset %u, but no other pin produces "
					"it.", i, pin_a->irq);
			return -EINVAL;
		}
	}

	/* validate spi cs_gpio */
	for (i = 0; i < MCP2210_NUM_PINS; ++i) {
		const struct mcp2210_pin_config *pin = &src->pins[i];
		u8 cs_gpio = pin->spi.cs_gpio;

		if (pin->mode != MCP2210_PIN_SPI || !pin->spi.use_cs_gpio)
			continue;

		if (cs_gpio > MCP2210_NUM_PINS || src->pins[cs_gpio].mode
							!= MCP2210_PIN_GPIO) {
			printk(KERN_ERR "Invalid: spi pin %u uses gpio for "
			       "chip select, but the specified pin (%hhu) is "
			       "not valid or not a gpio", i, cs_gpio);
			return -EINVAL;
		}
	}

	if (!(src->pins[6].has_irq && src->pins[6].mode == MCP2210_PIN_DEDICATED)
			&& (src->poll_intr_usecs || src->stale_intr_usecs)) {
		printk(KERN_ERR "Invalid: poll_intr_usecs and stale_intr_usecs should be "
				"zero unless pin 6 is set to dedicated with .has_irq=1\n");
		return -EINVAL;
	}
	return 0;
}


/******************************************************************************
 * Creek configuration scheme functions
 */

#ifdef CONFIG_MCP2210_CREEK

static const uint pow10[] = {
	1,
	10,
	100,
	1000,
	10000,
	100000,
	1000000,
	10000000,
	100000000,
	1000000000,
};

static uint _pack_uint(uint value, uint value_bits, uint scale_bits)
{
	const uint num_end = (1 << value_bits);
	const uint scale_end = (1 << scale_bits);
	uint num;
	uint scale;
	uint mult;
	uint ret;

	for (scale = 0, mult = 1; scale < scale_end; ++scale, mult *= 10) {
		num = value / mult;
		if (num < num_end) {
			ret = num | scale << value_bits;

			/* some sanity checks until we know that this is correct */
			BUG_ON(num   & ~((1 << value_bits) - 1));
			BUG_ON(scale & ~((1 << scale_bits) - 1));
			BUG_ON(ret   & ~((1 << (value_bits + scale_bits)) - 1));

			return ret;
		}
	}

	BUG();

	return (uint)-1;
}

static uint _unpack_uint(uint packed, uint value_bits, uint scale_bits)
{
	const uint value_mask = (1 << value_bits) - 1;
	const uint scale_mask = (1 << scale_bits) - 1;
	const uint value = packed & value_mask;
	const uint scale = (packed >> value_bits) & scale_mask;

#if 0
	uint i, mult;

	for (i = 0, mult = 1; i != scale; ++i, mult *= 10);
	return value * mult;
#else
	if (scale <= 9)
		return value * pow10[scale];
	else
		return (uint)-1;

#endif
}

static uint _unpack_uint_opt(struct bit_creek *src, uint value_bits,
			     uint scale_bits, uint def)
{
	if (creek_get_bits(src, 1)) {
		uint data = creek_get_bits(src, value_bits + scale_bits);
		return _unpack_uint(data, value_bits, scale_bits);
	} else
		return def;
}

/**
 * pack_uint_opt - Write an optional packed uint value to a bit stream
 *
 * If the value matches def, then only a single zero bit is written. Otherwise,
 * a one bit is written followed by the valued converted to a packed uint as
 * specified by value_ and scale_ bits.
 */
static void _pack_uint_opt(struct bit_creek *dest, uint value, uint value_bits,
			   uint scale_bits, uint def)
{
	bool is_not_default = (value != def);

	creek_put_bits(dest, is_not_default, 1);
	if (is_not_default) {
		uint data = _pack_uint(value, value_bits, scale_bits);
		creek_put_bits(dest, data, value_bits + scale_bits);
	}
}


static __always_inline void validate_packed(uint value_bits, uint scale_bits)
{
	BUILD_BUG_ON(scale_bits + value_bits >= sizeof(uint) * 8);
}

/**
 * pack_uint - Pack a positive integral value into a smaller space by
 *	       tossing out precision.
 *
 * @value:	The (normal) value
 * @value_bits:	Size (in bits) of the value portion of the result (currently
 * 		requires a compile-time constant)
 * @scale_bits:	Size (in bits) of the scale portion of the result (currently
 * 		requires a compile-time constant)
 *
 * This mechanism for packing numbers allows for a wide range of values with a
 * limited precision.  A "10:2" packed value has 10 bits of value and 2 bits of
 * scale allowing for the values 987, 9870, 98700 or 987000, but not 9879,
 * 98799, etc.  In other words, the least significant portion of the value is
 * zeroed in the conversion process.
 *
 * @returns Upon error, (uint)-1.  Otherwise, the result which is
 * 	    guaranteed to fit into value_bits + scale_bits bits.
 */
static __always_inline uint pack_uint(uint value, uint value_bits,
				      uint scale_bits)
{
	validate_packed(value_bits, scale_bits);

	return _pack_uint(value, value_bits, scale_bits);
}

/**
 * unpack_uint - Unpack a packed positive integral number.
 *
 * @packed:	The packed value
 * @value_bits:	Size (in bits) of the value portion of packed (currently
 * 		requires a compile-time constant)
 * @scale_bits:	Size (in bits) of the scale portion of packed (currently
 * 		requires a compile-time constant)
 *
 * @see pack_uint
 *
 * @returns The unpacked value.
 */
static __always_inline uint unpack_uint(uint packed, uint value_bits,
					uint scale_bits)
{
	validate_packed(value_bits, scale_bits);

	return _unpack_uint(packed, value_bits, scale_bits);
}

/**
 * unpack_uint_opt - Read and unpack an optional packed uint value from a
 * 		     bit stream
 */
static __always_inline uint unpack_uint_opt(struct bit_creek *src,
					    uint value_bits, uint scale_bits,
					    uint def)
{
	validate_packed(value_bits, scale_bits);

	return _unpack_uint_opt(src, value_bits, scale_bits, def);
}


/**
 * pack_uint_opt - Write an optional packed uint value to a bit stream
 *
 * If the value matches def, then only a single zero bit is written. Otherwise,
 * a one bit is written followed by the valued converted to a packed uint as
 * specified by value_ and scale_ bits.
 */
static __always_inline void pack_uint_opt(struct bit_creek *dest, uint value,
					  uint value_bits, uint scale_bits,
					  uint def)
{
	validate_packed(value_bits, scale_bits);

	_pack_uint_opt(dest, value, value_bits, scale_bits, def);
}

/**
 * creek_get_bits -- helper function to treat our buffer like a stream of bits
 */
uint creek_get_bits(struct bit_creek *bs, uint num_bits) {
	const uint MAX_BITS = sizeof(uint) * 8;
//	const uint bit_mask = num_bits == sizeof(uint) * 8 ? 0xffffffff : ((1 << num_bits) - 1);
	const u8 *p = &bs->start[bs->pos / 8];
	uint skip_bits = bs->pos % 8;
	uint start_bits = 8 - skip_bits;
	uint ret;

	BUG_ON(!num_bits);
	BUG_ON(num_bits > MAX_BITS);

	if ((bs->pos + num_bits - 1) / 8 >= bs->size) {
		bs->overflow = 1;
		return (uint)-EOVERFLOW;
	}

	bs->pos += num_bits;
	ret = *p++ >> skip_bits;

	if (num_bits <= start_bits)
		return ret & ((1 << num_bits) - 1);

	num_bits -= start_bits;

	for (;num_bits >= 8; num_bits -= 8)
		ret = ret << 8 | *p++;

	/* less than 8 bits needed, but more than zero */
	if (num_bits > 0)
		ret = ret << num_bits | (*p & ((1 << num_bits) - 1));

	return ret;
}

int creek_put_bits(struct bit_creek *bs, uint value, uint num_bits) {
	const uint MAX_BITS = sizeof(uint) * 8;
	const uint bit_mask = num_bits == sizeof(uint) * 8
				? (uint)-1
				: (uint)((1 << num_bits) - 1);
	u8 *p = &bs->start[bs->pos / 8];
	uint skip_bits = bs->pos % 8;	/* count of bits existing at byte pos
					 * points to and need to be skipped */

	BUG_ON(!num_bits);
	BUG_ON(num_bits > MAX_BITS);
	BUG_ON(value & ~bit_mask);

#if 0
	if (num_bits == 8) {
		printk("0x%02x %c\n", value, value & 0x7f);
		printk("bs->pos = %u (pos %% 8 = %u)\n", (uint)bs->pos, (uint)bs->pos % 8);
	}
#endif

	if ((bs->pos + num_bits - 1) / 8 >= bs->size) {
		bs->overflow = 1;
		return -EOVERFLOW;
	}

	bs->pos += num_bits;
	if (skip_bits) {
		uint start_bits = 8 - skip_bits;
		u8 bits = num_bits < start_bits ? num_bits
						: start_bits;

		*p++ |= (value >> (num_bits - bits)) << skip_bits;
		num_bits -= bits;
	}

	while(num_bits > 0) {
		if (num_bits >= 8) {
			*p++ = (value >> (num_bits - 8));
			num_bits -= 8;
		} else {
			*p = value & ((1 << num_bits) - 1);
			return 0;
		}
	}

	return 0;
}

/**
 * decode_strings - this could be done far more efficiently with arch-specific
 * code but nobody cares.
 */
static const char **decode_strings(char *dest, size_t dest_size,
				   struct bit_creek *src, uint num_strings)
{
	const char **index = kzalloc(sizeof(char *) * num_strings, GFP_KERNEL);
	const char *  const dend = &dest[dest_size];
	const char ** const iend = &index[num_strings];
	char *d;
	const char **i;

	if (!index)
		return ERR_PTR(-ENOMEM);

	//printk("index = %p\n", index);
	for (index[0] = d = dest, i = &index[1]; d != dend;) {
		u8 c = creek_get_bits(src, 8);

		*d++ = (char)(c & 0x7f);
		//printk("c = 0x%02hhx, d = '%c'\n", c, *(d - 1));

		/* bit 7 high terminates the string */
		if (c & 0x80) {
			if (unlikely(d == dend))
				goto error;

			/* terminate string */
			*d++ = 0;

			/* record start of next string */
			if (i != iend) {
				//printk("i = %p\n", i);
				*i++ = d;
			} else
				break;
		}
	}

	/* make sure we're exactly where we expected to be */
	if (d == dend && i == iend)
		return index;

error:
	printk(KERN_ERR "strings not properly read\n");
	kfree(index);
	return ERR_PTR(-EPROTO);
}

#ifdef CONFIG_MCP2210_DEBUG
static void creek_debug(const char *str, struct bit_creek *bs)
{
	printk(KERN_DEBUG "%s - %u\n", str, (uint)bs->pos);
}
#else
# define creek_debug(a,b)
#endif

/**
 * mcp2210_board_config - read in and decode settings
 *
 * Reads in encoded settings from source and writes them to dest
 */
struct mcp2210_board_config *creek_decode(
		const struct mcp2210_chip_settings *chip_settings,
		const u8* src, size_t size, gfp_t gfp_flags)
{
	struct mcp2210_board_config *board_config = NULL;
	struct mcp2210_board_config *tmp;
	struct creek_data dec;
	struct bit_creek bs;
	int ret = 0;
	uint strings_start;
	uint i;

	memset(&dec, 0, sizeof(dec));
	bs.start = (u8 *)src;
	bs.size = size;
	bs.pos = 0;
	bs.overflow = 0;

	creek_debug("-------- creek_decode --------", &bs);

	/* magic */
	dec.magic[0] = creek_get_bits(&bs, 8);
	dec.magic[1] = creek_get_bits(&bs, 8);
	dec.magic[2] = creek_get_bits(&bs, 8);
	dec.magic[3] = creek_get_bits(&bs, 8);
	creek_debug("magic", &bs);

	if (memcmp(dec.magic, CREEK_CONFIG_MAGIC, 4)) {
		printk(KERN_WARNING "EEPROM magic doesn't match");
		return ERR_PTR(-ENODEV);
	}

	/* version */
	dec.ver = creek_get_bits(&bs, 4);
	creek_debug("version", &bs);

	if (dec.ver > 2) {
		printk(KERN_ERR "Creek version %hhu unsupported", dec.ver);
		return ERR_PTR(-EPROTO);
	}

	tmp = kzalloc(sizeof(*tmp), gfp_flags);
	if (!tmp)
		return ERR_PTR(-ENOMEM);

	/* read has_strings, validate & populate pin modes, count SPI pins */
	for (i = 0; i < MCP2210_NUM_PINS; ++i) {
		struct mcp2210_pin_config *pin = &tmp->pins[i];
		u8 str_flags = creek_get_bits(&bs, 2);

		dec.name_index[i]	= str_flags & 1 ? ++dec.str_count : 0;
		dec.modalias_index[i]	= str_flags & 2 ? ++dec.str_count : 0;

		switch ((pin->mode = chip_settings->pin_mode[i])) {
		case MCP2210_PIN_GPIO:
		case MCP2210_PIN_DEDICATED:
			break;

		case MCP2210_PIN_SPI:
			dec.spi_pin_num[dec.spi_count++] = i;
			break;

		default:
			printk(KERN_ERR "invalid pin_mode\n");
			return ERR_PTR(-EPROTO);
		};
	}
	creek_debug("strings present, count SPI", &bs);

	/* 3-wire capability */
	tmp->_3wire_capable = creek_get_bits(&bs, 1);
	if (tmp->_3wire_capable) {
		tmp->_3wire_tx_enable_active_high = creek_get_bits(&bs, 1);
		tmp->_3wire_tx_enable_pin = creek_get_bits(&bs, 3);
	}
	creek_debug("3wire", &bs);

	/* read IRQs */
	for (i = 0; i < MCP2210_NUM_PINS; ++i) {
		struct mcp2210_pin_config *pin = &tmp->pins[i];

		if (pin->mode == MCP2210_PIN_DEDICATED && i != 6)
			/* skip IRQ for dedicated pins other than 6 */
			continue;

		if ((pin->has_irq = creek_get_bits(&bs, 1))) {
			pin->irq = creek_get_bits(&bs, 3);
			if (dec.ver >= 1)
				pin->irq_threaded = creek_get_bits(&bs, 1);

			if (pin->mode == MCP2210_PIN_GPIO) {
				dec.have_gpio_irqs = 1;
				if (dec.ver == 0)
					pin->irq_type = creek_get_bits(&bs, 3);
				else
					pin->irq_type = creek_get_bits(&bs, 4);
			}
		}
	}
	creek_debug("IRQs", &bs);

	/* read gpio polling settings */
	if (dec.have_gpio_irqs) {
		uint data	      = creek_get_bits(&bs, 13);
		u32 interval	      = unpack_uint(data, 10, 3);

		tmp->poll_gpio_usecs  = interval;
		tmp->stale_gpio_usecs = unpack_uint_opt(&bs, 10, 3, interval);
	}
	creek_debug("gpio polling done", &bs);

	/* read interrupt polling settings */
	if (tmp->pins[6].mode == MCP2210_PIN_DEDICATED && tmp->pins[6].has_irq) {
		/* if no gpio irqs, don't read the feature bit */
		uint same_as_gpio = dec.have_gpio_irqs && creek_get_bits(&bs, 1);

		if (same_as_gpio) {
			tmp->poll_intr_usecs  = tmp->poll_gpio_usecs;
			tmp->stale_intr_usecs = tmp->stale_gpio_usecs;
		} else {
			uint data	      = creek_get_bits(&bs, 13);
			u32 interval	      = unpack_uint(data, 10, 3);
			tmp->poll_intr_usecs  = interval;
			tmp->stale_intr_usecs = unpack_uint_opt(&bs, 10, 3,
								interval);
		}
	}
	creek_debug("intr polling done", &bs);

	for (i = 0; i < dec.spi_count; ++i) {
		u8 pin = dec.spi_pin_num[i];
		struct mcp2210_pin_config_spi *spi = &tmp->pins[pin].spi;
		uint speed_mag = dec.ver < 2 ? 2 : 3;

		spi->max_speed_hz	   = unpack_uint_opt(&bs, 10, speed_mag,
							     MCP2210_MAX_SPEED);
		spi->min_speed_hz	   = unpack_uint_opt(&bs, 10, speed_mag,
							     MCP2210_MIN_SPEED);
		spi->mode		   = creek_get_bits(&bs, 8);
		spi->bits_per_word	   = 8;
		spi->use_cs_gpio	   = creek_get_bits(&bs, 1);
		if (spi->use_cs_gpio)
			spi->cs_gpio	   = creek_get_bits(&bs, 3);
		spi->cs_to_data_delay	   = unpack_uint_opt(&bs, 7, 2, 0);
		spi->last_byte_to_cs_delay = unpack_uint_opt(&bs, 7, 2, 0);
		spi->delay_between_bytes   = unpack_uint_opt(&bs, 7, 2, 0);
		spi->delay_between_xfers   = unpack_uint_opt(&bs, 7, 2, 0);
		creek_debug("spi data", &bs);
	}

	/* record start of strings and find out how many bytes we need */
	strings_start = bs.pos;
	for (i = 0; i < dec.str_count; ) {
		u8 c = creek_get_bits(&bs, 8);
		if (c & 0x80) {
			++i;
			dec.str_size += 2;
		} else
			dec.str_size += 1;
		c &= 0x7f;
		if (unlikely(c < ' ' || c == 0x7f)) {
			ret = -EPROTO;
			goto exit_free;
		}
	}
	creek_debug("strings", &bs);

	if (bs.overflow) {
		ret = -EPROTO;
		goto exit_free;
	}

	/* now that we know how much room we need for strings we can allocate &
	 * init our return value */
	if (!(board_config = kzalloc(sizeof(struct mcp2210_board_config)
				   + dec.str_size, GFP_KERNEL)))
		return ERR_PTR(-ENOMEM);

	/* copy temporary data and then free tmp */
	memcpy(board_config, tmp, sizeof(*board_config));
	kfree(tmp);
	board_config->strings_size = dec.str_size;

	/* rewind bit stream and read strings */
	bs.pos = strings_start;
	dec.string_index = decode_strings((char *)board_config->strings,
					  board_config->strings_size, &bs,
					  dec.str_count);

	if (IS_ERR(dec.string_index)) {
		ret = PTR_ERR(dec.string_index);
		dec.string_index = NULL;
		goto exit_free;
	}

	/* now populate our mcp2210_board_config with the string pointers */
	board_config->strings_size = dec.str_size;
	for (i = 0; i < MCP2210_NUM_PINS; ++i) {
		if (dec.name_index[i])
			board_config->pins[i].name = dec.string_index[
						dec.name_index[i] - 1];
		if (dec.modalias_index[i])
			board_config->pins[i].modalias = dec.string_index[
						dec.modalias_index[i] - 1];
	}

//	printk(KERN_DEBUG "creek_encode: used %u bits\n", bs.pos);

exit_free:
	kfree(dec.string_index);
	if (ret) {
		kfree(board_config);
		return ERR_PTR(ret);
	}
	return board_config;
}

/* we could restrict strings to one case and such and get them smaller, but
 * 7-bit terminated should be good enough */
static void put_encoded_string(struct bit_creek *dest, const char *str)
{
	if (!str || !*str) {
		creek_put_bits(dest, 0x80, 8);
		return;
	}

	while (str[1])
		creek_put_bits(dest, *str++, 8);

	creek_put_bits(dest, *str | 0x80, 8);
}

int creek_encode(const struct mcp2210_board_config *src,
		 const struct mcp2210_chip_settings *chip, u8* dest,
		 size_t size, u8 ver)
{
	struct creek_data data;
	struct bit_creek bs;
	uint i;
	int ret;

	ret = validate_board_config(src, chip);
	if (ret) {
		printk(KERN_ERR "Invalid configuration\n");
		return ret;
	}

	if (ver > 2) {
		printk(KERN_ERR "Invalid encoding version: %u. Supported "
				"versions are 0 and 1.\n", ver);
		return ret;
	}

	memset(&data, 0, sizeof(data));
	bs.start = (u8 *)dest;
	bs.size = size;
	bs.pos = 0;
	bs.overflow = 0;

	creek_debug("-------- creek_encode --------", &bs);

	/* magic */
	creek_put_bits(&bs, CREEK_CONFIG_MAGIC[0], 8);
	creek_put_bits(&bs, CREEK_CONFIG_MAGIC[1], 8);
	creek_put_bits(&bs, CREEK_CONFIG_MAGIC[2], 8);
	creek_put_bits(&bs, CREEK_CONFIG_MAGIC[3], 8);
	creek_debug("magic", &bs);

	/* version */
	creek_put_bits(&bs, ver, 4);
	creek_debug("version", &bs);

	/* write has_string header */
	for (i = 0; i < MCP2210_NUM_PINS; ++i) {
		const struct mcp2210_pin_config *pin = &src->pins[i];
		u8 value = (!!pin->name) | (!!pin->modalias) << 1;

		creek_put_bits(&bs, value, 2);
	}
	creek_debug("strings present", &bs);

	/* 3-wire capability (a gpio that functions as a tx enable) */
	creek_put_bits(&bs, src->_3wire_capable, 1);
	if (src->_3wire_capable) {
		creek_put_bits(&bs, src->_3wire_tx_enable_active_high, 1);
		creek_put_bits(&bs, src->_3wire_tx_enable_pin, 3);
	}
	creek_debug("3wire", &bs);

	/* write irq data & count spi devices */
	for (i = 0; i < MCP2210_NUM_PINS; ++i) {
		const struct mcp2210_pin_config *pin = &src->pins[i];

		switch (pin->mode) {
		case MCP2210_PIN_SPI:
			data.spi_pin_num[data.spi_count++] = i;
			break;

		case MCP2210_PIN_DEDICATED:
			/* skip IRQ for dedicated pins other than 6 */
			if (i != 6)
				continue;
			printk(KERN_DEBUG "dedicated pin...\n");
		};

		creek_put_bits(&bs, pin->has_irq, 1);
		printk(KERN_DEBUG "pin %u %s IRQ (pos = %u)\n", i, pin->has_irq
				? "has" : "doesn't have", (uint)bs.pos);

		if (pin->has_irq) {
			/* write the relative IRQ value */
			creek_put_bits(&bs, pin->irq, 3);
			if (ver >= 1)
				creek_put_bits(&bs, pin->irq_threaded, 1);

			/* only gpio have an irq_type, the dedicated line's
			 * type is configured via the USB interface */
			if (pin->mode == MCP2210_PIN_GPIO) {
				data.have_gpio_irqs = 1;
				if (ver == 0)
					creek_put_bits(&bs, pin->irq_type, 3);
				else
					creek_put_bits(&bs, pin->irq_type, 4);
			}
		}
	}
	creek_debug("IRQs", &bs);

	/* write gpio polling settings */
	if (data.have_gpio_irqs) {
		u32 interval = src->poll_gpio_usecs;

		creek_put_bits(&bs, pack_uint(interval, 10, 3), 13);
		pack_uint_opt(&bs, src->stale_gpio_usecs, 10, 3, interval);
	}
	creek_debug("gpio polling done", &bs);

	/* write interrupt polling settings */
	if (src->pins[6].has_irq && src->pins[6].mode
						== MCP2210_PIN_DEDICATED) {
		uint same_as_gpio = data.have_gpio_irqs
			&& src->poll_gpio_usecs  == src->poll_intr_usecs
			&& src->stale_intr_usecs == src->stale_gpio_usecs;

		/* if we don't have any gpio irqs we should skip this bit */
		if (data.have_gpio_irqs)
			creek_put_bits(&bs, same_as_gpio, 1);

		if (!same_as_gpio) {
			u32 interval = src->poll_intr_usecs;

			creek_put_bits(&bs, pack_uint(interval, 10, 3), 13);
			pack_uint_opt(&bs, src->stale_intr_usecs, 10, 3,
				      interval);
		}
	}
	creek_debug("intr polling done", &bs);

	/* write spi setting data */
	for (i = 0; i < data.spi_count; ++i) {
		u8 pin = data.spi_pin_num[i];
		const struct mcp2210_pin_config_spi *spi = &src->pins[pin].spi;
		uint speed_mag = ver < 2 ? 2 : 3;

		pack_uint_opt(&bs, spi->max_speed_hz, 10, speed_mag,
			      MCP2210_MAX_SPEED);
		pack_uint_opt(&bs, spi->min_speed_hz, 10, speed_mag,
			      MCP2210_MIN_SPEED);
		creek_put_bits(&bs, spi->mode, 8);
		/* spi->bits_per_word should always be 8 since this chip
		 * doesn't support any other value */
		creek_put_bits(&bs, spi->use_cs_gpio, 1);
		if (spi->use_cs_gpio)
			creek_put_bits(&bs, spi->cs_gpio, 3);
		pack_uint_opt(&bs, spi->cs_to_data_delay,	7, 2, 0);
		pack_uint_opt(&bs, spi->last_byte_to_cs_delay,	7, 2, 0);
		pack_uint_opt(&bs, spi->delay_between_bytes,	7, 2, 0);
		pack_uint_opt(&bs, spi->delay_between_xfers,	7, 2, 0);
		creek_debug("spi data", &bs);
	}

	for (i = 0; i < MCP2210_NUM_PINS; ++i) {
		const struct mcp2210_pin_config *pin = &src->pins[i];
		if (pin->name) {
			put_encoded_string(&bs, pin->name);
			creek_debug("name", &bs);
		}

		if (pin->modalias) {
			put_encoded_string(&bs, pin->modalias);
			creek_debug("modalias", &bs);
		}
	}
	creek_debug("strings", &bs);

	printk(KERN_DEBUG "creek_encode: used %u bits\n", (uint)bs.pos);

	if (bs.overflow)
		return -EOVERFLOW;

	return bs.pos;
}

#ifdef __KERNEL__
struct mcp2210_board_config *mcp2210_creek_probe(struct mcp2210_device *dev,
						 gfp_t gfp_flags)
{
	const u8 all_read = MCP2210_EEPROM_READ
			  | MCP2210_EEPROM_READ << 2
			  | MCP2210_EEPROM_READ << 4
			  | MCP2210_EEPROM_READ << 6;
	unsigned long irqflags;
	uint i;
	u8 *eeprom_tmp = NULL;
	struct mcp2210_chip_settings *pucs = &dev->s.power_up_chip_settings;
	struct mcp2210_board_config *ret = NULL;

	BUG_ON(!dev);
	BUG_ON(!dev->s.have_power_up_chip_settings);

	if (!(eeprom_tmp = kzalloc(0x100, gfp_flags)))
		return ERR_PTR(-ENOMEM);

	/* make sure the entire eeprom is read and then copy it to our tmp
	 * buffer */
	spin_lock_irqsave(&dev->eeprom_spinlock, irqflags);
	for (i = 0; i < 64; ++i) {
		if (unlikely(dev->eeprom_state[i] != all_read)) {
			ret = ERR_PTR(-EPERM);
			break;
		}
	}

	if (!ret)
		memcpy(eeprom_tmp, dev->eeprom_cache, 0x100);
	spin_unlock_irqrestore(&dev->eeprom_spinlock, irqflags);

	if (ret) {
		mcp2210_err("EEPROM not completely read.");
		goto exit_free;
	}

	ret = creek_decode(pucs, eeprom_tmp, 0x100, gfp_flags);

exit_free:
	kfree(eeprom_tmp);
	return ret;
}
#endif /*__KERNEL__ */
#endif /* CONFIG_MCP2210_CREEK */

/******************************************************************************
 * Verbose debug support functions
 */

#ifdef CONFIG_MCP2210_DEBUG_VERBOSE
struct code_desc {
	u8 code;
	const char *name;
};

const char indent_str[41] = "                                        ";
static inline const char *get_indent(unsigned size)
{
	static const unsigned max_indent = sizeof(indent_str) - 1;

	if (size > max_indent)
		return indent_str;
	else
		return &indent_str[max_indent - size];
}


static struct code_desc mcp2210_cmd_codes[] = {
	{MCP2210_CMD_SET_NVRAM,		"MCP2210_CMD_SET_NVRAM"},
	{MCP2210_CMD_GET_NVRAM,		"MCP2210_CMD_GET_NVRAM"},
	{MCP2210_CMD_SEND_PASSWD,	"MCP2210_CMD_SEND_PASSWD"},
	{MCP2210_CMD_GET_SPI_CONFIG,	"MCP2210_CMD_GET_SPI_CONFIG"},
	{MCP2210_CMD_SET_SPI_CONFIG,	"MCP2210_CMD_SET_SPI_CONFIG"},
	{MCP2210_CMD_GET_CHIP_CONFIG,	"MCP2210_CMD_GET_CHIP_CONFIG"},
	{MCP2210_CMD_SET_CHIP_CONFIG,	"MCP2210_CMD_SET_CHIP_CONFIG"},
	{MCP2210_CMD_GET_PIN_DIR,	"MCP2210_CMD_GET_PIN_DIR"},
	{MCP2210_CMD_SET_PIN_DIR,	"MCP2210_CMD_SET_PIN_DIR"},
	{MCP2210_CMD_GET_PIN_VALUE,	"MCP2210_CMD_GET_PIN_VALUE"},
	{MCP2210_CMD_SET_PIN_VALUE,	"MCP2210_CMD_SET_PIN_VALUE"},
	{MCP2210_CMD_READ_EEPROM,	"MCP2210_CMD_READ_EEPROM"},
	{MCP2210_CMD_WRITE_EEPROM,	"MCP2210_CMD_WRITE_EEPROM"},
	{MCP2210_CMD_GET_INTERRUPTS,	"MCP2210_CMD_GET_INTERRUPTS"},
	{MCP2210_CMD_SPI_TRANSFER,	"MCP2210_CMD_SPI_TRANSFER"},
	{MCP2210_CMD_SPI_CANCEL,	"MCP2210_CMD_SPI_CANCEL"},
	{MCP2210_CMD_SPI_RELEASE,	"MCP2210_CMD_SPI_RELEASE"},
	{MCP2210_CMD_GET_STATUS,	"MCP2210_CMD_GET_STATUS"},
	{}
};

static struct code_desc mcp2210_sub_cmd_codes[] = {
	{MCP2210_NVRAM_CHIP,		"MCP2210_NVRAM_CHIP"},
	{MCP2210_NVRAM_SPI,		"MCP2210_NVRAM_SPI"},
	{MCP2210_NVRAM_KEY_PARAMS,	"MCP2210_NVRAM_KEY_PARAMS"},
	{MCP2210_NVRAM_MFG,		"MCP2210_NVRAM_MFG"},
	{MCP2210_NVRAM_MFG,		"MCP2210_NVRAM_MFG"},
	{}
};

static struct code_desc mcp2210_status_codes[] = {
	{MCP2210_STATUS_SUCCESS,	"MCP2210_STATUS_SUCCESS"},
	{MCP2210_STATUS_SPI_NOT_OWNED,	"MCP2210_STATUS_SPI_NOT_OWNED"},
	{MCP2210_STATUS_BUSY,		"MCP2210_STATUS_BUSY"},
	{MCP2210_STATUS_WRITE_FAIL,	"MCP2210_STATUS_WRITE_FAIL"},
	{MCP2210_STATUS_NO_ACCESS,	"MCP2210_STATUS_NO_ACCESS"},
	{MCP2210_STATUS_PERM_LOCKED,	"MCP2210_STATUS_PERM_LOCKED"},
	{MCP2210_STATUS_BAD_PASSWD,	"MCP2210_STATUS_BAD_PASSWD"},
	{}
};

static struct code_desc mcp2210_pin_modes[] = {
	{MCP2210_PIN_GPIO,	"gpio"},
	{MCP2210_PIN_SPI,	"spi"},
	{MCP2210_PIN_DEDICATED,	"dedicated"},
	{}
};

static struct code_desc mcp2210_eeprom_status_codes[] = {
	{MCP2210_EEPROM_UNREAD,		"unread"},
	{MCP2210_EEPROM_READ_PENDING,	"read pending"},
	{MCP2210_EEPROM_READ,		"read"},
	{MCP2210_EEPROM_DIRTY,		"dirty"},
	{}
};

static struct code_desc mcp2210_cmd_type_id_codes[] = {
	{MCP2210_CMD_TYPE_CTL,		"ctl"},
	{MCP2210_CMD_TYPE_SPI,		"spi"},
	{MCP2210_CMD_TYPE_EEPROM,	"eeprom"},
	{}
};

static struct code_desc mcp2210_state_codes[] = {
	{MCP2210_STATE_NEW,		"new"},
	{MCP2210_STATE_SUBMITTED,	"submitted"},
	{MCP2210_STATE_COMPLETE,	"done"},
	{MCP2210_STATE_DEAD,	"dead"},
	{}
};


static const char *get_code_str(const struct code_desc *code_desc, u8 code)
{
	unsigned i;
	for (i = 0; code_desc[i].name; ++i) {
		if (code == code_desc[i].code) {
			return code_desc[i].name;
		}
	}

	return "(unknown value)";
}

inline const char *get_cmd_str(u8 cmd)
{
	return get_code_str(mcp2210_cmd_codes, cmd);
}

inline const char *get_sub_cmd_str(u8 sub_cmd)
{
	return get_code_str(mcp2210_sub_cmd_codes, sub_cmd);
}

inline const char *get_status_str(u8 status)
{
	return get_code_str(mcp2210_status_codes, status);
}

inline const char *get_pin_mode_str(u8 mode)
{
	return get_code_str(mcp2210_pin_modes, mode);
}

inline const char *get_eeprom_status_str(u8 mode)
{
	return get_code_str(mcp2210_eeprom_status_codes, mode);
}

inline const char *get_cmd_type_str(u8 mode)
{
	return get_code_str(mcp2210_cmd_type_id_codes, mode);
}

inline const char *get_state_str(u8 mode)
{
	return get_code_str(mcp2210_state_codes, mode);
}

void dump_chip_settings(const char *level, unsigned indent, const char *start,
			const struct mcp2210_chip_settings *s)
{
	const char *ind = get_indent(indent + 2);
	unsigned i;
	char buf[19];

	printk("%s%s%s%p struct mcp2210_chip_settings {\n"
	       "%s.pin_mode {\n",
	       level, get_indent(indent), start, s,
	       ind);

	for (i = 0; i < MCP2210_NUM_PINS; ++i)
		printk("%s%s  [%u] = %s\n", level, ind, i,
		       get_pin_mode_str(s->pin_mode[i]));

	for (i = 0; i < 8; ++i) {
		size_t off = i * 2 + i / 4;
		snprintf(&buf[off], sizeof(buf) - off, "%02hhx ", s->password[i]);
	}
	buf[17] = 0;

	printk("%s%s}\n"
	       "%s.gpio_value           = 0x%04hx\n"
	       "%s.gpio_direction       = 0x%04hx\n"
	       "%s.other_settings       = 0x%02hhx\n"
	       "%s.nvram_access_control = 0x%02hhx\n"
	       "%s.password             = %s\n"
	       "%s}\n",
	       level, ind,
	       ind, s->gpio_value,
	       ind, s->gpio_direction,
	       ind, s->other_settings,
	       ind, s->nvram_access_control,
	       ind, buf,
	       get_indent(indent));
}

void dump_spi_xfer_settings(const char *level, unsigned indent,
			    const char *start,
			    const struct mcp2210_spi_xfer_settings *s)
{
	const char *ind = get_indent(indent + 2);

	printk("%s%s%s%p struct mcp2210_spi_xfer_setting {\n"
	       "%s.bitrate               = %u\n"
	       "%s.idle_cs               = 0x%04hx\n"
	       "%s.active_cs             = 0x%04hx\n"
	       "%s.cs_to_data_delay      = 0x%04hx\n"
	       "%s.last_byte_to_cs_delay = 0x%04hx\n"
	       "%s.delay_between_bytes   = 0x%04hx\n"
	       "%s.bytes_per_trans       = 0x%04hx\n"
	       "%s.mode                  = 0x%02hhx\n"
	       "%s}\n",
	       level, get_indent(indent), start, s,
	       ind, s->bitrate,
	       ind, s->idle_cs,
	       ind, s->active_cs,
	       ind, s->cs_to_data_delay,
	       ind, s->last_byte_to_cs_delay,
	       ind, s->delay_between_bytes,
	       ind, s->bytes_per_trans,
	       ind, s->mode,
	       get_indent(indent));
}

void dump_usb_key_params(
	const char *level, unsigned indent, const char *start,
	const struct mcp2210_usb_key_params *params)
{
	const char *ind = get_indent(indent + 2);

	printk("%s%s%s%p struct mcp2210_spi_xfer_setting {\n"
	       "%s.vid               = 0x%04hx\n"
	       "%s.pid               = 0x%04hx\n"
	       "%s.chip_power_option = 0x%02hhx\n"
	       "%s.requested_power   = %hhu (%umA)\n"
	       "%s}\n",
	       level, get_indent(indent), start, params,
	       ind, params->vid,
	       ind, params->pid,
	       ind, params->chip_power_option,
	       ind, params->requested_power, params->requested_power * 2u,
	       get_indent(indent));
}

void dump_state(const char *level, unsigned indent, const char *start,
		const struct mcp2210_state *s)
{
	const char *ind = get_indent(indent + 2);

	printk("%s%s%s%p struct mcp2210_state {\n"
	       "%s.have_chip_settings          = %hhu\n"
	       "%s.have_power_up_chip_settings = %hhu\n"
	       "%s.have_spi_settings           = %hhu\n"
	       "%s.have_power_up_spi_settings  = %hhu\n"
	       "%s.have_usb_key_params         = %hhu\n",
	       level, get_indent(indent), start, s,
	       ind, s->have_chip_settings,
	       ind, s->have_power_up_chip_settings,
	       ind, s->have_spi_settings,
	       ind, s->have_power_up_spi_settings,
	       ind, s->have_usb_key_params);

	if (!s->have_chip_settings)
		printk("%s%s.chip_settings          = (uninitialized)\n",
		       level, ind);
	else
		dump_chip_settings(level, indent + 2,
				".chip_settings = ",
				&s->chip_settings);

	if (!s->have_power_up_chip_settings)
		printk("%s%s.power_up_chip_settings = (uninitialized)\n",
		       level, ind);
	else
		dump_chip_settings(level, indent + 2,
				".power_up_chip_settings = ",
				&s->power_up_chip_settings);

	if (!s->have_spi_settings)
		printk("%s%s.spi_settings           = (uninitialized)\n",
		       level, ind);
	else
		dump_spi_xfer_settings(level, indent + 2,
				".spi_settings = ",
				&s->spi_settings);

	if (!s->have_power_up_spi_settings)
		printk("%s%s.power_up_spi_settings  = (uninitialized)\n",
		       level, ind);
	else
		dump_spi_xfer_settings(level, indent + 2,
				".power_up_spi_settings = ",
				&s->power_up_spi_settings);

	if (!s->have_usb_key_params)
		printk("%s%s.usb_key_params         = (uninitialized)\n",
		       level, ind);
	else
		dump_usb_key_params(level, indent + 2,
				".usb_key_params = ",
				&s->usb_key_params);

	printk("%s%s.cur_spi_config              = %d\n"
	       "%s.idle_cs                     = 0x%04hx\n"
	       "%s.active_cs                   = 0x%04hx\n"
	       "%s.spi_delay_per_kb            = %lu\n"
	       "%s}\n",
	       level, ind, s->cur_spi_config,
	       ind, s->idle_cs,
	       ind, s->active_cs,
	       ind, s->spi_delay_per_kb,
	       ind);
}

void dump_pin_config(const char *level, unsigned indent, const char *start,
		     const struct mcp2210_pin_config *cfg)
{
	const char *ind = get_indent(indent + 2);
	const char *ind2 = get_indent(indent + 4);

	printk("%s%s%s%p struct mcp2210_pin_config {\n"
	       "%s.mode         = %hhu (%s)\n"
	       "%s.has_irq      = %hhu\n"
	       "%s.irq          = %hhu\n"
	       "%s.irq_type     = 0x%02hhx\n"
	       "%s.irq_threaded = %hhx\n",
	       level, get_indent(indent), start, cfg,
	       ind, cfg->mode, get_pin_mode_str(cfg->mode),
	       ind, cfg->has_irq,
	       ind, cfg->irq,
	       ind, cfg->irq_type,
	       ind, cfg->irq_threaded);

	switch (cfg->mode) {
	case MCP2210_PIN_DEDICATED:
	case MCP2210_PIN_GPIO:
		break;

	case MCP2210_PIN_SPI:
		printk("%s%s.spi {\n"
		       "%s.max_speed_hz          = %u\n"
		       "%s.min_speed_hz          = %u\n"
		       "%s.mode                  = 0x%02hhx\n"
		       "%s.bits_per_word         = %hhu\n"
		       "%s.cs_to_data_delay      = %hu\n"
		       "%s.last_byte_to_cs_delay = %hu\n"
		       "%s.delay_between_bytes   = %hu\n"
		       "%s.delay_between_xfers   = %hu\n"
		       "%s}\n",
		       level, ind,
		       ind2, cfg->spi.max_speed_hz,
		       ind2, cfg->spi.min_speed_hz,
		       ind2, cfg->spi.mode,
		       ind2, cfg->spi.bits_per_word,
		       ind2, cfg->spi.cs_to_data_delay,
		       ind2, cfg->spi.last_byte_to_cs_delay,
		       ind2, cfg->spi.delay_between_bytes,
		       ind2, cfg->spi.delay_between_xfers,
		       ind);
		break;
	};

	printk("%s%s.modalias     = %s\n"
	       "%s.name         = %s\n"
	       "%s}\n",
	       level, ind, cfg->modalias,
	       ind, cfg->name,
	       get_indent(indent));
}

void dump_board_config(const char *level, unsigned indent, const char *start,
		       const struct mcp2210_board_config *bc)
{
	const char *ind = get_indent(indent + 2);
	char buf[7] = "[0] = ";
	uint i;

	printk("%s%s%s%p struct mcp2210_board_config {\n"
	       "%s.pins {\n",
	       level, get_indent(indent), start, bc,
	       ind);

	for (i = 0; i < MCP2210_NUM_PINS; ++i) {
		buf[1] = '0' + i;
		dump_pin_config(level, indent + 4, buf, &bc->pins[i]);
	}

	printk("%s%s}\n"
	       "%s.poll_gpio_usecs  = %u\n"
	       "%s.stale_gpio_usecs = %u\n"
	       "%s.poll_intr_usecs  = %u\n"
	       "%s.stale_intr_usecs = %u\n"
	       "%s.strings_size     = %u\n"
	       "%s.strings          = %p \"%s\"\n"
	       "%s}\n",
	       level, ind,
	       ind, (uint)bc->poll_gpio_usecs,
	       ind, (uint)bc->stale_gpio_usecs,
	       ind, (uint)bc->poll_intr_usecs,
	       ind, (uint)bc->stale_intr_usecs,
	       ind, (uint)bc->strings_size,
	       ind, bc->strings, bc->strings,
	       get_indent(indent));
}

#ifdef __KERNEL__
void dump_ep(const char *level, unsigned indent, const char *start,
	     const struct mcp2210_endpoint *ep)
{
	const char *ind = get_indent(indent + 2);

	printk("%s%s%s%p struct mcp2210_endpoint {\n"
	       "%s.ep                = %p\n"
	       "%s.urb               = %p\n"
	       "%s.buffer            = %p\n"
	       "%s.submit_time       = %lu\n"
	       "%s.unlink_in_process = %d\n"
	       "%s.state             = %hhu\n"
	       "%s.is_mcp_endianness = %hhu\n"
	       "%s.kill              = %hhu\n"
	       "%s.is_dir_in         = %hhu\n"
	       "%s.retry_count       = %hhu\n"
	       "%s}\n",
	       level, get_indent(indent), start, ep,
	       ind, ep->ep,
	       ind, ep->urb,
	       ind, ep->buffer,
	       ind, ep->submit_time,
	       ind, atomic_read(&ep->unlink_in_process),
	       ind, ep->state,
	       ind, ep->is_mcp_endianness,
	       ind, ep->kill,
	       ind, ep->is_dir_in,
	       ind, ep->retry_count,
	       get_indent(indent));
}

void dump_dev(const char *level, unsigned indent, const char *start,
	      const struct mcp2210_device *dev)
{
	const char *ind = get_indent(indent + 2);

	printk("%s%s%s%p struct mcp2210_device {\n"
	       "%s.udev                = %-p\n"
	       "%s.intf                = %-p\n"

	       "%s.dev_spinlock        = %slocked\n"
	       "%s.queue_spinlock      = %slocked\n"
#ifdef CONFIG_MCP2210_IOCTL
	       "%s.io_mutex            = %slocked\n"
#endif
	       "%s.kref\n"
#ifdef CONFIG_MCP2210_DEBUG
	       "%s.manager_running     = %d\n"
#endif
	       "%s.cmd_queue           = {.next = %-p, .prev = %-p}\n"
	       "%s.cur_cmd             = %-p\n"
	       "%s.delayed_list        = {.next = %-p, .prev = %-p}\n"
	       "%s.delayed_cmd         = %-p\n"
	       "%s.timer\n",
	       level, get_indent(indent), start, dev,
	       ind, dev->udev,
	       ind, dev->intf,

	       ind, spin_is_locked((struct spinlock*)&dev->dev_spinlock)
		    ? "" : "un",
	       ind, spin_is_locked((struct spinlock*)&dev->queue_spinlock)
		    ? "" : "un",
#ifdef CONFIG_MCP2210_IOCTL
	       ind, mutex_is_locked((struct mutex*)&dev->io_mutex)
		    ? "" : "un",
#endif
	       ind,
#ifdef CONFIG_MCP2210_DEBUG
	       ind, atomic_read(&dev->manager_running),
#endif
	       ind, dev->cmd_queue.next, dev->cmd_queue.prev,
	       ind, dev->cur_cmd,
	       ind, dev->delayed_list.next, dev->delayed_list.prev,
	       ind, dev->delayed_cmd,
	       ind);

	dump_ep(level, indent + 2, ".eps[EP_OUT]     = ", &dev->eps[EP_OUT]);
	dump_ep(level, indent + 2, ".eps[EP_IN]      = ", &dev->eps[EP_IN]);

	printk("%s%s.delayed_work\n"
	       "%s.dead                = %d\n"
	       "%s.debug_chatter_count = %hhu\n"
	       "%s.spi_in_flight       = %hhu\n"
	       "%s.have_config         = %hhu\n"
	       "%s.is_spi_probed       = %hhu\n"
	       "%s.is_gpio_probed      = %hhu\n"
	       "%s.is_irq_probed       = %hhu\n"
	       "%s.poll_intr           = %hhu\n"
	       "%s.poll_gpio           = %hhu\n",
	       level, ind,
	       ind, dev->dead,
	       ind, dev->debug_chatter_count,
	       ind, dev->spi_in_flight,
	       ind, dev->have_config,
	       ind, dev->is_spi_probed,
	       ind, dev->is_gpio_probed,
	       ind, dev->is_irq_probed,
	       ind, dev->poll_intr,
	       ind, dev->poll_gpio);

	dump_state(level, indent + 2, ".s = ", &dev->s);

	if (dev->config)
		dump_board_config(level, indent + 2, ".config = ", dev->config);
	else
		printk("%s%s.config = (null)\n", level, ind);

	printk("%s%s.ctl_cmd\n"
#ifdef CONFIG_MCP2210_EEPROM
	       "%s.eeprom_spinlock     = %slocked\n"
	       "%s.eeprom_state        = {TODO}\n"
	       "%s.eeprom_cache        = {TODO}\n"
#endif
	       "%s.names\n"
#ifdef CONFIG_MCP2210_GPIO
	       "%s.gpio\n"
#endif
#ifdef CONFIG_MCP2210_SPI
	       "%s.spi_master          = %-p\n"
	       "%s.chips[]             = {TODO}\n"
#endif
#ifdef CONFIG_MCP2210_IRQ
	       "%s.irq_lock            = %slocked\n"
	       "%s.nr_irqs             = %u\n"
	       "%s.irq_base            = %d\n"
	       "%s.irq_revmap[8]\n"
	       "%s.irq_type[8]\n"
	       "%s.irq_mask            = %d\n"
	       "%s.last_poll_gpio      = %ld\n"
	       "%s.last_poll_intr;     = %ld\n"
	       "%s.interrupt_event_counter = %d\n"
# ifdef CONFIG_MCP2210_GPIO
	       "%s.cmd_poll_gpio\n"
# endif
	       "%s.cmd_poll_intr\n"
#endif
	       "%s}\n",
       	       level, get_indent(indent),
#ifdef CONFIG_MCP2210_EEPROM
	       ind, spin_is_locked((struct spinlock*)&dev->eeprom_spinlock)
		    ? "" : "un",
	       ind,
	       ind,
#endif
	       ind,
#ifdef CONFIG_MCP2210_GPIO
	       ind,
#endif
#ifdef CONFIG_MCP2210_SPI
	       ind, dev->spi_master,
	       ind,
#endif
#ifdef CONFIG_MCP2210_IRQ
	       ind, mutex_is_locked((struct mutex*)&dev->irq_lock)
		    ? "" : "un",
	       ind, dev->nr_irqs,
	       ind, dev->irq_base,
	       ind, //dev->irq_revmap,
	       ind, //dev->irq_type,
	       ind, dev->irq_mask,
	       ind, dev->last_poll_gpio,
	       ind, dev->last_poll_intr,
	       ind, dev->interrupt_event_counter,

# ifdef CONFIG_MCP2210_GPIO
	       ind,
# endif
	       ind,
#endif /* CONFIG_MCP2210_IRQ */
	       ind);
}

void dump_cmd_head(const char *level, unsigned indent, const char *start, const struct mcp2210_cmd *cmd)
{
	const char *ind = get_indent(indent);

	printk("%s%s%s%p struct mcp2210_cmd {\n"
	       "%s  .dev            = %p\n"
	       "%s  .type           = %p (%s)\n"
	       "%s  .node           = %p struct list_head {.next = %p, .prev = %p}\n"
	       "%s  .time_queued    = %lu\n"
	       "%s  .time_started   = %lu\n"
	       "%s  .delay_until    = %lu\n"
	       "%s  .status         = %d\n"
	       "%s  .mcp_status     = 0x%02hhx\n"
	       "%s  .state          = %hhu (%s)\n"
	       "%s  .can_retry      = %hhu\n"
	       "%s  .delayed        = %hhu\n"
	       "%s  .nonatomic      = %hhu\n"
	       "%s  .repeat_count   = %d\n"
	       "%s  .complete       = %p\n"
	       "%s  .context        = %p\n"
	       "%s}\n",
	       level, ind, start, cmd,
	       ind, cmd->dev,
	       ind, cmd->type, cmd->type ? cmd->type->desc : "none",
	       ind, &cmd->node, cmd->node.next, cmd->node.prev,
	       ind, cmd->time_queued,
	       ind, cmd->time_started,
	       ind, cmd->delay_until,
	       ind, cmd->status,
	       ind, cmd->mcp_status,
	       ind, cmd->state, "",
	       ind, cmd->can_retry,
	       ind, cmd->delayed,
	       ind, cmd->nonatomic,
	       ind, cmd->repeat_count,
	       ind, cmd->complete,
	       ind, cmd->context,
	       ind);
}
#endif /* __KERNEL__ */

static inline char hex_nybble(u8 n)
{
	return n + (n < 10 ? '0' : 'a');
}

static void dump_ctl_msg(const char *level, unsigned indent, struct mcp2210_msg *msg, int is_req, int is_set_cmd)
{
	const char *ind = get_indent(indent);

	switch (msg->cmd) {
		u8 sub_cmd;

	case MCP2210_CMD_SET_NVRAM:
	case MCP2210_CMD_GET_NVRAM:
		sub_cmd = is_req ? msg->head.req.xet.sub_cmd
				 : msg->head.rep.xet.sub_cmd;
		switch (sub_cmd) {
		case MCP2210_NVRAM_SPI:
			goto spi_config;

		case MCP2210_NVRAM_CHIP:
			goto chip_config;

		case MCP2210_NVRAM_KEY_PARAMS: {
			const char *fmt =
			       "%s%s.body.%set_usb_params {\n"
			       "%s  .vid               = 0x%04hx\n"
			       "%s  .pid               = 0x%04hx\n"
			       "%s  .chip_power_option = 0x%02hhx\n"
			       "%s  .requested_power   = %u\n"
			       "%s}\n";
			if (is_set_cmd) {
				struct mcp2210_usb_key_params *body = &msg->
							body.set_usb_params;
				printk(fmt,
				       level, ind, "s",
				       ind, body->vid,
				       ind, body->pid,
				       ind, body->chip_power_option,
				       ind, body->requested_power * 2,
				       ind);
			} else {
				typeof(msg->body.get_usb_params) *body = &msg->
							body.get_usb_params;
				printk(fmt,
				       level, ind, "g",
				       ind, body->vid,
				       ind, body->pid,
				       ind, body->chip_power_option,
				       ind, body->requested_power * 2,
				       ind);
			}
			break;
		}

		case MCP2210_NVRAM_PROD:
		case MCP2210_NVRAM_MFG:
			printk("%s%s  no dump for message type\n", level, ind);
			break;

		};
		break;
	case MCP2210_CMD_SET_SPI_CONFIG:
	case MCP2210_CMD_GET_SPI_CONFIG:
spi_config:
		dump_spi_xfer_settings(level, indent + 2, ".body.spi = ", &msg->body.spi);
		break;

	case MCP2210_CMD_SET_CHIP_CONFIG:
	case MCP2210_CMD_GET_CHIP_CONFIG:
chip_config:
		dump_chip_settings(level, indent + 2, ".body.chip = ", &msg->body.chip);
		break;

	case MCP2210_CMD_SET_PIN_DIR:
	case MCP2210_CMD_GET_PIN_DIR:
	case MCP2210_CMD_SET_PIN_VALUE:
	case MCP2210_CMD_GET_PIN_VALUE:
		printk("%s%s.body.gpio = 0x%04hx\n",
		       level, ind, msg->body.gpio);
		break;

	default:
		printk("%s%s  no dump for message type\n", level, ind);
		break;
	};
}

void dump_mcp_msg(const char *level, unsigned indent, const char *start,
		  struct mcp2210_msg *msg, int is_req)
{
	const char *ind = get_indent(indent + 2);
	u8 cmd = msg->cmd;
	u8 status;
	const char *status_str;
	int is_set_cmd = 0;

	printk("%s%s%s%p struct mcp2210_msg {\n"
	       "%s.cmd = 0x%02hhx (%s)\n",
	       level, get_indent(indent), start, msg,
	       ind, cmd, get_cmd_str(cmd));

	if (is_req) {
		status = 0;
		status_str = NULL;
	} else {
		status = msg->head.rep.status;
		status_str = get_status_str(status);
	}

	switch (cmd) {
	case MCP2210_CMD_SET_NVRAM:
	case MCP2210_CMD_SET_SPI_CONFIG:
	case MCP2210_CMD_SET_CHIP_CONFIG:
	case MCP2210_CMD_SET_PIN_DIR:
	case MCP2210_CMD_SET_PIN_VALUE:
		is_set_cmd = 1;

		/* fall-through */
	case MCP2210_CMD_GET_NVRAM:
	case MCP2210_CMD_GET_SPI_CONFIG:
	case MCP2210_CMD_GET_CHIP_CONFIG:
	case MCP2210_CMD_GET_PIN_DIR:
	case MCP2210_CMD_GET_PIN_VALUE:
		if (is_req) {
			printk("%s%s.head.req.xet {\n"
			       "%s  .sub_cmd  = 0x%02hhx (%s)\n"
			       "%s  .reserved = 0x%04hx\n"
			       "%s}\n",
			       level, ind,
			       ind, msg->head.req.xet.sub_cmd,
				    get_sub_cmd_str(msg->head.req.xet.sub_cmd),
			       ind, msg->head.req.xet.reserved,
			       ind);

			if (is_set_cmd) {
				dump_ctl_msg(level, indent + 2, msg, is_req,
					     is_set_cmd);
			}
		} else {
			printk("%s%s.head.rep.xet {\n"
			       "%s  .status   = 0x%02hhx (%s)\n"
			       "%s  .sub_cmd  = 0x%02hhx (%s)\n"
			       "%s  .reserved = 0x%02hhx\n"
			       "%s}\n",
			       level, ind,
			       ind, status, status_str,
			       ind, msg->head.rep.xet.sub_cmd,
				    get_sub_cmd_str(msg->head.rep.xet.sub_cmd),
			       ind, msg->head.rep.xet.reserved,
			       ind);

			if (!is_set_cmd)
				dump_ctl_msg(level, indent + 2, msg, is_req,
					     is_set_cmd);
		}
		break;

	case MCP2210_CMD_READ_EEPROM:
	case MCP2210_CMD_WRITE_EEPROM:
		if (is_req) {
			printk("%s%s.head.req.eeprom {\n"
			       "%s  .addr     = 0x%02hhx\n"
			       "%s  .value    = 0x%02hhx\n"
			       "%s  .reserved = 0x%02hhx\n"
			       "%s}\n",
			       level, ind,
			       ind, msg->head.req.eeprom.addr,
			       ind, msg->head.req.eeprom.value,
			       ind, msg->head.req.eeprom.reserved,
			       ind);
		} else {
			printk("%s%s.head.rep.eeprom {\n"
			       "%s  .status = 0x%02hhx (%s)\n"
			       "%s  .addr   = 0x%02hhx\n"
			       "%s  .value  = 0x%02hhx\n"
			       "%s}\n",
			       level, ind,
			       ind, status, status_str,
			       ind, msg->head.rep.eeprom.addr,
			       ind, msg->head.rep.eeprom.value,
			       ind);
		}
		break;

	case MCP2210_CMD_GET_INTERRUPTS:
		if (is_req) {
			printk("%s%s.head.req.intr {\n"
			       "%s  .reset    = 0x%02hhx\n"
			       "%s  .reserved = 0x%04hx\n"
			       "%s}\n",
			       level, ind,
			       ind, msg->head.req.intr.reset,
			       ind, msg->head.req.intr.reserved,
			       ind);
		} else {
			printk("%s%s.head.rep.status = 0x%02hhx (%s)\n"
			       "%s.body.interrupt_event_counter = 0x%04hx\n",
			       level, ind, status, status_str,
			       ind, msg->body.interrupt_event_counter);
		}
		break;

	case MCP2210_CMD_SPI_TRANSFER:
		if (is_req) {
			printk("%s%s.head.req.spi {\n"
			       "%s  .size     = 0x%02hhx\n"
			       "%s  .reserved = 0x%04hx\n"
			       "%s}\n",
			       level, ind,
			       ind, msg->head.req.spi.size,
			       ind, msg->head.req.spi.reserved,
			       ind);
		} else {
			printk("%s%s.head.rep.spi {\n"
			       "%s  .status     = 0x%02hhx (%s)\n"
			       "%s  .size       = 0x%02hhx\n"
			       "%s  .spi_status = 0x%02hhx\n"
			       "%s}\n",
			       level, ind,
			       ind, status, status_str,
			       ind, msg->head.rep.spi.size,
			       ind, msg->head.rep.spi.spi_status,
			       ind);
		}
		break;

	case MCP2210_CMD_GET_STATUS:
	case MCP2210_CMD_SPI_CANCEL:
		if (!is_req) {
			printk("%s%s.head.rep.spi_status {\n"
			       "%s  .status                      = 0x%02hhx\n"
			       "%s  .release_external_req_status = 0x%02hhx\n"
			       "%s  .current_bus_owner           = 0x%02hhx\n"
			       "%s}\n"
			       "%s.body.spi_status {\n"
			       "%s  .num_pwd_gusses = 0x%02hhx\n"
			       "%s  .pwd_gussed     = 0x%02hhx\n"
			       "%s}\n",
			       level, ind,
			       ind, msg->head.rep.spi_status.status,
			       ind, msg->head.rep.spi_status
				    .release_external_req_status,
			       ind, msg->head.rep.spi_status.current_bus_owner,
			       ind,
			       ind,
			       ind, msg->body.spi_status.num_pwd_guesses,
			       ind, msg->body.spi_status.pwd_guessed,
			       ind);
		}
		break;

	case MCP2210_CMD_SPI_RELEASE:
		if (!is_req)
			printk("%s%s.head.rep.status = 0x%02hhx\n",
			       level, ind, msg->head.rep.status);
		break;

	};

	printk("%s%s}\n", level, get_indent(indent));
}

#ifdef __KERNEL__
void dump_spi_transfer(const char *level, unsigned indent, const char *start,
		       const struct spi_transfer *xfer)
{
	const char *ind = get_indent(indent);

	printk("%s%s%s%p struct spi_transfer {\n"
	       "%s  .tx_buf        = %p\n"
	       "%s  .rx_buf        = %p\n"
	       "%s  .len           = %u\n"
	       "%s  .tx_dma        = %pad\n"
	       "%s  .rx_dma        = %pad\n"
	       "%s  .cs_change     = %u\n"
	       "%s  .bits_per_word = %hhu\n"
	       "%s  .word_delay_usecs = %hu\n"
	       "%s  .speed_hz      = %u\n"
	       "%s  .transfer_list = {.next = %p, .prev = %p}\n"
	       "%s}\n",
	       level, ind, start, xfer,
	       ind, xfer->tx_buf,
	       ind, xfer->rx_buf,
	       ind, xfer->len,
	       ind, &xfer->tx_dma,
	       ind, &xfer->tx_dma,
	       ind, xfer->cs_change,
	       ind, xfer->bits_per_word,
	       ind, xfer->word_delay.value,
	       ind, xfer->speed_hz,
	       ind, xfer->transfer_list.next, xfer->transfer_list.prev,
	       ind);
}

void dump_spi_message(const char *level, unsigned indent, const char *start,
		      const struct spi_message *msg)
{
	const char *ind = get_indent(indent);

	printk("%s%s%s%p struct spi_message {\n"
	       "%s  .transfers     = {.next = %p, .prev = %p}\n"
	       "%s  .spi           = %p\n"
	       "%s  .is_dma_mapped = %u\n"
	       "%s  .complete      = %p\n"
	       "%s  .context       = %p\n"
	       "%s  .actual_length = %u\n"
	       "%s  .status        = %d\n"
	       "%s  .queue         = {.next = %p, .prev = %p}\n"
	       "%s  .state         = %p\n"
	       "%s}\n",
	       level, ind, start, msg,
	       ind, msg->transfers.next, msg->transfers.prev,
	       ind, msg->spi,
	       ind, msg->is_dma_mapped,
	       ind, msg->complete,
	       ind, msg->context,
	       ind, msg->actual_length,
	       ind, msg->status,
	       ind, msg->queue.next, msg->queue.prev,
	       ind, msg->state,
	       ind);
}


void dump_spi_device(const char *level, unsigned indent, const char *start,
		     const struct spi_device *spi_dev)
{
	const char *ind = get_indent(indent);

	printk("%s%s%s%p struct spi_device {\n"
	       "%s  .dev\n"
	       "%s  .master           = %p\n"
	       "%s  .max_speed_hz     = %u\n"
	       "%s  .chip_select      = 0x%02hhx\n"
	       "%s  .mode             = 0x%02hhx\n"
	       "%s  .bits_per_word    = 0x%02hhx\n"
	       "%s  .irq              = %d\n"
	       "%s  .controller_state = %p\n"
	       "%s  .controller_data  = %p\n"
	       "%s  .modalias         = %s\n"
	       "%s}\n",
	       level, ind, start, spi_dev,
	       ind,
	       ind, spi_dev->master,
	       ind, spi_dev->max_speed_hz,
	       ind, spi_dev->chip_select,
	       ind, spi_dev->mode,
	       ind, spi_dev->bits_per_word,
	       ind, spi_dev->irq,
	       ind, spi_dev->controller_state,
	       ind, spi_dev->controller_data,
	       ind, spi_dev->modalias,
	       ind);
}

void dump_cmd_ctl(const char *level, unsigned indent, const char *start,
		  const struct mcp2210_cmd *cmd_head)
{
	struct mcp2210_device *dev = cmd_head->dev;
	struct mcp2210_cmd_ctl *cmd = (struct mcp2210_cmd_ctl *)cmd_head;
	const char *ind = get_indent(indent);

	printk("%s%s%s%p struct mcp2210_cmd_ctl {\n",
	       level, ind, start, cmd);

	dump_cmd_head(level, indent + 2, ".head             = ", cmd_head);
	dump_mcp_msg(level, indent + 2, ".req              = ", &cmd->req, true);

	printk("%s%s  .pin               = %hhu\n"
	       "%s  .is_mcp_endianness = %hhu\n"
	       "%s}\n",
	       level, ind, cmd->pin,
	       ind, cmd->is_mcp_endianness,
	       ind);

	if (cmd_head->state == MCP2210_STATE_COMPLETE) {
		dump_mcp_msg(level, indent, "Control command response = ",
			     dev->eps[EP_IN].buffer, false);

	}

	return;
}

void dump_cmd_spi(const char *level, unsigned indent, const char *start,
		  const struct mcp2210_cmd *cmd_head)
{
	//struct mcp2210_device *dev = cmd_head->dev;
	struct mcp2210_cmd_spi_msg *cmd = (void *)cmd_head;
	const char *ind = get_indent(indent);

	printk("%s%s%s%p struct mcp2210_cmd_type_spi {\n",
	       level, ind, start, cmd);

	dump_cmd_head(level, indent + 2, ".head = ", cmd_head);
	dump_spi_device(level, indent + 2, ".spi = ", cmd->spi);
	dump_spi_message(level, indent + 2, ".msg = ", cmd->msg);
	dump_spi_transfer(level, indent + 2, ".xfer = ", cmd->xfer);

	printk("%s  .pos             = %u\n"
	       "%s  .pending_unacked = %hu\n"
	       "%s  .pending_bytes   = %hu\n"
	       "%s  .busy_count      = %u\n"
	       "%s  .spi_in_flight   = %hhu\n",
	       level, cmd->pos,
	       ind, cmd->pending_unacked,
	       ind, cmd->pending_bytes,
	       ind, cmd->busy_count,
	       ind, cmd->spi_in_flight);

	if (cmd->ctl_cmd)
		dump_cmd_ctl(level, indent + 2, ".ctl_cmd = ",
			     &cmd->ctl_cmd->head);
	else
		printk("%s%s  .ctl_cmd         = (null)\n", level, ind);

	printk("%s%s}\n", level, ind);
}

void dump_cmd_eeprom(const char *level, unsigned indent, const char *start,
		     const struct mcp2210_cmd *cmd_head)
{
	struct mcp2210_cmd_eeprom *cmd = (void *)cmd_head;
	const char *ind = get_indent(indent + 2);

	printk("%s%s%s%p struct mcp2210_cmd_eeprom {\n",
	       level, get_indent(indent), start, cmd);

	dump_cmd_head(level, indent + 2, ".head = ", cmd_head);

	printk("%s%s.op        = 0x%02hhx\n"
	       "%s.addr      = 0x%02hhx\n"
	       "%s.zero_tail = 0x%02hhx\n"
	       "%s.size      = 0x%02x\n"
	       "%s}\n",
	       level, ind, cmd->op,
	       ind, cmd->addr,
	       ind, cmd->zero_tail,
	       ind, cmd->size,
	       get_indent(indent));
}

void _dump_cmd(const char *level, unsigned indent, const char *start,
	       const struct mcp2210_cmd *cmd_head)
{
	const struct mcp2210_cmd_type *type = cmd_head->type;

	/* hopefully, this can be compiled as a simple compare & jump into
	 * either type->dump or dump_cmd_head */
	if (type && type->dump)
		type->dump(level, indent, start, cmd_head);
	else
		dump_cmd_head(level, indent, start, cmd_head);
}

#endif /* __KERNEL__ */
#endif /* CONFIG_MCP2210_DEBUG_VERBOSE */

#if defined(__KERNEL__) && defined(CONFIG_MCP2210_DEBUG)
void _mcp2210_dump_urbs(struct mcp2210_device *dev, const char *level,
			int urb_mask)
{
	unsigned i;

	for (i = 0; i < 2; ++i) {
		if (urb_mask & 1 << i) {
			char buf[11];
			scnprintf(buf, sizeof(buf), "URB %s: ", urb_dir_str[i]);
			print_mcp_msg(level, buf, dev->eps[i].buffer);
		}
	}
}
#endif
