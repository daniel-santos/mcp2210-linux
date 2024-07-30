/*
 * MCP2210 userspace utility
 * Copyright (c) 2013-2017 Daniel Santos <daniel.santos@pobox.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
//#include <sys/time.h>
#include <stdint.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <linux/spi/spidev.h>
#include <limits.h>

#include "mcp2210-user.h"
#include "settings.h"

static const char *argv0;

static void show_usage(void);

static int eeprom_read(int argc, char *argv[]);
static int eeprom_write(int argc, char *argv[]);
static int eeprom_read_write(int is_read, int argc, char *argv[]);

static const char *hex_digits="0123456789abcdef";

static inline char hex_digit(int v) {
	return hex_digits[v & 0xf];
}

void dump_hex(FILE *dest, const u8* src, size_t size) {
	const unsigned byte_per_row = 32;
	const unsigned space_every = 4;
	const size_t ascii_start = byte_per_row * 2 + (byte_per_row + space_every - 1) / space_every + 1;
	char buf[128];
	unsigned i;

	assert(sizeof(buf) >= ascii_start + byte_per_row + 2);
//	printf("%lu >= %lu\n", sizeof(buf), ascii_start + byte_per_row + 2);

	memset(buf, 0, sizeof(buf));

	buf[ascii_start - 1] = '|';
	buf[ascii_start + byte_per_row ] = '|';
	buf[ascii_start + byte_per_row + 1] = 0;
	for (i = 0; i < size;) {
		u8 b = src[i];
		unsigned col = i % byte_per_row;
		size_t off = col * 2 + col / 4;

		buf[off++] = hex_digit(b >> 4);
		buf[off++] = hex_digit(b);
		buf[off++] = ' ';
		buf[ascii_start + col] = (isprint(b)) ? b : '.';

		++i;
		if ((!(i % byte_per_row))) {
			fprintf(dest, "%s\n", buf);
		} else if (i == size) {
			ssize_t fill1_size = ascii_start - off - 1;
			ssize_t fill2_size = byte_per_row - col - 1;

			if (fill1_size > 0)
				memset(&buf[off], ' ', fill1_size);
			if (fill2_size > 0)
				memset(&buf[ascii_start + col + 1], ' ', fill2_size);
			fprintf(dest, "%s\n", buf);
			break;
		}
	}
}


static struct config {
	bool verbose;
	const char *name;
	const char *cmd;
	const char *target;
	const char *filein;
	const char *fileout;
	struct {
		u8 addr;
		u16 size;
	} eeprom;
	struct {
		const char *name;
		u8 mode;
		u32 speed_hz;
		u16 delay_usecs;
		u8 bits_per_word;
	} spi;
} config = {
	.verbose = 1,
	.name = "/dev/usb2spi_bridge0",
	.cmd = NULL,
	.target = NULL,
	.filein = NULL,
	.fileout = NULL,
	.eeprom = {
		.addr = 0,
		.size = 0x100,
	},
	.spi = {
		.name		= "/dev/spidev0.0",
		.mode		= 3,
		.speed_hz	= 100 * 1000,
		.delay_usecs	= 0,
		.bits_per_word	= 8,
	},
};
/**
 * Determine base by prefix and offset to number. Uses standard rules
 * (expressed in regex below):
 * 0[xX][0-9a-fA-F]+ denotes a hexidecimal number
 * 0[0-7]+           denotes an octal number
 * (0|[1-9][0-9]*)   denotes a decimal number
 */
int get_param_base_and_start(const char **str)
{
	assert(*str);

	/* Parse an "0x", "0X" or "0" -prefixed string, but not the string
	 * "0" (which will be treated as base ten). */
	if (**str == '0' && *(*str + 1)) {
		++*str;
		if (**str == 'x' || **str == 'X') {
			++*str;
			return 16;
		} else {
			return 8;
		}
	} else {
		return 10;
	}
}

static void _get_ull_param(const char *str, unsigned long long *dest,
			   size_t size,
			   unsigned long long min,
			   unsigned long long max)
{
	const char *start = str;
	char *endptr;
	int base = get_param_base_and_start(&start);

	errno = 0;
	if (size == 8)
		*dest = strtoull(start, &endptr, base);
	else
		*dest = strtoul(start, &endptr, base);

	if (errno)
		fatal_error("bad number: %s", str);

	if (*dest < min || *dest > max) {
		errno = ERANGE;
		fatal_error("%s should be between %llu and %llu", str, min, max);
	}
}

/**
 * @returns zero on success, non-zero if the string didn't represent a clean
 *          number
 */
