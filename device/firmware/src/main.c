#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/multi_heap/shared_multi_heap.h>
#include <ff.h>

#define DISK_DRIVE_NAME "SD"
#define DISK_MOUNT_PT "/SD:"
#define CAPTURE_PATH DISK_MOUNT_PT "/capture.bmp"

#define PNG_BLOCK_SIZE 65535U

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		(sizeof(CONFIG_BT_DEVICE_NAME) - 1)),
};

static FATFS fat_fs;
static struct fs_mount_t sd_mount = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = DISK_MOUNT_PT,
};

static int ov2640_write_reg(const struct device *i2c, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	return i2c_write(i2c, buf, 2, 0x30);
}

/* Essential OV2640 initialization registers (from Zephyr driver) */
static const uint8_t ov2640_init_regs[][2] = {
	/* Reset and basic sensor setup */
	{0xFF, 0x01}, /* BANK_SEL = sensor */
	{0x12, 0x80}, /* COM7 soft reset */
};

/*
 * OV2640 default registers aligned with omiGlass (omiGlass firmware
 * ov2640_settings_cif + RGB565 path). Same resolution/quality tuning
 * as omiGlass: VGA 640x480, AEC/AGC targets and JPEG-quality-style tuning.
 */
static const uint8_t ov2640_default_regs[][2] = {
	{0xFF, 0x00}, /* BANK_SEL = DSP */
	{0x2c, 0xff},
	{0x2e, 0xdf},
	{0xFF, 0x01}, /* BANK_SEL = sensor */
	{0x3c, 0x32},
	{0x11, 0x01}, /* CLKRC - omiGlass: clock divider for stability */
	{0x09, 0x02}, /* COM2 - output drive 3x */
	{0x04, 0x28 | 0x08}, /* REG04 */
	{0x13, 0xE5}, /* COM8 - omiGlass: AGC, AEC, banding filter (no AWB in COM8) */
	{0x14, 0x08 | (0x02 << 5)}, /* COM9 - omiGlass: AGC gain ceiling 8x (CAMERA_JPEG_QUALITY style) */
	{0x15, 0x00}, /* COM10 */
	{0x2D, 0x00}, /* AEC LSB */
	{0x2E, 0x00}, /* AEC MSB */
	{0x45, 0x00}, /* AECHH */
	{0x10, 0x00}, /* AEC[7:0] */
	{0x2c, 0x0c},
	{0x33, 0x78},
	{0x3a, 0x33},
	{0x3b, 0xfb},
	{0x3e, 0x00},
	{0x43, 0x11},
	{0x16, 0x10},
	{0x39, 0x92}, /* omiGlass CIF */
	{0x35, 0xda}, /* omiGlass CIF */
	{0x22, 0x1a}, /* omiGlass AEW-style */
	{0x37, 0xc3}, /* omiGlass CIF */
	{0x23, 0x00},
	{0x34, 0xc0}, /* ARCOM2 omiGlass CIF */
	{0x06, 0x88},
	{0x07, 0xc0},
	{0x0d, 0x87}, /* COM4 omiGlass */
	{0x0e, 0x41}, /* omiGlass CIF */
	{0x4c, 0x00},
	{0x4a, 0x81},
	{0x21, 0x99},
	{0x24, 0x40}, /* AEW - omiGlass target brightness */
	{0x25, 0x38}, /* AEB - omiGlass floor brightness */
	{0x26, 0x82}, /* VV - omiGlass AEC/AGC thresholds */
	{0x48, 0x00}, /* COM19 */
	{0x49, 0x00}, /* ZOOMS */
	{0x5c, 0x00},
	{0x63, 0x00},
	{0x46, 0x00}, /* FLL */
	{0x47, 0x00}, /* FLH */
	{0x0C, 0x38 | 0x02}, /* COM3 */
	{0x5D, 0x55},
	{0x5E, 0x7d},
	{0x5F, 0x7d},
	{0x60, 0x55},
	{0x61, 0x70}, /* HISTO_LOW */
	{0x62, 0x80}, /* HISTO_HIGH */
	{0x7c, 0x05},
	{0x20, 0x80},
	{0x28, 0x30},
	{0x6c, 0x00},
	{0x6d, 0x00}, /* omiGlass CIF */
	{0x6e, 0x00},
	{0x70, 0x02},
	{0x71, 0x94},
	{0x73, 0xc1},
	{0x3d, 0x38}, /* omiGlass CIF */
	{0x5a, 0x23}, /* omiGlass CIF */
	{0x4F, 0xca}, /* BD50 omiGlass CIF */
	{0x50, 0xa8}, /* BD60 omiGlass CIF */
	{0xFF, 0x00}, /* BANK_SEL = DSP */
	{0xe5, 0x7f},
	{0xF9, 0x80 | 0x40}, /* MC_BIST */
	{0x41, 0x24},
	{0xE0, 0x10 | 0x04}, /* RESET */
	{0x76, 0xff},
	{0x33, 0xa0},
	{0x42, 0x20},
	{0x43, 0x18},
	{0x4c, 0x00},
	{0x87, 0x80 | 0x40 | 0x10}, /* CTRL3 */
	{0x88, 0x3f},
	{0xd7, 0x03},
	{0xd9, 0x10},
	{0xD3, 0x80 | 0x02}, /* R_DVP_SP */
	{0xc8, 0x08},
	{0xc9, 0x80},
	{0x7c, 0x00},
	{0x7d, 0x00},
	{0x7c, 0x03},
	{0x7d, 0x48},
	{0x7d, 0x48},
	{0x7c, 0x08},
	{0x7d, 0x20},
	{0x7d, 0x10},
	{0x7d, 0x0e},
	{0x90, 0x00},
	{0x91, 0x0e},
	{0x91, 0x1a},
	{0x91, 0x31},
	{0x91, 0x5a},
	{0x91, 0x69},
	{0x91, 0x75},
	{0x91, 0x7e},
	{0x91, 0x88},
	{0x91, 0x8f},
	{0x91, 0x96},
	{0x91, 0xa3},
	{0x91, 0xaf},
	{0x91, 0xc4},
	{0x91, 0xd7},
	{0x91, 0xe8},
	{0x91, 0x20},
	{0xC2, 0x08 | 0x04 | 0x02}, /* CTRL0 - enable YUV422/YUV/RGB */
	{0x00, 0x00},
};

