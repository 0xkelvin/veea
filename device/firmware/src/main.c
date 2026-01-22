#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <ff.h>

#define DISK_DRIVE_NAME "SD"
#define DISK_MOUNT_PT "/SD:"
#define CAPTURE_PATH DISK_MOUNT_PT "/capture.png"

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
			uint16_t pixel = ((uint16_t)row[2 * x + 1] << 8) | row[2 * x];
			uint8_t r = (pixel >> 11) & 0x1F;
			uint8_t g = (pixel >> 5) & 0x3F;
			uint8_t b = pixel & 0x1F;

			row_buf[1 + (x * 3) + 0] = (r << 3) | (r >> 2);
			row_buf[1 + (x * 3) + 1] = (g << 2) | (g >> 4);
			row_buf[1 + (x * 3) + 2] = (b << 3) | (b >> 2);
		}

		ret = zlib_write_data(&zw, row_buf, 1 + (width * 3U));
		if (ret != 0) {
			k_free(row_buf);
			return ret;
		}
	}
	k_free(row_buf);

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

static int capture_png_to_sd(void)
{
	const struct device *camera = DEVICE_DT_GET(DT_CHOSEN(zephyr_camera));
	struct video_caps caps = { .type = VIDEO_BUF_TYPE_OUTPUT };
	struct video_format fmt = { 0 };
	struct video_buffer *dequeued = NULL;
	struct fs_file_t file;
	uint32_t width = 160;
	uint32_t height = 120;
	uint32_t pitch;
	int ret;
	bool streaming = false;
	uint8_t allocated = 0;

	if (!device_is_ready(camera)) {
		printk("Camera device not ready\n");
		return -ENODEV;
	}

	ret = video_get_caps(camera, &caps);
	if (ret != 0) {
		printk("Camera caps failed (%d)\n", ret);
		return ret;
	}

	const struct video_format_cap *cap = caps.format_caps;
	while (cap && cap->pixelformat != 0) {
		if (cap->pixelformat == VIDEO_PIX_FMT_RGB565) {
			if (!format_supports(cap, width, height)) {
				width = cap->width_min;
				height = cap->height_min;
			}
			break;
		}
		cap++;
	}

	if (!cap || cap->pixelformat == 0) {
		printk("RGB565 not supported by camera\n");
		return -ENOTSUP;
	}

	fmt.type = VIDEO_BUF_TYPE_OUTPUT;
	fmt.pixelformat = VIDEO_PIX_FMT_RGB565;
	fmt.width = width;
	fmt.height = height;
	fmt.pitch = 0;

	ret = video_set_format(camera, &fmt);
	if (ret != 0) {
		printk("Failed to set format (%d)\n", ret);
		return ret;
	}

	pitch = fmt.pitch ? fmt.pitch : (fmt.width * 2U);

	uint8_t buffer_count = caps.min_vbuf_count ? caps.min_vbuf_count : 1U;
	struct video_buffer buffers[2];
	if (buffer_count > ARRAY_SIZE(buffers)) {
		buffer_count = ARRAY_SIZE(buffers);
	}

	for (uint8_t i = 0; i < buffer_count; i++) {
		buffers[i].type = VIDEO_BUF_TYPE_OUTPUT;
		buffers[i].size = fmt.size ? fmt.size : (fmt.width * fmt.height * 2U);
		buffers[i].buffer = k_malloc(buffers[i].size);
		if (buffers[i].buffer == NULL) {
			printk("No memory for frame buffer\n");
			ret = -ENOMEM;
			goto cleanup;
		}
		allocated++;
		ret = video_enqueue(camera, &buffers[i]);
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

	ret = video_dequeue(camera, &dequeued, K_SECONDS(2));
	video_stream_stop(camera, VIDEO_BUF_TYPE_OUTPUT);
	streaming = false;
	if (ret != 0 || dequeued == NULL) {
		printk("No frame received (%d)\n", ret);
		ret = ret ? ret : -EIO;
		goto cleanup;
	}

	ret = mount_sdcard();
	if (ret != 0) {
		goto cleanup;
	}

	fs_file_t_init(&file);
	ret = fs_open(&file, CAPTURE_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret != 0) {
		printk("Open %s failed (%d)\n", CAPTURE_PATH, ret);
		goto cleanup;
	}

	ret = png_write_rgb565(&file, dequeued->buffer, fmt.width, fmt.height, pitch);
	fs_close(&file);
	if (ret != 0) {
		printk("PNG write failed (%d)\n", ret);
		goto cleanup;
	}

	printk("Saved image to %s\n", CAPTURE_PATH);
	ret = 0;

cleanup:
	if (streaming) {
		video_stream_stop(camera, VIDEO_BUF_TYPE_OUTPUT);
	}
	for (uint8_t i = 0; i < allocated; i++) {
		k_free(buffers[i].buffer);
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

	err = capture_png_to_sd();
	if (err != 0) {
		printk("Capture failed (%d)\n", err);
	}

	while (true) {
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