static __always_inline int _get_unsigned_param(const char *str, void *dest,
					       size_t size,
					       unsigned long long min,
					       unsigned long long max)
{
	unsigned long long valull;

	/* debug sanity-check sizes */
	assert(min <= max);

	if (size == sizeof(unsigned char))
		assert(max <= UCHAR_MAX);
	else if (size == sizeof(unsigned short))
		assert(max <= USHRT_MAX);
	else if (size == sizeof(unsigned int))
		assert(max <= UINT_MAX);
	else if (size == sizeof(unsigned long))
		assert(max <= ULONG_MAX);
	else if (size == sizeof(unsigned long long))
		assert(max <= ULLONG_MAX);

	_get_ull_param(str, &valull, size, min, max);

	if (size == sizeof(unsigned char))
		*(unsigned char *)dest = valull;
	else if (size == sizeof(unsigned short))
		*(unsigned short *)dest = valull;
	else if (size == sizeof(unsigned int))
		*(unsigned int *)dest = valull;
	else if (size == sizeof(unsigned long))
		*(unsigned long *)dest = valull;
	else if (size == sizeof(unsigned long long))
		*(unsigned long long *)dest = valull;

	return 0;
}

#define get_unsigned_param(str, dest, min, max) \
	_get_unsigned_param((str), (dest), sizeof(*(dest)), (min), (max))



static inline u8 get_bool_param(const char *str)
{
	u8 dest;
	get_unsigned_param(str, &dest, 0, 1);
	return dest;
}



/******************************************************************************
 *
 */

int get_input_data(void *dest, size_t size, int short_read_ok) {
	const char *file_name = config.filein;
	int use_stdin = !file_name || !strcmp(file_name, "-");
	int fd;
	ssize_t ret;

	fprintf(stderr, "get_input_data dest %p, size %lu\n", dest, (unsigned long) size);

	if (use_stdin)
		fd = fileno(stdin);
	else if ((fd = open(file_name, O_RDONLY)) < 0) {
		perror("open");
		return errno;
	}

	ret = read(fd, dest, size);
	if (ret < 0) {
		perror("read(): reading input");
		ret = errno;
	} else if ((size_t)ret != size && !short_read_ok) {
		fprintf(stderr, "Short input: expected %lu bytes, got %ld.\n",
			(unsigned long)size, (long)ret);
		ret = -EOVERFLOW;
	}

	if (!use_stdin)
		close(fd);

	return ret;
}

/* what a crappy fn name... */
int get_input_data2(void **dest, size_t size) {
	int ret;

	if (!size)
		size = 0x8000;

	*dest = malloc(size);
	if (!*dest)
		return -ENOMEM;

	ret = get_input_data(*dest, size, 1);

	if (ret < 0) {
		free(*dest);
		*dest = NULL;
	} else {
		size = (size_t)ret;
		*dest = realloc(*dest, size);
	}

	return ret;
}


int put_output_data(const void *src, size_t size, int append) {
	const char *file_name = config.fileout;
	int use_stdout = !file_name || !strcmp(file_name, "-");
	int fd;
	ssize_t ret;
	int flags = O_RDWR | O_CREAT | (append ? O_APPEND : O_TRUNC );

	fprintf(stderr, "put_output_data src: %p, size: %lu\n", src, (unsigned long) size);

	if (use_stdout) {
		fd = fileno(stdout);
		if (isatty(fd)) {
			fprintf(stderr, "Cowardly refusing to write binary data to terminal\n");
			return -EPERM;
		}
	} else if ((fd = open(file_name, flags, S_IRUSR | S_IWUSR)) < 0) {
		perror("open");
		return errno;
	}

	ret = write(fd, src, size);
	if (ret < 0) {
		perror("write(): writing output");
		ret = errno;
	} else if ((size_t)ret != size) {
		fprintf(stderr, "put_output_data: Failed to write all data: have %lu bytes, only wrote %ld.\n",
			(unsigned long)size, (long)ret);
		ret = -EOVERFLOW;
	} else
		ret = 0;

	if (!use_stdout)
		close(fd);

	return ret;
}