/*
 * Resolution: same as omiGlass FRAMESIZE_VGA = 640x480 (video_set_format below).
 */

/* RGB565 output format */
static const uint8_t ov2640_rgb565_regs[][2] = {
	{0xFF, 0x00}, /* BANK_SEL = DSP */
	{0xDA, 0x08}, /* IMAGE_MODE - RGB565 */
	{0xD7, 0x03},
	{0xDF, 0x00},
	{0x33, 0xa0},
	{0x3C, 0x00},
	{0xe1, 0x67},
	{0x00, 0x00},
};

static int ov2640_init_sensor(const struct device *i2c)
{
	int ret;
	size_t i;

	printk("Initializing OV2640 sensor...\n");

	/* Soft reset */
	ret = ov2640_write_reg(i2c, 0xFF, 0x01);
	if (ret) return ret;
	ret = ov2640_write_reg(i2c, 0x12, 0x80);
	if (ret) return ret;
	k_sleep(K_MSEC(100));

	/* Write default registers (exposure, gain settings) */
	for (i = 0; i < ARRAY_SIZE(ov2640_default_regs); i++) {
		ret = ov2640_write_reg(i2c, ov2640_default_regs[i][0], 
				       ov2640_default_regs[i][1]);
		if (ret) {
			printk("Failed to write default reg 0x%02x (%d)\n",
			       ov2640_default_regs[i][0], ret);
			return ret;
		}
	}

	/* Note: Resolution is set by Zephyr driver via video_set_format() */

	/* Set RGB565 output format */
	for (i = 0; i < ARRAY_SIZE(ov2640_rgb565_regs); i++) {
		ret = ov2640_write_reg(i2c, ov2640_rgb565_regs[i][0],
				       ov2640_rgb565_regs[i][1]);
		if (ret) {
			printk("Failed to write RGB565 reg 0x%02x (%d)\n",
			       ov2640_rgb565_regs[i][0], ret);
			return ret;
		}
	}

	/* Wait for auto-exposure to stabilize */
	printk("OV2640 initialization complete, waiting for AEC...\n");
	k_sleep(K_MSEC(500));
	return 0;
}

static const struct device *ov2640_i2c_bus;

static bool ov2640_detected_on_bus(const struct device *i2c, const char *bus_name)
{
	uint8_t buf[2];
	uint8_t pid = 0;
	uint8_t ver = 0;
	int ret;

	if (!device_is_ready(i2c)) {
		printk("%s not ready\n", bus_name);
		return false;
	}

	/* Wait for camera to power up */
	k_sleep(K_MSEC(100));

	/* Select sensor register bank (0xFF = 0x01) */
	buf[0] = 0xFF;
	buf[1] = 0x01;
	ret = i2c_write(i2c, buf, 2, 0x30);
	if (ret != 0) {
		printk("%s OV2640 bank select failed (%d)\n", bus_name, ret);
		return false;
	}

	k_sleep(K_MSEC(10));

	/* Read PID register (0x0A) */
	buf[0] = 0x0A;
	ret = i2c_write_read(i2c, 0x30, buf, 1, &pid, 1);
	if (ret != 0) {
		printk("%s OV2640 PID read failed (%d)\n", bus_name, ret);
		return false;
	}

	/* Read VER register (0x0B) */
	buf[0] = 0x0B;
	ret = i2c_write_read(i2c, 0x30, buf, 1, &ver, 1);
	if (ret != 0) {
		printk("%s OV2640 VER read failed (%d)\n", bus_name, ret);
		return false;
	}

	if ((pid == 0x00 && ver == 0x00) || (pid == 0xFF && ver == 0xFF)) {
		printk("%s OV2640 invalid ID (PID 0x%02x VER 0x%02x)\n",
		       bus_name, pid, ver);
		return false;
	}

	printk("%s OV2640 detected (PID 0x%02x VER 0x%02x)\n", bus_name, pid, ver);

	/* Initialize the camera since Zephyr driver failed at boot */
	ret = ov2640_init_sensor(i2c);
	if (ret != 0) {
		printk("OV2640 init failed (%d)\n", ret);
		return false;
	}

	ov2640_i2c_bus = i2c;
	return true;
}

