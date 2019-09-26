// SPDX-License-Identifier: GPL-2.0
/*
 * debugfs attributes for Wilco EC
 *
 * Copyright 2019 Google LLC
 *
 * There is only one attribute used for debugging, called raw.
 * You can write a hexadecimal sentence to raw, and that series of bytes
 * will be sent to the EC. Then, you can read the bytes of response
 * by reading from raw.
 *
 * For writing:
 * Bytes 0-1 indicate the message type:
 *         00 F0 = Execute Legacy Command
 *         00 F2 = Read/Write NVRAM Property
 * Byte 2 provides the command code
 * Bytes 3+ consist of the data passed in the request
 *
 * When referencing the EC interface spec, byte 2 corresponds to MBOX[0],
 * byte 3 corresponds to MBOX[1], etc.
 *
 * At least three bytes are required, for the msg type and command,
 * with additional bytes optional for additional data.
 *
 * Example:
 * // Request EC info type 3 (EC firmware build date)
 * $ echo 00 f0 38 00 03 00 > raw
 * // View the result. The decoded ASCII result "12/21/18" is
 * // included after the raw hex.
 * $ cat raw
 * 00 31 32 2f 32 31 2f 31 38 00 38 00 01 00 2f 00  .12/21/18.8...
 */

#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/platform_data/wilco-ec.h>
#include <linux/platform_device.h>

#define DRV_NAME "wilco-ec-debugfs"

/* The 256 raw bytes will take up more space when represented as a hex string */
#define FORMATTED_BUFFER_SIZE (EC_MAILBOX_DATA_SIZE_EXTENDED * 4)

struct wilco_ec_debugfs {
	struct wilco_ec_device *ec;
	struct dentry *dir;
	size_t response_size;
	u8 raw_data[EC_MAILBOX_DATA_SIZE_EXTENDED];
	u8 formatted_data[FORMATTED_BUFFER_SIZE];
};
static struct wilco_ec_debugfs *debug_info;

/**
 * parse_hex_sentence() - Convert a ascii hex representation into byte array.
 * @in: Input buffer of ascii.
 * @isize: Length of input buffer.
 * @out: Output buffer.
 * @osize: Length of output buffer, e.g. max number of bytes to parse.
 *
 * An valid input is a series of ascii hexadecimal numbers, separated by spaces.
 * An example valid input is
 * "   00 f2 0    000076 6 0  ff"
 *
 * If an individual "word" within the hex sentence is longer than MAX_WORD_SIZE,
 * then the sentence is illegal, and parsing will fail.
 *
 * Return: Number of bytes parsed, or negative error code on failure.
 */
static int parse_hex_sentence(const char *in, int isize, u8 *out, int osize)
{
	int n_parsed = 0;
	int word_start = 0;
	int word_end;
	int word_len;
	/* Temp buffer for holding a "word" of chars that represents one byte */
	#define MAX_WORD_SIZE 16
	char tmp[MAX_WORD_SIZE + 1];
	u8 byte;

	while (word_start < isize && n_parsed < osize) {
		/* Find the start of the next word */
		while (word_start < isize && isspace(in[word_start]))
			word_start++;
		 /* reached the end of the input before next word? */
		if (word_start >= isize)
			break;

		/* Find the end of this word */
		word_end = word_start;
		while (word_end < isize && !isspace(in[word_end]))
			word_end++;

		/* Copy to a tmp NULL terminated string */
		word_len = word_end - word_start;
		if (word_len > MAX_WORD_SIZE)
			return -EINVAL;
		memcpy(tmp, in + word_start, word_len);
		tmp[word_len] = '\0';

		/*
		 * Convert from hex string, place in output. If fails to parse,
		 * just return -EINVAL because specific error code is only
		 * relevant for this one word, returning it would be confusing.
		 */
		if (kstrtou8(tmp, 16, &byte))
			return -EINVAL;
		out[n_parsed++] = byte;

		word_start = word_end;
	}
	return n_parsed;
}

/* The message type takes up two bytes*/
#define TYPE_AND_DATA_SIZE ((EC_MAILBOX_DATA_SIZE) + 2)