int get_config(int argc, char *argv[]) {
	struct mcp2210_ioctl_data *data;
	struct mcp2210_ioctl_data_config *cfg;
	const size_t struct_size = IOCTL_DATA_CONFIG_SIZE + 0x100;
	int ret = 0;
	u8 buf[0x100];

	memset(buf, 0, sizeof(buf));

	data = malloc(struct_size);
	if (!data) {
		fatal_error("no mem");
		return -ENOMEM;
	}

	memset(data, 0, struct_size);
	data->struct_size = struct_size;
	cfg = &data->body.config;

	fprintf(stderr, "offset = %u\n", (uint)offsetof(struct mcp2210_ioctl_data, body.config));

	ret = mcp2210_do_ioctl(config.name, MCP2210_IOCTL_CONFIG_GET, data);
	if (ret)
		goto exit_free;

	dump_state("", 0, "state = ", &cfg->state);
	if (cfg->have_config)
		dump_board_config("", 0, ".config = ", &cfg->config);

#if 0
	ret = creek_encode(&cfg->config, &cfg->state.power_up_chip_settings, buf, sizeof(buf));
	if (ret < 0) {
		errno = -ret;
		fatal_error("creek_encode");
	}

	ret = (ret + 7) / 8;

	ret = put_output_data(buf, ret, 0);

	if (ret > 0) {
		fprintf(stderr, "wrote %d bytes\n", ret);
		ret = 0;
	}
#endif
exit_free:
	free(data);
	return ret;
}

#ifndef CONFIG_MCP2210_CREEK
static int encode(int argc, char *argv[]) {
	fprintf(stderr, "Feature disabled (set CONFIG_MCP2210_CREEK to enable)\n");
	return -EPERM;
}

static int decode(int argc, char *argv[]) {
	return encode(argc, argv);
}

#else /* CONFIG_MCP2210_CREEK */

static int encode(int argc, char *argv[]) {
	struct mcp2210_board_config *board_config;
	u8 buf[0x100];
	uint size;
	int ret = 0;

	memset(buf, 0, sizeof(buf));
	board_config = copy_board_config(NULL, &my_board_config, 0);
	if (!board_config) {
		errno = ENOMEM;
		perror("copy_board_config");
		goto exit_free;
	}

	ret = creek_encode(board_config, &my_chip_settings, buf, sizeof(buf),
			   2);
	if (ret < 0) {
		errno = -ret;
		perror("creek_encode");
		goto exit_free;
	}

	size = (ret + 7) / 8;
	fprintf(stderr, "encoded into %d bits (%u bytes)\n", ret, size);

	ret = put_output_data(buf, size, 0);

exit_free:
	free(board_config);
	return ret;
}

static int decode(int argc, char *argv[]) {
	struct mcp2210_board_config *board_config;
	u8 buf[0x100];
	int ret;

	ret = get_input_data(buf, sizeof(buf), 1);
	if (ret < 0)
		return ret;

	board_config = creek_decode(&my_chip_settings, buf, (uint)ret, 0);
	if (IS_ERR_VALUE(board_config)) {
		ret = PTR_ERR(board_config);
		fprintf(stderr, "decodeing creek config failed: ");
		errno = -ret;
		perror("creek_decode");
		return 0;
	}

	dump_board_config(KERN_INFO, 0, "board_config = ", board_config);

	free(board_config);
	return 0;
}
#endif /* CONFIG_MCP2210_CREEK */

enum settings_mask {
	SETTINGS_CHIP_SETTINGS		= 1 << 0,
	SETTINGS_POWER_UP_CHIP_SETTINGS	= 1 << 1,
	SETTINGS_SPI_SETTINGS		= 1 << 2,
	SETTINGS_POWER_UP_SPI_SETTINGS	= 1 << 3,
	SETTINGS_USB_KEY_PARAMS		= 1 << 4,
	SETTINGS_BOARD_CONFIG		= 1 << 5,
};