static bool ov2640_detected(void)
{
	const struct device *i2c1 = DEVICE_DT_GET(DT_NODELABEL(i2c1));
	const struct device *i2c0 = DEVICE_DT_GET(DT_NODELABEL(i2c0));

	if (ov2640_detected_on_bus(i2c1, "I2C1")) {
		return true;
	}
	if (ov2640_detected_on_bus(i2c0, "I2C0")) {
		printk("OV2640 responds on I2C0; update devicetree if needed.\n");
		return true;
	}
	return false;
}

static uint32_t crc32_init(void)
{
	return 0xFFFFFFFFU;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
	static uint32_t table[256];
	static bool table_ready;

	if (!table_ready) {
		for (uint32_t i = 0; i < ARRAY_SIZE(table); i++) {
			uint32_t c = i;
			for (int j = 0; j < 8; j++) {
				c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
			}
			table[i] = c;
		}
		table_ready = true;
	}

	for (size_t i = 0; i < len; i++) {
		crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
	}
	return crc;
}

static uint32_t crc32_finalize(uint32_t crc)
{
	return crc ^ 0xFFFFFFFFU;
}

static uint32_t adler32_update(uint32_t adler, const uint8_t *data, size_t len)
{
	uint32_t a = adler & 0xFFFFU;
	uint32_t b = (adler >> 16) & 0xFFFFU;
	const uint32_t mod = 65521U;

	for (size_t i = 0; i < len; i++) {
		a = (a + data[i]) % mod;
		b = (b + a) % mod;
	}
	return (b << 16) | a;
}

static int fs_write_all(struct fs_file_t *file, const uint8_t *data, size_t len)
{
	size_t offset = 0;

	while (offset < len) {
		ssize_t wrote = fs_write(file, data + offset, len - offset);
		if (wrote < 0) {
			return (int)wrote;
		}
		offset += (size_t)wrote;
	}
	return 0;
}

static int png_write_u32_be(struct fs_file_t *file, uint32_t value)
{
	uint8_t buf[4] = {
		(uint8_t)(value >> 24),
		(uint8_t)(value >> 16),
		(uint8_t)(value >> 8),
		(uint8_t)value,
	};

	return fs_write_all(file, buf, sizeof(buf));
}

static int png_write_chunk(struct fs_file_t *file, const char type[4],
			   const uint8_t *data, size_t len)
{
	uint32_t crc = crc32_init();
	int ret;

	ret = png_write_u32_be(file, (uint32_t)len);
	if (ret != 0) {
		return ret;
	}

	ret = fs_write_all(file, (const uint8_t *)type, 4);
	if (ret != 0) {
		return ret;
	}
	crc = crc32_update(crc, (const uint8_t *)type, 4);

	if (len > 0 && data != NULL) {
		ret = fs_write_all(file, data, len);
		if (ret != 0) {
			return ret;
		}
		crc = crc32_update(crc, data, len);
	}

	return png_write_u32_be(file, crc32_finalize(crc));
}

struct zlib_writer {
	struct fs_file_t *file;
	uint32_t remaining;
	uint32_t block_remaining;
	uint32_t adler;
	uint32_t crc;
};

static int zlib_start_block(struct zlib_writer *w)
{
	if (w->remaining == 0) {
		return 0;
	}

	uint32_t block_len = (w->remaining > PNG_BLOCK_SIZE) ? PNG_BLOCK_SIZE : w->remaining;
	uint8_t header = (w->remaining == block_len) ? 0x01 : 0x00;
	uint8_t len_lo = block_len & 0xFF;
	uint8_t len_hi = (block_len >> 8) & 0xFF;
	uint16_t nlen = (uint16_t)~block_len;
	uint8_t nlen_lo = nlen & 0xFF;
	uint8_t nlen_hi = (nlen >> 8) & 0xFF;
	uint8_t block_hdr[5] = { header, len_lo, len_hi, nlen_lo, nlen_hi };
	int ret;

	ret = fs_write_all(w->file, block_hdr, sizeof(block_hdr));
	if (ret != 0) {
		return ret;
	}

	w->crc = crc32_update(w->crc, block_hdr, sizeof(block_hdr));
	w->block_remaining = block_len;
	return 0;
}