static ssize_t raw_write(struct file *file, const char __user *user_buf,
			 size_t count, loff_t *ppos)
{
	char *buf = debug_info->formatted_data;
	struct wilco_ec_message msg;
	u8 request_data[TYPE_AND_DATA_SIZE];
	ssize_t kcount;
	int ret;

	if (count > FORMATTED_BUFFER_SIZE)
		return -EINVAL;

	kcount = simple_write_to_buffer(buf, FORMATTED_BUFFER_SIZE, ppos,
					user_buf, count);
	if (kcount < 0)
		return kcount;

	ret = parse_hex_sentence(buf, kcount, request_data, TYPE_AND_DATA_SIZE);
	if (ret < 0)
		return ret;
	/* Need at least two bytes for message type and one for command */
	if (ret < 3)
		return -EINVAL;

	/* Clear response data buffer */
	memset(debug_info->raw_data, '\0', EC_MAILBOX_DATA_SIZE_EXTENDED);

	msg.type = request_data[0] << 8 | request_data[1];
	msg.flags = WILCO_EC_FLAG_RAW;
	msg.command = request_data[2];
	msg.request_data = ret > 3 ? request_data + 3 : 0;
	msg.request_size = ret - 3;
	msg.response_data = debug_info->raw_data;
	msg.response_size = EC_MAILBOX_DATA_SIZE;

	/* Telemetry commands use extended response data */
	if (msg.type == WILCO_EC_MSG_TELEMETRY_LONG) {
		msg.flags |= WILCO_EC_FLAG_EXTENDED_DATA;
		msg.response_size = EC_MAILBOX_DATA_SIZE_EXTENDED;
	}

	ret = wilco_ec_mailbox(debug_info->ec, &msg);
	if (ret < 0)
		return ret;
	debug_info->response_size = ret;

	return count;
}

static ssize_t raw_read(struct file *file, char __user *user_buf, size_t count,
			loff_t *ppos)
{
	int fmt_len = 0;

	if (debug_info->response_size) {
		fmt_len = hex_dump_to_buffer(debug_info->raw_data,
					     debug_info->response_size,
					     16, 1, debug_info->formatted_data,
					     FORMATTED_BUFFER_SIZE, true);
		/* Only return response the first time it is read */
		debug_info->response_size = 0;
	}

	return simple_read_from_buffer(user_buf, count, ppos,
				       debug_info->formatted_data, fmt_len);
}

static const struct file_operations fops_raw = {
	.owner = THIS_MODULE,
	.read = raw_read,
	.write = raw_write,
	.llseek = no_llseek,
};

/**
 * wilco_ec_debugfs_probe() - Create the debugfs node
 * @pdev: The platform device, probably created in core.c
 *
 * Try to create a debugfs node. If it fails, then we don't want to change
 * behavior at all, this is for debugging after all. Just fail silently.
 *
 * Return: 0 always.
 */
static int wilco_ec_debugfs_probe(struct platform_device *pdev)
{
	struct wilco_ec_device *ec = dev_get_drvdata(pdev->dev.parent);

	debug_info = devm_kzalloc(&pdev->dev, sizeof(*debug_info), GFP_KERNEL);
	if (!debug_info)
		return 0;
	debug_info->ec = ec;
	debug_info->dir = debugfs_create_dir("wilco_ec", NULL);
	if (!debug_info->dir)
		return 0;
	debugfs_create_file("raw", 0644, debug_info->dir, NULL, &fops_raw);

	return 0;
}

static int wilco_ec_debugfs_remove(struct platform_device *pdev)
{
	debugfs_remove_recursive(debug_info->dir);

	return 0;
}

static struct platform_driver wilco_ec_debugfs_driver = {
	.driver = {
		.name = DRV_NAME,
	},
	.probe = wilco_ec_debugfs_probe,
	.remove = wilco_ec_debugfs_remove,
};

module_platform_driver(wilco_ec_debugfs_driver);

MODULE_ALIAS("platform:" DRV_NAME);
MODULE_AUTHOR("Nick Crews <ncrews@chromium.org>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Wilco EC debugfs driver");