int set_config(int argc, char *argv[]) {
	struct mcp2210_ioctl_data *data;
	struct mcp2210_ioctl_data_config *cfg;
	size_t struct_size = IOCTL_DATA_CONFIG_SIZE;
	size_t strings_size = 0;
	int ret = 0;

	u8 mask;

	get_unsigned_param(argv[0], &mask, 0, 0x3f);

	if (mask & SETTINGS_BOARD_CONFIG) {
		unsigned i;

		for (i = 0; i < MCP2210_NUM_PINS; ++i) {
			const struct mcp2210_pin_config *pin = &my_board_config.pins[i];
			if (pin->name && *pin->name)
				strings_size += strlen(pin->name) + 1;
			if (pin->modalias && *pin->modalias)
				strings_size += strlen(pin->modalias) + 1;
		}
		struct_size += strings_size;
	}

	data = malloc(struct_size);
	if (!data) {
		fatal_error("no mem");
		return -ENOMEM;
	}

	memset(data, 0, struct_size);
	data->struct_size = struct_size;
	cfg = &data->body.config;

	if (mask & SETTINGS_CHIP_SETTINGS) {
		cfg->state.have_chip_settings = 1;
		memcpy(&cfg->state.chip_settings,
		       &my_chip_settings,
		       sizeof(my_chip_settings));
	}

	if (mask & SETTINGS_POWER_UP_CHIP_SETTINGS) {
		cfg->state.have_power_up_chip_settings = 1;
		memcpy(&cfg->state.power_up_chip_settings,
		       &my_power_up_chip_settings,
		       sizeof(my_power_up_chip_settings));
	}

	if (mask & SETTINGS_SPI_SETTINGS) {
		cfg->state.have_spi_settings = 1;
		memcpy(&cfg->state.spi_settings,
		       &my_spi_settings,
		       sizeof(my_spi_settings));
	}

	if (mask & SETTINGS_POWER_UP_SPI_SETTINGS) {
		cfg->state.have_power_up_spi_settings = 1;
		memcpy(&cfg->state.power_up_spi_settings,
		       &my_power_up_spi_settings,
		       sizeof(my_power_up_spi_settings));
	}

	if (mask & SETTINGS_USB_KEY_PARAMS) {
		cfg->state.have_usb_key_params = 1;
		memcpy(&cfg->state.usb_key_params,
		       &my_usb_key_params,
		       sizeof(my_usb_key_params));
	}

	if (mask & SETTINGS_BOARD_CONFIG) {
		cfg->have_config = 1;
		cfg->config.strings_size = strings_size;
		copy_board_config(&cfg->config, &my_board_config, 0);
	}

	dump_state("", 0, "state = ", &cfg->state);
	if (cfg->have_config)
		dump_board_config("", 0, ".config = ", &cfg->config);

	ret = mcp2210_do_ioctl(config.name, MCP2210_IOCTL_CONFIG_SET, data);

	free(data);
	return ret;
}

static int eeprom_read(int argc, char *argv[]) {
	return eeprom_read_write(1, argc, argv);
}

static int eeprom_write(int argc, char *argv[]) {
	return eeprom_read_write(0, argc, argv);
}

static int eeprom_read_write(int is_read, int argc, char *argv[]) {
	struct mcp2210_ioctl_data *data;
	struct mcp2210_ioctl_data_eeprom *eeprom;
	const size_t struct_size = sizeof(struct mcp2210_ioctl_data);
	uint addr = 0;
	uint size = 0;
	int i;
	int ret = 0;

	for (i = 0; i < argc; ++i) {
		const char *arg = argv[i];
		if (!strncmp(arg, "addr=", 5)) {
			get_unsigned_param(&arg[5], &addr, 0, 0xff);
		} else if (!strncmp(arg, "size=", 5)) {
			get_unsigned_param(&arg[5], &size, 1, 0x100);
		} else {
			fprintf(stderr, "what you talkin' 'bout Wilis? %s\n", arg);
			show_usage();
			exit (-EINVAL);
		}
	}

	fprintf(stderr, "eeprom_read_write: addr %u, size %u\n", addr, size);

	data = malloc(struct_size);
	if (!data) {
		fatal_error("no mem");
		return -ENOMEM;
	}

	memset(data, 0, struct_size);
	data->struct_size = struct_size;
	eeprom = &data->body.eeprom;
	eeprom->addr = addr;
	eeprom->size = size;
	eeprom->is_read = is_read;

	if (!is_read) {
		ret = get_input_data(eeprom->data, eeprom->size, 0);
		if (ret != eeprom->size)
			goto exit_free;
	}

//	config.fileout = "a.out";
//	ret = put_output_data(data, struct_size);

	ret = mcp2210_do_ioctl(config.name, MCP2210_IOCTL_EEPROM, data);

	if (is_read) {
//		config.fileout = "b.out";
		ret = put_output_data(eeprom->data, eeprom->size, 0);
//		ret = put_output_data(data, struct_size);
	}

exit_free:
	free(data);
	return ret;
}

struct spi_msg {
	size_t struct_size;
	u8 *buf;
	size_t buf_size;
	unsigned num_xfers;
	struct spi_ioc_transfer xfers[0];
};

struct spi_msg *parse_msgs(int argc, char *argv[]) {
	size_t size;
	const char *p;
	struct spi_msg *msg = NULL;
	unsigned next_buf_off;
	struct spi_ioc_transfer *xfer;
	struct spi_msg proto;


	if (argc != 1) {
		fprintf(stderr, "too many arguments\n");
		return ERR_PTR(-EINVAL);
	}

	if (!(**argv)) {
		fprintf(stderr, "msg is an string empty\n");
		return ERR_PTR(-EINVAL);
	}