static int zlib_write_data(struct zlib_writer *w, const uint8_t *data, size_t len)
{
	while (len > 0) {
		if (w->block_remaining == 0) {
			int ret = zlib_start_block(w);
			if (ret != 0) {
				return ret;
			}
		}

		size_t chunk = len;
		if (chunk > w->block_remaining) {
			chunk = w->block_remaining;
		}

		int ret = fs_write_all(w->file, data, chunk);
		if (ret != 0) {
			return ret;
		}

		w->crc = crc32_update(w->crc, data, chunk);
		w->adler = adler32_update(w->adler, data, chunk);
		w->block_remaining -= (uint32_t)chunk;
		w->remaining -= (uint32_t)chunk;
		data += chunk;
		len -= chunk;
	}

	return 0;
}

static int png_write_rgb565(struct fs_file_t *file, const uint8_t *rgb565,
			    uint32_t width, uint32_t height, uint32_t pitch)
{
	static const uint8_t signature[8] = {
		0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
	};
	uint8_t ihdr[13];
	uint32_t row_size = (width * 3U) + 1U;
	uint32_t data_len = row_size * height;
	uint32_t block_count = (data_len + PNG_BLOCK_SIZE - 1U) / PNG_BLOCK_SIZE;
	uint32_t zlib_len = 2U + data_len + (block_count * 5U) + 4U;
	uint32_t crc;
	int ret;

	ret = fs_write_all(file, signature, sizeof(signature));
	if (ret != 0) {
		return ret;
	}

	ihdr[0] = (width >> 24) & 0xFF;
	ihdr[1] = (width >> 16) & 0xFF;
	ihdr[2] = (width >> 8) & 0xFF;
	ihdr[3] = width & 0xFF;
	ihdr[4] = (height >> 24) & 0xFF;
	ihdr[5] = (height >> 16) & 0xFF;
	ihdr[6] = (height >> 8) & 0xFF;
	ihdr[7] = height & 0xFF;
	ihdr[8] = 8; /* bit depth */
	ihdr[9] = 2; /* color type: truecolor */
	ihdr[10] = 0; /* compression */
	ihdr[11] = 0; /* filter */
	ihdr[12] = 0; /* interlace */

	ret = png_write_chunk(file, "IHDR", ihdr, sizeof(ihdr));
	if (ret != 0) {
		return ret;
	}

	ret = png_write_u32_be(file, zlib_len);
	if (ret != 0) {
		return ret;
	}

	ret = fs_write_all(file, (const uint8_t *)"IDAT", 4);
	if (ret != 0) {
		return ret;
	}

	crc = crc32_update(crc32_init(), (const uint8_t *)"IDAT", 4);

	struct zlib_writer zw = {
		.file = file,
		.remaining = data_len,
		.block_remaining = 0,
		.adler = 1U,
		.crc = crc,
	};
	const uint8_t zhdr[2] = { 0x78, 0x01 };

	ret = fs_write_all(file, zhdr, sizeof(zhdr));
	if (ret != 0) {
		return ret;
	}
	zw.crc = crc32_update(zw.crc, zhdr, sizeof(zhdr));

	uint8_t *row_buf = k_malloc(1 + (width * 3U));
	if (row_buf == NULL) {
		return -ENOMEM;
	}

	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *row = rgb565 + (y * pitch);
		row_buf[0] = 0;
		for (uint32_t x = 0; x < width; x++) {
			/* ESP32 DVP outputs RGB565 big-endian (high byte first) */
			uint16_t pixel = ((uint16_t)row[2 * x] << 8) | row[2 * x + 1];
			uint8_t r = (pixel >> 11) & 0x1F;
			uint8_t g = (pixel >> 5) & 0x3F;
			uint8_t b = pixel & 0x1F;

			row_buf[1 + (x * 3) + 0] = (r << 3) | (r >> 2);
			row_buf[1 + (x * 3) + 1] = (g << 2) | (g >> 4);
			row_buf[1 + (x * 3) + 2] = (b << 3) | (b >> 2);
		}

		ret = zlib_write_data(&zw, row_buf, 1 + (width * 3U));
		if (ret != 0) {
			shared_multi_heap_free(row_buf);
			return ret;
		}
	}
	shared_multi_heap_free(row_buf);

	uint8_t adler_be[4] = {
		(uint8_t)(zw.adler >> 24),
		(uint8_t)(zw.adler >> 16),
		(uint8_t)(zw.adler >> 8),
		(uint8_t)zw.adler,
	};
	ret = fs_write_all(file, adler_be, sizeof(adler_be));
	if (ret != 0) {
		return ret;
	}
	zw.crc = crc32_update(zw.crc, adler_be, sizeof(adler_be));

	ret = png_write_u32_be(file, crc32_finalize(zw.crc));
	if (ret != 0) {
		return ret;
	}

	return png_write_chunk(file, "IEND", NULL, 0);
}

static inline uint8_t clamp_u8(int value)
{
	if (value < 0) {
		return 0;
	}
	if (value > 255) {
		return 255;
	}
	return (uint8_t)value;
}

static int png_write_yuyv(struct fs_file_t *file, const uint8_t *yuyv,
			  uint32_t width, uint32_t height, uint32_t pitch)
{
	static const uint8_t signature[8] = {
		0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
	};
	uint8_t ihdr[13];
	uint32_t row_size = (width * 3U) + 1U;
	uint32_t data_len = row_size * height;
	uint32_t block_count = (data_len + PNG_BLOCK_SIZE - 1U) / PNG_BLOCK_SIZE;
	uint32_t zlib_len = 2U + data_len + (block_count * 5U) + 4U;
	uint32_t crc;
	int ret;

	ret = fs_write_all(file, signature, sizeof(signature));
	if (ret != 0) {
		return ret;
	}

	ihdr[0] = (width >> 24) & 0xFF;
	ihdr[1] = (width >> 16) & 0xFF;
	ihdr[2] = (width >> 8) & 0xFF;
	ihdr[3] = width & 0xFF;
	ihdr[4] = (height >> 24) & 0xFF;
	ihdr[5] = (height >> 16) & 0xFF;
	ihdr[6] = (height >> 8) & 0xFF;
	ihdr[7] = height & 0xFF;
	ihdr[8] = 8; /* bit depth */
	ihdr[9] = 2; /* color type: truecolor */
	ihdr[10] = 0; /* compression */
	ihdr[11] = 0; /* filter */
	ihdr[12] = 0; /* interlace */

	ret = png_write_chunk(file, "IHDR", ihdr, sizeof(ihdr));
	if (ret != 0) {
		return ret;
	}

	ret = png_write_u32_be(file, zlib_len);
	if (ret != 0) {
		return ret;
	}

	ret = fs_write_all(file, (const uint8_t *)"IDAT", 4);
	if (ret != 0) {
		return ret;
	}

	crc = crc32_update(crc32_init(), (const uint8_t *)"IDAT", 4);

	struct zlib_writer zw = {
		.file = file,
		.remaining = data_len,
		.block_remaining = 0,
		.adler = 1U,
		.crc = crc,
	};
	const uint8_t zhdr[2] = { 0x78, 0x01 };

	ret = fs_write_all(file, zhdr, sizeof(zhdr));
	if (ret != 0) {
		return ret;
	}
	zw.crc = crc32_update(zw.crc, zhdr, sizeof(zhdr));

	uint8_t *row_buf = k_malloc(1 + (width * 3U));
	if (row_buf == NULL) {
		return -ENOMEM;
	}

	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *row = yuyv + (y * pitch);
		row_buf[0] = 0;

		for (uint32_t x = 0; x < width; x += 2) {
			uint8_t y0 = row[(x * 2) + 0];
			uint8_t u = row[(x * 2) + 1];
			uint8_t y1 = row[(x * 2) + 2];
			uint8_t v = row[(x * 2) + 3];

			int c0 = (int)y0 - 16;
			int c1 = (int)y1 - 16;
			int d = (int)u - 128;
			int e = (int)v - 128;

			int r0 = (298 * c0 + 409 * e + 128) >> 8;
			int g0 = (298 * c0 - 100 * d - 208 * e + 128) >> 8;
			int b0 = (298 * c0 + 516 * d + 128) >> 8;
			row_buf[1 + (x * 3) + 0] = clamp_u8(r0);
			row_buf[1 + (x * 3) + 1] = clamp_u8(g0);
			row_buf[1 + (x * 3) + 2] = clamp_u8(b0);

			if (x + 1 < width) {
				int r1 = (298 * c1 + 409 * e + 128) >> 8;
				int g1 = (298 * c1 - 100 * d - 208 * e + 128) >> 8;
				int b1 = (298 * c1 + 516 * d + 128) >> 8;
				row_buf[1 + ((x + 1) * 3) + 0] = clamp_u8(r1);
				row_buf[1 + ((x + 1) * 3) + 1] = clamp_u8(g1);
				row_buf[1 + ((x + 1) * 3) + 2] = clamp_u8(b1);
			}
		}

		ret = zlib_write_data(&zw, row_buf, 1 + (width * 3U));
		if (ret != 0) {
			shared_multi_heap_free(row_buf);
			return ret;
		}
	}
	shared_multi_heap_free(row_buf);

	uint8_t adler_be[4] = {
		(uint8_t)(zw.adler >> 24),
		(uint8_t)(zw.adler >> 16),
		(uint8_t)(zw.adler >> 8),
		(uint8_t)zw.adler,
	};
	ret = fs_write_all(file, adler_be, sizeof(adler_be));
	if (ret != 0) {
		return ret;
	}
	zw.crc = crc32_update(zw.crc, adler_be, sizeof(adler_be));

	ret = png_write_u32_be(file, crc32_finalize(zw.crc));
	if (ret != 0) {
		return ret;
	}

	return png_write_chunk(file, "IEND", NULL, 0);
}