	/* Parse the message string and determine the number of messages as
	 * well as the sum of message sizes */
	memset(&proto, 0, sizeof(proto));
	p = *argv;
	size = 0;
	while (1) {
		u8 val;

		if (*p == ',' || !*p) {
			++proto.num_xfers;
			if (!size)
				fprintf(stderr, "WARNING: zero-sized transfer\n");

			proto.buf_size += size;

			if (!*p || !p[1])
				break;

			size = 0;
			++p;
			continue;
		}

		if (isspace(*p)) {
			++p;
			continue;
		}

		if (!strncasecmp(p, "cs", 2)) {
			p += 2;
			continue;
		}

		if (sscanf(p, "%02hhx", &val) != 1)
			goto exit_bad_fmt;

		p += 2;
		++size;
	}

	proto.buf_size *= 2;			/* separate tx & rx buffers */
	proto.struct_size = sizeof(struct spi_msg)
		    + sizeof(struct spi_ioc_transfer) * proto.num_xfers
		    + proto.buf_size;

	/* allocate & init msg object */
	if (!(msg = malloc(proto.struct_size)))
		return ERR_PTR(-ENOMEM);
	memset(msg, 0, proto.struct_size);

	msg->struct_size = proto.struct_size;
	msg->buf	  = &((u8 *)msg)[proto.struct_size - proto.buf_size];
	msg->buf_size	  = proto.buf_size;
	msg->num_xfers	  = proto.num_xfers;


	/* Parse again and init each xfer (minus tx_buf contents) */
	p = *argv;
	size = 0;
	next_buf_off = 0;
	xfer = msg->xfers;
	xfer->cs_change = 0;
	while (1) {
		u8 val;

		if (*p == ',' || !*p) {
			xfer->tx_buf	    = (ulong)&msg->buf[next_buf_off];
			next_buf_off	   += size;
			xfer->rx_buf	    = (ulong)&msg->buf[next_buf_off];
			next_buf_off	   += size;
			xfer->len	    = size;
			xfer->speed_hz	    = config.spi.speed_hz;
			xfer->delay_usecs   = config.spi.delay_usecs;
			xfer->bits_per_word = config.spi.bits_per_word;
			++xfer;
			xfer->cs_change	    = 0;

			if (!*p || !p[1])
				break;
			size = 0;
			++p;
			continue;
		}

		if (isspace(*p)) {
			++p;
			continue;
		}

		if (!strncasecmp(p, "cs", 2)) {
			xfer->cs_change = 1;
			p += 2;
			continue;
		}

		if (sscanf(p, "%02hhx", &val) != 1)
			goto exit_bad_fmt;

		p += 2;
		++size;
	};
	assert(xfer == &msg->xfers[msg->num_xfers]);
	assert(next_buf_off == msg->buf_size);

	/* Parse a third time and populate contents of each xfer tx_buf */
	p = *argv;
	size = 0;
	next_buf_off = 0;
	xfer = msg->xfers;
	while (1) {
		u8 val;

		if (*p == ',' || !*p) {
			assert(xfer->len == size);

			if (!*p || !p[1])
				break;

			++xfer;
			size = 0;
			++p;
			continue;
		}

		if (isspace(*p)) {
			++p;
			continue;
		}

		if (!strncasecmp(p, "cs", 2)) {
			p += 2;
			continue;
		}

		if (sscanf(p, "%02hhx", &val) != 1)
			goto exit_bad_fmt;

		p += 2;
		((u8*)(ulong)(xfer->tx_buf))[size++] = val;
	};

	return msg;

exit_bad_fmt:
	fprintf(stderr, "bad msg format");
	if (msg)
		free (msg);
	return ERR_PTR(-EINVAL);
}

static __always_inline void _spidev_set(int fd, unsigned long request, void *value, u8 size, const char *desc) {
	const unsigned long magic	= _IOC_TYPE(request);
	const unsigned long ordinal	= _IOC_NR(request);
	const unsigned long sz		= _IOC_SIZE(request);
	const unsigned long read_req	= _IOC(_IOC_READ, magic, ordinal, sz);
	const unsigned long write_req	= _IOC(_IOC_WRITE, magic, ordinal, sz);
	union {
		u8 u8;
		u16 u16;
		u32 u32;
		u64 u64;
	} val;
	u64 cur_val64;
	u64 target_val64;

	assert(sz == size);

	if (ioctl(fd, read_req, &val) < 0)
		fatal_error("failed to read %s", desc);

	switch (size) {
	case 1: cur_val64 = val.u8;  target_val64 = *(u8 *)value; break;
	case 2: cur_val64 = val.u16; target_val64 = *(u16*)value; break;
	case 4: cur_val64 = val.u32; target_val64 = *(u32*)value; break;
	case 8: cur_val64 = val.u64; target_val64 = *(u64*)value; break;
	default: assert(0);
	};

	/* only call the spi_setup if needed */
	if (cur_val64 != target_val64) {
		if (ioctl(fd, write_req, value) < 0)
			fatal_error("failed to set %s", desc);
	}
}