static int mount_sdcard(void)
{
	int ret = disk_access_ioctl(DISK_DRIVE_NAME, DISK_IOCTL_CTRL_INIT, NULL);
	if (ret != 0) {
		printk("SD init failed (%d)\n", ret);
		return ret;
	}

	ret = fs_mount(&sd_mount);
	if (ret < 0) {
		printk("SD mount failed (%d)\n", ret);
		return ret;
	}

	return 0;
}

/* Write raw RGB565 as 24-bit BMP file (simpler than PNG, good for debugging) */
static int bmp_write_rgb565(struct fs_file_t *file, const uint8_t *rgb565,
			    uint32_t width, uint32_t height, uint32_t pitch)
{
	/* BMP Header (14 bytes) + DIB Header (40 bytes) = 54 bytes */
	uint8_t header[54];
	uint32_t row_size = ((width * 3 + 3) / 4) * 4; /* 24-bit, padded to 4 bytes */
	uint32_t pixel_data_size = row_size * height;
	uint32_t file_size = 54 + pixel_data_size;
	uint8_t *row_buf;
	int ret;

	/* Allocate from PSRAM for large row buffers */
	row_buf = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, row_size);
	if (!row_buf) {
		row_buf = k_malloc(row_size);
	}
	if (!row_buf) {
		printk("BMP: no memory for row buffer (%u bytes)\n", row_size);
		return -ENOMEM;
	}

	memset(header, 0, sizeof(header));

	/* BMP Header */
	header[0] = 'B';
	header[1] = 'M';
	header[2] = file_size & 0xFF;
	header[3] = (file_size >> 8) & 0xFF;
	header[4] = (file_size >> 16) & 0xFF;
	header[5] = (file_size >> 24) & 0xFF;
	/* Reserved (4 bytes) = 0 */
	header[10] = 54; /* Pixel data offset */

	/* DIB Header (BITMAPINFOHEADER) */
	header[14] = 40; /* DIB header size */
	header[18] = width & 0xFF;
	header[19] = (width >> 8) & 0xFF;
	header[20] = (width >> 16) & 0xFF;
	header[21] = (width >> 24) & 0xFF;
	header[22] = height & 0xFF;
	header[23] = (height >> 8) & 0xFF;
	header[24] = (height >> 16) & 0xFF;
	header[25] = (height >> 24) & 0xFF;
	header[26] = 1; /* Color planes */
	header[28] = 24; /* Bits per pixel */
	/* Compression = 0 (BI_RGB) */
	header[34] = pixel_data_size & 0xFF;
	header[35] = (pixel_data_size >> 8) & 0xFF;
	header[36] = (pixel_data_size >> 16) & 0xFF;
	header[37] = (pixel_data_size >> 24) & 0xFF;

	ret = fs_write_all(file, header, sizeof(header));
	if (ret != 0) {
		shared_multi_heap_free(row_buf);
		return ret;
	}

	/* BMP stores rows bottom-to-top */
	for (int32_t y = height - 1; y >= 0; y--) {
		const uint8_t *src_row = rgb565 + y * pitch;
		memset(row_buf, 0, row_size);

		for (uint32_t x = 0; x < width; x++) {
			/* RGB565 big-endian from ESP32 DVP */
			uint16_t pixel = ((uint16_t)src_row[2 * x] << 8) | src_row[2 * x + 1];
			uint8_t r = (pixel >> 11) & 0x1F;
			uint8_t g = (pixel >> 5) & 0x3F;
			uint8_t b = pixel & 0x1F;

			/* Expand to 8-bit and store as BGR */
			row_buf[x * 3 + 0] = (b << 3) | (b >> 2);
			row_buf[x * 3 + 1] = (g << 2) | (g >> 4);
			row_buf[x * 3 + 2] = (r << 3) | (r >> 2);
		}

		ret = fs_write_all(file, row_buf, row_size);
		if (ret != 0) {
			shared_multi_heap_free(row_buf);
			return ret;
		}
	}

	shared_multi_heap_free(row_buf);
	printk("BMP: wrote %ux%u image (%u bytes)\n", width, height, file_size);
	return 0;
}

static bool format_supports(const struct video_format_cap *cap, uint32_t width, uint32_t height)
{
	if (width < cap->width_min || width > cap->width_max) {
		return false;
	}
	if (height < cap->height_min || height > cap->height_max) {
		return false;
	}
	if (cap->width_step && ((width - cap->width_min) % cap->width_step)) {
		return false;
	}
	if (cap->height_step && ((height - cap->height_min) % cap->height_step)) {
		return false;
	}
	return true;
}