struct spi_msg *get_msg_from_file(void) {
	struct spi_msg *msg;
	void *data;
	u8 *tx_buf;
	u8 *rx_buf;
	size_t data_size;
	size_t struct_size;
	int ret;

	ret = get_input_data2(&data, 0x8000);

	if (ret < 0)
		return NULL;

	data_size = (size_t)ret;

	/* the spi_msg, a struct spi_ioc_transfer payload and then two buffers
	 * for the data */
	struct_size = sizeof(*msg)
		    + sizeof(struct spi_ioc_transfer)
		    + data_size * 2;

	msg = malloc(struct_size);
	if (!msg)
		goto error_free;

	memset(msg, 0, struct_size);

	tx_buf = &((u8*)msg)[struct_size - data_size * 2];
	rx_buf = &((u8*)msg)[struct_size - data_size];

	msg->struct_size	  = struct_size;
	msg->buf		  = data;
	msg->buf_size		  = (size_t)ret;
	msg->num_xfers		  = 1;
	msg->xfers->tx_buf	  = (ulong)memcpy(tx_buf, data, data_size);
	msg->xfers->rx_buf	  = (ulong)memset(rx_buf, 0, data_size);
	msg->xfers->len		  = data_size;
	msg->xfers->speed_hz	  = config.spi.speed_hz;
	msg->xfers->delay_usecs	  = config.spi.delay_usecs;
	msg->xfers->bits_per_word = config.spi.bits_per_word;
	msg->xfers->cs_change	  = 0;

	return msg;

error_free:
	free(data);
	free(msg);
	return NULL;
}

#define spidev_set(fd, request, ignore_zero_value, value, desc) 	\
	do {								\
		if (ignore_zero_value && !(*(value)))			\
			break;						\
		_spidev_set(fd, request, value, sizeof(*value), desc);	\
	} while (0)

static int spidev_send(int argc, char *argv[]) {
	int ret = 0;
	int fd;
	unsigned i;
	struct spi_msg *msg;

	if (argc) {
		msg = parse_msgs(argc, argv);
		if (IS_ERR(msg)) {
			if (PTR_ERR(msg) == -EINVAL) {
				show_usage();
				return -1;
			}
			errno = -PTR_ERR(msg);
			fatal_error("failed to parse messages");
		}
	} else
		msg = get_msg_from_file();

	if (!msg)
		return -ENOMEM;

	for (i = 0; i < msg->num_xfers; ++i) {
		fprintf(stderr, "request %d:\n", i);
		dump_hex(stderr, (u8*)(ulong)msg->xfers[i].tx_buf, msg->xfers[i].len);
	}

	if ((fd = open(config.spi.name, O_RDWR)) < 0)
		fatal_error("open failed for %s", config.spi.name);

	spidev_set(fd, SPI_IOC_RD_MODE, 0, &config.spi.mode, "spi mode");
	spidev_set(fd, SPI_IOC_RD_MAX_SPEED_HZ, 1, &config.spi.speed_hz, "spi max speed");
	spidev_set(fd, SPI_IOC_RD_BITS_PER_WORD, 1, &config.spi.bits_per_word, "spi bits per word");

	if (ioctl(fd, SPI_IOC_MESSAGE(msg->num_xfers), msg->xfers) < 0)
		fatal_error("spi message failed");

	for (i = 0; i < msg->num_xfers; ++i) {
		const u8 *data = (u8*)(ulong)msg->xfers[i].rx_buf;
		size_t len = msg->xfers[i].len;
		fprintf(stderr, "response %d:\n", i);
		dump_hex(stderr, data, len);
		put_output_data(data, len, i);
	}
	close(fd);
	free (msg);
	return ret;
}

struct command {
	const char *name;
	int min_args;
	int max_args;
	const struct command *sub_cmd;
	int (*func)(int argc, char *argv[]);
};

static int dispatch(int argc, char *argv[], const struct command cmd_list[]) {
	const struct command *cmd;

	if (argc < 1) {
		fprintf(stderr, "Not enough arguments supplied\n");
		show_usage();
		return -EINVAL;
	}

	for (cmd = cmd_list; cmd->name; ++cmd) {
		if (!strcmp(cmd->name, *argv)) {
			if (argc < cmd->min_args) {
				fprintf(stderr, "too few arguments supplied for command %s, expected %u, got %u\n", cmd->name, cmd->min_args, argc);
				show_usage();
				return -EINVAL;
			}
			--argc;
			++argv;
			return cmd->sub_cmd
			     ? dispatch(argc, argv, cmd->sub_cmd)
			     : cmd->func(argc, argv);
		}
	}

	fprintf(stderr, "Invalid argument `%s'\n", argv[0]);
	show_usage();
	return -EINVAL;
}

static int not_yet_implemented(int argc, char *argv[]) {
	fprintf(stderr, "not yet implemented\n");
	return 0;
}


static const struct command sub_cmd_list_get[] = {
	{
		.name = "config",
		.func = get_config,
	}, {}
};

static const struct command sub_cmd_list_set[] = {
	{
		.name = "config",
		.min_args = 1,
		.max_args = 1,
		.func = set_config,
	}, {}
};

static const struct command sub_cmd_list_eeprom[] = {
	{
		.name = "read",
		.func = eeprom_read,
	}, {
		.name = "write",
		.func = eeprom_write,
	}, {}
};

static const struct command sub_cmd_list_test[] = {
	{
		.name = "bitstream",
		.func = test_bit_stream,
	}, {
		.name = "config",
		.func = test_encoding,
	}, {}
};

static const struct command command_list[] = {
	{
		.name	  = "get",
		.min_args = 1,
		.sub_cmd  = sub_cmd_list_get,
	}, {
		.name	  = "set",
		.min_args = 1,
		.sub_cmd  = sub_cmd_list_set,
	}, {
		.name	  = "encode",
		.min_args = 1,
		.func     = encode,
	}, {
		.name	  = "decode",
		.min_args = 1,
		.func     = decode,
	}, {
		.name	  = "eeprom",
		.min_args = 1,
		.sub_cmd  = sub_cmd_list_eeprom,
	}, {
		.name	  = "test",
		.min_args = 1,
		.sub_cmd  = sub_cmd_list_test,
	}, {
		.name	  = "msg",
		.min_args = 0,
		.func	  = not_yet_implemented,
	}, {
		.name	  = "spi",
		.min_args = 1,
		.func	  = spidev_send,
	}, { },

};