static int capture_bmp_to_sd(void)
{
	const struct device *camera = DEVICE_DT_GET(DT_CHOSEN(zephyr_camera));
	struct video_caps caps = { .type = VIDEO_BUF_TYPE_OUTPUT };
	struct video_format fmt = { 0 };
	struct video_buffer *dequeued = NULL;
	struct fs_file_t file;
	/* Same as omiGlass: FRAMESIZE_VGA = 640x480 */
	uint32_t width = 640;
	uint32_t height = 480;
	uint32_t pitch;
	int ret;
	bool streaming = false;
	struct video_buffer *buffers[CONFIG_VIDEO_BUFFER_POOL_NUM_MAX] = { 0 };
	uint8_t buffer_count = 0;

	if (!device_is_ready(camera)) {
		printk("Camera device not ready\n");
		return -ENODEV;
	}

	if (!ov2640_detected()) {
		printk("Camera not detected on I2C\n");
		return -ENODEV;
	}

	/* Give camera time to stabilize after I2C probing */
	k_sleep(K_MSEC(500));

	ret = video_get_caps(camera, &caps);
	if (ret != 0) {
		printk("Camera caps failed (%d)\n", ret);
		return ret;
	}

	/* Find the format cap matching our target resolution */
	const struct video_format_cap *cap = caps.format_caps;
	const struct video_format_cap *chosen = NULL;
	const struct video_format_cap *fallback = NULL;
	
	printk("Looking for %ux%u RGB565...\n", width, height);
	while (cap && cap->pixelformat != 0) {
		printk("  Available: %c%c%c%c %ux%u\n",
		       (char)cap->pixelformat,
		       (char)(cap->pixelformat >> 8),
		       (char)(cap->pixelformat >> 16),
		       (char)(cap->pixelformat >> 24),
		       cap->width_min, cap->height_min);
		if (cap->pixelformat == VIDEO_PIX_FMT_RGB565) {
			/* Exact match for our target resolution */
			if (cap->width_min == width && cap->height_min == height) {
				chosen = cap;
				printk("  -> Found exact match!\n");
			}
			/* Keep track of largest RGB565 as fallback */
			if (fallback == NULL || cap->width_min > fallback->width_min) {
				fallback = cap;
			}
		}
		cap++;
	}

	if (chosen == NULL && fallback != NULL) {
		printk("Exact resolution not found, using %ux%u\n", 
		       fallback->width_min, fallback->height_min);
		chosen = fallback;
		width = fallback->width_min;
		height = fallback->height_min;
	}

	if (chosen == NULL) {
		printk("No supported camera format (need RGB565)\n");
		return -ENOTSUP;
	}

	fmt.type = VIDEO_BUF_TYPE_OUTPUT;
	fmt.pixelformat = chosen->pixelformat;
	fmt.width = width;
	fmt.height = height;
	fmt.pitch = 0;

	ret = video_set_format(camera, &fmt);
	if (ret != 0) {
		printk("Set format failed (%d)\n", ret);
		return ret;
	}

	/* Use what the driver actually set */
	width = fmt.width;
	height = fmt.height;
	printk("Format set: %ux%u pitch=%u size=%u\n", fmt.width, fmt.height, fmt.pitch, fmt.size);

	/* Apply omiGlass-style exposure/AEC after format (driver may reset) */
	if (ov2640_i2c_bus) {
		printk("Applying omiGlass exposure/AEC settings...\n");
		ov2640_write_reg(ov2640_i2c_bus, 0xFF, 0x01); /* BANK_SEL = sensor */
		ov2640_write_reg(ov2640_i2c_bus, 0x13, 0xE5); /* COM8 - AGC, AEC, banding */
		ov2640_write_reg(ov2640_i2c_bus, 0x24, 0x40); /* AEW - omiGlass target */
		ov2640_write_reg(ov2640_i2c_bus, 0x25, 0x38); /* AEB - omiGlass floor */
		ov2640_write_reg(ov2640_i2c_bus, 0x26, 0x82); /* VV - omiGlass thresholds */
		ov2640_write_reg(ov2640_i2c_bus, 0x14, 0x08 | (0x02 << 5)); /* COM9 - AGC 8x */
	}
	
	pitch = fmt.pitch ? fmt.pitch : (fmt.width * 2U);

	buffer_count = caps.min_vbuf_count ? caps.min_vbuf_count : 1U;
	if (buffer_count > ARRAY_SIZE(buffers)) {
		buffer_count = ARRAY_SIZE(buffers);
	}

	if (fmt.size == 0) {
		fmt.size = fmt.width * fmt.height * 2U;
	}

	for (uint8_t i = 0; i < buffer_count; i++) {
		/* Allocate video_buffer struct + data buffer from PSRAM */
		size_t total_size = sizeof(struct video_buffer) + fmt.size + CONFIG_VIDEO_BUFFER_POOL_ALIGN;
		bool from_psram = false;
		uint8_t *mem = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, total_size);
		if (mem != NULL) {
			from_psram = true;
		} else {
			/* Fallback to system heap */
			mem = k_malloc(total_size);
		}
		if (mem == NULL) {
			printk("No memory for frame buffer (%u bytes)\n", total_size);
			ret = -ENOMEM;
			goto cleanup;
		}
		printk("Allocated %u bytes from %s\n", total_size, from_psram ? "PSRAM" : "heap");
		buffers[i] = (struct video_buffer *)mem;
		memset(buffers[i], 0, sizeof(struct video_buffer));
		/* Align buffer data */
		uintptr_t data_addr = (uintptr_t)(mem + sizeof(struct video_buffer));
		data_addr = (data_addr + CONFIG_VIDEO_BUFFER_POOL_ALIGN - 1) & ~(CONFIG_VIDEO_BUFFER_POOL_ALIGN - 1);
		buffers[i]->buffer = (uint8_t *)data_addr;
		buffers[i]->size = fmt.size;
		buffers[i]->type = VIDEO_BUF_TYPE_OUTPUT;
		printk("Buffer %d allocated at %p (data at %p, %u bytes)\n", i, buffers[i], buffers[i]->buffer, fmt.size);
		ret = video_enqueue(camera, buffers[i]);
		if (ret != 0) {
			printk("Enqueue failed (%d)\n", ret);
			goto cleanup;
		}
	}

	ret = video_stream_start(camera, VIDEO_BUF_TYPE_OUTPUT);
	if (ret != 0) {
		printk("Stream start failed (%d)\n", ret);
		goto cleanup;
	}
	streaming = true;

	/* Wait for auto-exposure to stabilize before capturing (AEC needs time) */
	printk("Waiting for auto-exposure to stabilize...\n");
	k_sleep(K_MSEC(3000));

	ret = video_dequeue(camera, &dequeued, K_SECONDS(10));
	video_stream_stop(camera, VIDEO_BUF_TYPE_OUTPUT);
	streaming = false;
	if (ret != 0 || dequeued == NULL) {
		printk("No frame received (%d)\n", ret);
		ret = ret ? ret : -EIO;
		goto cleanup;
	}

	printk("Frame bytesused=%u size=%u pitch=%u fmt.pitch=%u\n",
	       dequeued->bytesused, dequeued->size, pitch, fmt.pitch);
	if (dequeued->bytesused && (dequeued->bytesused / fmt.height) > 0) {
		uint32_t computed = dequeued->bytesused / fmt.height;
		if (computed != pitch) {
			printk("Adjusting pitch %u -> %u\n", pitch, computed);
			pitch = computed;
		}
	}

	printk("*** Camera capture successful! ***\n");

	ret = mount_sdcard();
	if (ret != 0) {
		printk("SD card not available - image not saved\n");
		ret = 0; /* Don't treat missing SD card as failure */
		goto cleanup;
	}

	fs_file_t_init(&file);
	ret = fs_open(&file, CAPTURE_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret != 0) {
		printk("Open %s failed (%d)\n", CAPTURE_PATH, ret);
		goto cleanup;
	}

	if (fmt.pixelformat == VIDEO_PIX_FMT_RGB565) {
		ret = bmp_write_rgb565(&file, dequeued->buffer, fmt.width, fmt.height, pitch);
	} else if (fmt.pixelformat == VIDEO_PIX_FMT_YUYV) {
		/* For YUYV, convert to RGB565 first or save raw - for now just log */
		printk("YUYV format - saving raw data\n");
		ret = fs_write_all(&file, dequeued->buffer, dequeued->bytesused);
	} else {
		ret = -ENOTSUP;
	}
	fs_close(&file);
	if (ret != 0) {
		printk("BMP write failed (%d)\n", ret);
		goto cleanup;
	}

	printk("Saved image to %s\n", CAPTURE_PATH);
	ret = 0;

cleanup:
	if (streaming) {
		video_stream_stop(camera, VIDEO_BUF_TYPE_OUTPUT);
	}
	for (uint8_t i = 0; i < buffer_count; i++) {
		if (buffers[i] != NULL) {
			/* Try PSRAM free first, then system heap */
			shared_multi_heap_free(buffers[i]);
		}
	}
	return ret;
}

int main(void)
{
	int err;
	struct bt_le_adv_param adv_params = BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_CONN,
		BT_GAP_ADV_FAST_INT_MIN_2,
		BT_GAP_ADV_FAST_INT_MAX_2,
		NULL);

	printk("Veea device base starting...\n");

	
	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	err = bt_le_adv_start(&adv_params, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		printk("Advertising failed (err %d)\n", err);
		return 0;
	}

	printk("BLE advertising started\n");

	err = capture_bmp_to_sd();
	if (err != 0) {
		printk("Capture failed (%d)\n", err);
	}

	while (true) {
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