static void show_usage() {
	fprintf(stderr,
"Usage: %s [options] command [arguments]\n"
"Options:\n"
"  -h --help        Show this help\n"
"  -v --verbose     Verbose output\n"
"  -d --device      Device name (default /dev/usb2spi_bridge)\n"
"  -i --in <file>   An input file (default: standard in)\n"
"  -o --out <file>  An output file (default: standard out)\n"
"\n"
"SPI Options:\n"
/* "  -s --spi name[,mode[,speed[,bits_per_word]]]\n" */
"  -D --spidev     SPI device (default /dev/spidev0.0)\n"
"  -s --speed      max speed in Hz (default 20kHz)\n"
"  -e --delay      delay in uS between messages (default 0)\n"
"  -b --bpw        bits per word (default 8, !=8 unsupported by mcp2210)\n"
"  -l --loop       enable loopback (default disabled, unsupported by mcp2210)\n"
"  -H --cpha <1|0> clock phase (default 1)\n"
"  -O --cpol <1|0> clock polarity (default 1)\n"
"  -L --lsb        least significant bit first (default MSB first)\n"
"  -C --cs-high    chip select active high (default low)\n"
"  -3 --3wire      SDI/SDO signals shared (default disabled)\n"
"\n"
"Commands: (this section may be incomplete, incorrect, etc.)\n"
"  get    config   Retrieve configuration from device.\n"
"  set    config <mask>\n"
"                  Upload configuration to device.  The value of mask specifies\n"
"                  the information set(s) from settings.h to upload to the\n"
"                  device ORed together:\n"
"                    1  current chip settings\n"
"                    2  power-up chip settings\n"
"                    4  current spi transfer settings\n"
"                    8  power-up spi transfer settings\n"
"                    16 key USB parameters\n"
"                    32 board config (causes spi_master to be probed)\n"
"\n"
"  encode          Encodes settings (in settings.h) into creek format.\n"
"  decode          Decodes creek blob into settings and outputs to stderr. Note\n"
"                  pin modes in settings.h must match.\n"
"\n"
"  eeprom          Reads from or writes to user-EEPROM area of device.\n"
"         read  addr=n size=n\n"
"         write addr=n size=n\n"
"  test            Runs a unit test\n"
"         bitstream\n"
"         config\n"
"  msg             Send a raw message to the MCP2210 device.\n"
"  spi \"<msg_string>\"\n"
"             Send an SPI message to the spidev device (use -D to specify\n"
"             device). A valid msg_string will consist of whitespace,\n"
"             hexadecimal digit pairs and comma. A comma will delimit\n"
"             individual transfers within the message. See Examples below.\n"
"             (Note, currently lacking support for an input file)\n"
"\n"
"Examples:\n"
"  %s -d /dev/usb2spi_bridge0 get config > creek.dat\n"
"      Retrieve the settings from the device and encode the board_config\n"
"      into creek.dat (note: subject to change soon)\n"
"\n"
"  %s set config 1\n"
"      Set the current chip settings to the values from settings.h to the\n"
"      default device /dev/usb2spi_bridge0\n"
"\n"
"  %s set config 63\n"
"      Upload all of the settings in settings.h to device and probe spi\n"
"\n"
"  %s get eeprom addr=0 size=0x100 > eeprom.dat\n"
"    or\n"
"  %s -o eeprom.dat get eeprom addr=0 size=0x100\n"
"      Read the user-EEPROM area and write it to eeprom.dat\n"
"\n"
"  %s test config\n"
"      Run the configuration unit test\n"
"\n"
"  %s spi 0100\n"
"      Send an spi message with one transfer that's two bytes long\n"
"\n"
"  %s spi 38,00,00,00\n"
"      Send an spi message with four transfers, each one byte in size (reads\n"
"      the CONFIG register on an L6470)\n"
"\n", argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

static int process_args(int argc, char *argv[])
{
	int c;
	while (1) {
		static struct option long_options[] = {
			{ "help",    no_argument,	0, 'h' },
			{ "verbose", no_argument,	0, 'v' },
			{ "device",  required_argument, 0, 'd' },
			{ "in",      required_argument, 0, 'i' },
			{ "out",     required_argument, 0, 'o' },

			/*{ "spi",     required_argument, 0, 's' },*/
			{ "spidev",  required_argument, 0, 'D' },
			{ "speed",   required_argument, 0, 's' },
			{ "delay",   required_argument, 0, 'e' },
			{ "bpw",     required_argument, 0, 'b' },
			{ "loop",    no_argument,	0, 'l' },
			{ "cpha",    required_argument, 0, 'H' },
			{ "cpol",    required_argument, 0, 'O' },
			{ "lsb",     no_argument,	0, 'L' },
			{ "cs-high", no_argument,	0, 'C' },
			{ "3wire",   no_argument,	0, '3' },
			{ NULL, 0, 0, 0 },
		};

		/* getopt_long stores the option index here. */
		int option_index = 0;
		c = getopt_long(argc, argv, "hvd:i:o:D:s:e:b:lH:O:LC3", long_options, &option_index);

		/* Detect the end of the options. */
		if (c == -1)
			break;

		switch (c) {
		case '?': fprintf(stderr, "Invalid argument: %s\n", argv[optind]);
		/* fall-through */
		case 'h': show_usage(); return 0;
		case 'v': config.verbose = 1;				break;
		case 'd': config.name = optarg;				break;
		case 'i': config.filein = optarg;			break;
		case 'o': config.fileout = optarg;			break;

		/* SPI options */
		case 'D': config.spi.name = optarg;			break;
		case 's':
			get_unsigned_param(optarg, &config.spi.speed_hz, MCP2210_MIN_SPEED, MCP2210_MAX_SPEED);
			break;
		case 'e':
			get_unsigned_param(optarg, &config.spi.delay_usecs, 0, UINT_MAX);
			break;
		case 'b':
			get_unsigned_param(optarg, &config.spi.bits_per_word, 8, 8);
			break;
		case 'l': config.spi.mode |= SPI_LOOP;			break;
		case 'H': 
			if (!get_bool_param(optarg))
				config.spi.mode &= ~SPI_CPHA;
			break;
		case 'O':
			if (!get_bool_param(optarg))
				config.spi.mode &= ~SPI_CPOL;
			break;
		case 'L': config.spi.mode |= SPI_LSB_FIRST;		break;
		case 'C': config.spi.mode |= SPI_CS_HIGH;		break;
		case '3': config.spi.mode |= SPI_3WIRE;			break;
		default:
			return -EINVAL;
			break;
		}
	}

	return dispatch(argc - optind, &argv[optind], command_list);
}


int main(int argc, char *argv[])
{
	int ret;

//	u8 buf[5] = {1,2,64,65,31};
//	dump_hex(stderr, buf, sizeof(buf));
//	ret = 0;
//if (0) {
	argv0 = argv[0];
	if ((ret = process_args(argc, argv))) {
		if (ret < 0)
			ret = -ret;
		errno = ret;
		perror("main");
	}
//}
	return ret;
}

