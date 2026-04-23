#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/multi_heap/shared_multi_heap.h>
#include <ff.h>

/* ---- Storage ---- */
#define DISK_DRIVE_NAME "SD"
#define DISK_MOUNT_PT   "/SD:"

/* ---- Capture timing ---- */
#define CAPTURE_INTERVAL_MS  30000   /* one picture every 30 s              */
#define BLE_PHOTO_CHUNK_SIZE 200     /* bytes of photo data per notify      */

/*
 * BLE service / characteristic UUIDs – identical to the omiGlass firmware so
 * the same mobile app can be used without modification.
 *
 * Service:       19B10000-E8F2-537E-4F6C-D104768A1214
 * Photo data:    19B10005-E8F2-537E-4F6C-D104768A1214
 */
#define OMI_SVC_UUID \
	BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x19B10000, 0xE8F2, 0x537E, \
					       0x4F6C, 0xD104768A1214))
#define PHOTO_DATA_UUID \
	BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x19B10005, 0xE8F2, 0x537E, \
					       0x4F6C, 0xD104768A1214))

/* ------------------------------------------------------------------
 * BLE – connection state and GATT service
 * ------------------------------------------------------------------
 *
 * Static GATT service attribute layout:
 *   [0] Primary service declaration
 *   [1] Characteristic declaration
 *   [2] Characteristic value  <-- target for bt_gatt_notify()
 *   [3] CCC descriptor
 */
#define PHOTO_DATA_ATTR_IDX 2

static struct bt_conn *ble_conn;
static bool           ble_connected;
static bool           photo_notify_enabled;

static void photo_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	photo_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	printk("Photo notify %s\n", photo_notify_enabled ? "enabled" : "disabled");
}

BT_GATT_SERVICE_DEFINE(veea_svc,
	BT_GATT_PRIMARY_SERVICE(OMI_SVC_UUID),
	BT_GATT_CHARACTERISTIC(PHOTO_DATA_UUID, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(photo_ccc_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* Advertise the service UUID so phones can filter by it */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL,
		      BT_UUID_128_ENCODE(0x19B10000, 0xE8F2, 0x537E,
					 0x4F6C, 0xD104768A1214)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		(sizeof(CONFIG_BT_DEVICE_NAME) - 1)),
};

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("Connection failed (err %u)\n", err);
		return;
	}
	ble_conn      = bt_conn_ref(conn);
	ble_connected = true;
	printk("BLE connected\n");
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	printk("BLE disconnected (reason %u)\n", reason);
	ble_connected        = false;
	photo_notify_enabled = false;
	if (ble_conn) {
		bt_conn_unref(ble_conn);
		ble_conn = NULL;
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected    = connected_cb,
	.disconnected = disconnected_cb,
};

/* ------------------------------------------------------------------
 * SD card
 * ------------------------------------------------------------------*/
static FATFS fat_fs;
static struct fs_mount_t sd_mount = {
	.type      = FS_FATFS,
	.fs_data   = &fat_fs,
	.mnt_point = DISK_MOUNT_PT,
};
static bool sd_mounted;

/* ------------------------------------------------------------------
 * OV2640 camera sensor
 * ------------------------------------------------------------------*/
static const struct device *ov2640_i2c_bus;

static int ov2640_write_reg(const struct device *i2c, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	return i2c_write(i2c, buf, 2, 0x30);
}

static const uint8_t ov2640_default_regs[][2] = {
	{0xFF, 0x00}, /* BANK_SEL = DSP */
	{0x2c, 0xff},
	{0x2e, 0xdf},
	{0xFF, 0x01}, /* BANK_SEL = sensor */
	{0x3c, 0x32},
	{0x11, 0x00}, /* CLKRC - no clock divider */
	{0x09, 0x02}, /* COM2 - output drive 3x */
	{0x04, 0x28 | 0x08}, /* REG04 */
	{0x13, 0xE7}, /* COM8 - enable AGC, AEC, AWB, banding filter */
	{0x14, 0x08 | (0x07 << 5)}, /* COM9 - AGC gain ceiling 128x (max) */
	{0x15, 0x00}, /* COM10 */
	{0x2D, 0x00}, /* AEC LSB */
	{0x2E, 0x00}, /* AEC MSB */
	{0x45, 0x7F}, /* AECHH - AEC high bits (max exposure time) */
	{0x10, 0xFF}, /* AEC[7:0] - longer exposure */
	{0x2c, 0x0c},
	{0x33, 0x78},
	{0x3a, 0x33},
	{0x3b, 0xfb},
	{0x3e, 0x00},
	{0x43, 0x11},
	{0x16, 0x10},
	{0x39, 0x02},
	{0x35, 0x88},
	{0x22, 0x0a},
	{0x37, 0x40},
	{0x23, 0x00},
	{0x34, 0xa0}, /* ARCOM2 */
	{0x06, 0x02},
	{0x06, 0x88},
	{0x07, 0xc0},
	{0x0d, 0xb7},
	{0x0e, 0x01},
	{0x4c, 0x00},
	{0x4a, 0x81},
	{0x21, 0x99},
	{0x24, 0xA0}, /* AEW - target brightness (very high) */
	{0x25, 0x90}, /* AEB - floor brightness (very high) */
	{0x26, 0xD6}, /* VV - AEC/AGC thresholds */
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
	{0x6d, 0x80},
	{0x6e, 0x00},
	{0x70, 0x02},
	{0x71, 0x94},
	{0x73, 0xc1},
	{0x3d, 0x34},
	{0x5a, 0x57},
	{0x4F, 0xbb}, /* BD50 */
	{0x50, 0x9c}, /* BD60 */
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
	{0xC2, 0x0C}, /* CTRL0 - RGB565 mode */
};

/* JPEG output format – applied after video_set_format(JPEG)
 * QS = 0x10 gives medium-high quality (equivalent to ~quality 50 in
 * Arduino esp_camera terms); lower value = larger file / better quality.
 */
static const uint8_t ov2640_jpeg_regs[][2] = {
	{0xFF, 0x01}, /* BANK_SEL = sensor */
	{0x11, 0x01}, /* CLKRC /2 for JPEG stability */
	{0xFF, 0x00}, /* BANK_SEL = DSP */
	{0xDA, 0x10}, /* IMAGE_MODE – JPEG output */
	{0xD7, 0x03},
	{0xE0, 0x00}, /* RESET – normal operation */
	{0x44, 0x10}, /* QS – JPEG quantization scale */
};

/* RGB565 output format – force OV2640 to output true RGB565 */
static const uint8_t ov2640_rgb565_regs[][2] = {
	{0xFF, 0x00}, /* BANK_SEL = DSP */
	{0xDA, 0x08}, /* IMAGE_MODE – RGB565 output */
	{0xD7, 0x03},
	{0xDF, 0x00},
	{0x33, 0xa0},
	{0x3C, 0x00},
	{0xe1, 0x67},
	{0xC2, 0x0C}, /* CTRL0 – RGB565 mode (not YUV) */
	{0xE0, 0x00}, /* RESET – normal operation */
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

	/* Output format registers are applied per-capture via ov2640_jpeg_regs
	 * or ov2640_rgb565_regs after video_set_format() is called.
	 */
	printk("OV2640 initialization complete, waiting for AEC...\n");
	k_sleep(K_MSEC(500));
	return 0;
}

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

static int mount_sdcard(void)
{
	int ret;

	if (sd_mounted) {
		return 0;
	}

	ret = disk_access_ioctl(DISK_DRIVE_NAME, DISK_IOCTL_CTRL_INIT, NULL);
	if (ret != 0) {
		printk("SD init failed (%d)\n", ret);
		return ret;
	}

	ret = fs_mount(&sd_mount);
	if (ret < 0) {
		printk("SD mount failed (%d)\n", ret);
		return ret;
	}

	sd_mounted = true;
	return 0;
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

/* Write RGB565 frame as a 24-bit BMP (fallback when JPEG is unavailable) */
static int bmp_write_rgb565(struct fs_file_t *file, const uint8_t *rgb565,
			    uint32_t width, uint32_t height, uint32_t pitch)
{
	uint8_t header[54];
	uint32_t row_size = ((width * 3 + 3) / 4) * 4; /* 24-bit, 4-byte row align */
	uint32_t pixel_data_size = row_size * height;
	uint32_t file_size = 54 + pixel_data_size;
	uint8_t *row_buf;
	int ret;

	row_buf = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, row_size);
	if (!row_buf) {
		row_buf = k_malloc(row_size);
	}
	if (!row_buf) {
		printk("BMP: no memory for row buffer (%u bytes)\n", row_size);
		return -ENOMEM;
	}

	memset(header, 0, sizeof(header));
	/* BMP file header */
	header[0] = 'B'; header[1] = 'M';
	header[2] = file_size & 0xFF;
	header[3] = (file_size >> 8) & 0xFF;
	header[4] = (file_size >> 16) & 0xFF;
	header[5] = (file_size >> 24) & 0xFF;
	header[10] = 54; /* pixel data offset */
	/* DIB header (BITMAPINFOHEADER) */
	header[14] = 40;
	header[18] = width & 0xFF;
	header[19] = (width >> 8) & 0xFF;
	header[20] = (width >> 16) & 0xFF;
	header[21] = (width >> 24) & 0xFF;
	header[22] = height & 0xFF;
	header[23] = (height >> 8) & 0xFF;
	header[24] = (height >> 16) & 0xFF;
	header[25] = (height >> 24) & 0xFF;
	header[26] = 1;   /* colour planes */
	header[28] = 24;  /* bits per pixel */
	header[34] = pixel_data_size & 0xFF;
	header[35] = (pixel_data_size >> 8) & 0xFF;
	header[36] = (pixel_data_size >> 16) & 0xFF;
	header[37] = (pixel_data_size >> 24) & 0xFF;

	ret = fs_write_all(file, header, sizeof(header));
	if (ret != 0) {
		shared_multi_heap_free(row_buf);
		return ret;
	}

	/* BMP rows are stored bottom-to-top */
	for (int32_t y = height - 1; y >= 0; y--) {
		const uint8_t *src = rgb565 + y * pitch;

		memset(row_buf, 0, row_size);
		for (uint32_t x = 0; x < width; x++) {
			/* ESP32 DVP outputs RGB565 big-endian */
			uint16_t px = ((uint16_t)src[2 * x] << 8) | src[2 * x + 1];
			uint8_t r = (px >> 11) & 0x1F;
			uint8_t g = (px >> 5)  & 0x3F;
			uint8_t b =  px        & 0x1F;

			/* Expand to 8-bit; store as BGR for BMP */
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

/* ------------------------------------------------------------------
 * BLE photo streaming
 *
 * Protocol (compatible with omiGlass mobile app):
 *   First chunk:      [idx_lo, idx_hi, orientation, data…]  (≤203 bytes)
 *   Subsequent:       [idx_lo, idx_hi, data…]               (≤202 bytes)
 *   End-of-photo:     [0xFF, 0xFF]
 * ------------------------------------------------------------------*/
static int stream_photo_ble(const uint8_t *data, size_t len)
{
	static uint8_t chunk[BLE_PHOTO_CHUNK_SIZE + 3];
	uint16_t frame_idx = 0;
	size_t offset = 0;
	int ret;

	printk("BLE stream: %u bytes\n", len);

	while (offset < len) {
		size_t remaining = len - offset;
		size_t copy_len;
		size_t chunk_len;

		if (frame_idx == 0) {
			/* First chunk carries an orientation byte */
			copy_len  = MIN(remaining, BLE_PHOTO_CHUNK_SIZE - 1U);
			chunk[0]  = 0;    /* frame index low */
			chunk[1]  = 0;    /* frame index high */
			chunk[2]  = 0;    /* orientation: 0° */
			memcpy(&chunk[3], data + offset, copy_len);
			chunk_len = copy_len + 3;
		} else {
			copy_len  = MIN(remaining, BLE_PHOTO_CHUNK_SIZE);
			chunk[0]  = (uint8_t)(frame_idx & 0xFF);
			chunk[1]  = (uint8_t)((frame_idx >> 8) & 0xFF);
			memcpy(&chunk[2], data + offset, copy_len);
			chunk_len = copy_len + 2;
		}

		ret = bt_gatt_notify(NULL, &veea_svc.attrs[PHOTO_DATA_ATTR_IDX],
				     chunk, (uint16_t)chunk_len);
		if (ret != 0) {
			printk("BLE notify failed (%d)\n", ret);
			return ret;
		}

		offset    += copy_len;
		frame_idx++;
		k_sleep(K_MSEC(5)); /* yield so BLE stack can flush */
	}

	/* End-of-photo marker */
	chunk[0] = 0xFF;
	chunk[1] = 0xFF;
	ret = bt_gatt_notify(NULL, &veea_svc.attrs[PHOTO_DATA_ATTR_IDX], chunk, 2);
	if (ret != 0) {
		printk("BLE end-marker notify failed (%d)\n", ret);
	}

	printk("BLE stream complete: %u chunks sent\n", frame_idx);
	return 0;
}

/* ------------------------------------------------------------------
 * capture_and_route – capture one frame and either stream it over
 * BLE (when connected with notifications enabled) or save it to the
 * SD card (numbered IMG%05u.JPG / IMG%05u.BMP).
 * ------------------------------------------------------------------*/
static uint32_t image_counter;
static bool camera_initialized;

static int capture_and_route(void)
{
	const struct device *camera = DEVICE_DT_GET(DT_CHOSEN(zephyr_camera));
	struct video_format fmt = { 0 };
	struct video_buffer *dequeued = NULL;
	struct video_buffer *buffers[CONFIG_VIDEO_BUFFER_POOL_NUM_MAX] = { 0 };
	uint8_t buffer_count = 0;
	bool streaming = false;
	int ret;

	if (!device_is_ready(camera)) {
		printk("Camera device not ready\n");
		return -ENODEV;
	}

	/* Initialize camera sensor only once */
	if (!camera_initialized) {
		if (!ov2640_detected()) {
			printk("Camera not detected on I2C\n");
			return -ENODEV;
		}
		camera_initialized = true;
		k_sleep(K_MSEC(200));
	}

	/*
	 * Force RGB565 – Zephyr ESP32-S3 DVP driver does not produce valid
	 * JPEG output (bytesused is always 0).  We capture RGB565 and let
	 * the mobile app convert to a displayable image.
	 *
	 * Use 320x240 (QVGA) as a good balance between quality and BLE
	 * transfer time.  160x120 is too small; 640x480 is 600 KB which
	 * takes ~60 s over BLE at 200 bytes/chunk.
	 */
	uint32_t width  = 320;
	uint32_t height = 240;

	printk("Capturing RGB565 %ux%u...\n", width, height);

	fmt.type        = VIDEO_BUF_TYPE_OUTPUT;
	fmt.pixelformat = VIDEO_PIX_FMT_RGB565;
	fmt.width       = width;
	fmt.height      = height;
	fmt.pitch       = width * 2;

	ret = video_set_format(camera, &fmt);
	if (ret != 0) {
		printk("Set format failed (%d)\n", ret);
		return ret;
	}

	/* Apply RGB565 OV2640 registers after video_set_format() */
	if (ov2640_i2c_bus) {
		for (size_t i = 0; i < ARRAY_SIZE(ov2640_rgb565_regs); i++) {
			ov2640_write_reg(ov2640_i2c_bus,
					 ov2640_rgb565_regs[i][0],
					 ov2640_rgb565_regs[i][1]);
		}
	}

	/* Use actual format returned by driver */
	width  = fmt.width;
	height = fmt.height;

	if (fmt.size == 0) {
		fmt.size = width * height * 2U;
	}

	uint32_t pitch = fmt.pitch ? fmt.pitch : (width * 2U);

	struct video_caps caps = { .type = VIDEO_BUF_TYPE_OUTPUT };
	ret = video_get_caps(camera, &caps);
	buffer_count = (ret == 0 && caps.min_vbuf_count) ? caps.min_vbuf_count : 1U;
	if (buffer_count > ARRAY_SIZE(buffers)) {
		buffer_count = ARRAY_SIZE(buffers);
	}

	for (uint8_t i = 0; i < buffer_count; i++) {
		size_t total = sizeof(struct video_buffer) + fmt.size
			       + CONFIG_VIDEO_BUFFER_POOL_ALIGN;
		uint8_t *mem = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL,
						       total);
		if (!mem) {
			mem = k_malloc(total);
		}
		if (!mem) {
			printk("No frame buffer memory (%u bytes)\n",
			       (unsigned)total);
			ret = -ENOMEM;
			goto cleanup;
		}

		buffers[i] = (struct video_buffer *)mem;
		memset(buffers[i], 0, sizeof(struct video_buffer));

		uintptr_t da = (uintptr_t)(mem + sizeof(struct video_buffer));
		da = (da + CONFIG_VIDEO_BUFFER_POOL_ALIGN - 1)
		     & ~(uintptr_t)(CONFIG_VIDEO_BUFFER_POOL_ALIGN - 1);
		buffers[i]->buffer = (uint8_t *)da;
		buffers[i]->size   = fmt.size;
		buffers[i]->type   = VIDEO_BUF_TYPE_OUTPUT;

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

	/* Allow auto-exposure to settle */
	printk("Waiting for AEC to settle...\n");
	k_sleep(K_MSEC(2000));

	ret = video_dequeue(camera, &dequeued, K_SECONDS(10));
	video_stream_stop(camera, VIDEO_BUF_TYPE_OUTPUT);
	streaming = false;

	if (ret != 0 || dequeued == NULL) {
		printk("No frame received (%d)\n", ret);
		ret = ret ? ret : -EIO;
		goto cleanup;
	}

	/* Validate we got actual data */
	size_t img_len = dequeued->bytesused;
	if (img_len == 0) {
		/* bytesused=0 means driver didn't report size; use expected size */
		img_len = width * height * 2U;
		printk("Warning: bytesused=0, using expected size %u\n",
		       (unsigned)img_len);
	}

	/* Refine pitch from actual bytes transferred */
	if (dequeued->bytesused) {
		uint32_t computed = dequeued->bytesused / height;
		if (computed && computed != pitch) {
			printk("Pitch adjusted %u -> %u\n", pitch, computed);
			pitch = computed;
		}
	}

	printk("Frame captured: %u bytes (RGB565 %ux%u)\n",
	       (unsigned)img_len, width, height);

	/* Log first 16 bytes for debugging */
	printk("First 16 bytes:");
	for (int i = 0; i < 16 && i < (int)img_len; i++) {
		printk(" %02x", dequeued->buffer[i]);
	}
	printk("\n");

	/* ---------------------------------------------------------------
	 * Route: BLE when connected with notifications enabled,
	 *        otherwise SD card.
	 * ---------------------------------------------------------------*/
	if (ble_connected && photo_notify_enabled) {
		printk("Streaming image over BLE...\n");
		ret = stream_photo_ble(dequeued->buffer, img_len);
	} else {
		char path[32];

		snprintf(path, sizeof(path), DISK_MOUNT_PT "/IMG%05u.BMP",
			 (unsigned)image_counter);

		ret = mount_sdcard();
		if (ret != 0) {
			printk("SD unavailable – image dropped\n");
			ret = 0; /* non-fatal */
			goto cleanup;
		}

		struct fs_file_t file;
		fs_file_t_init(&file);
		ret = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
		if (ret != 0) {
			printk("Open %s failed (%d)\n", path, ret);
			goto cleanup;
		}

		ret = bmp_write_rgb565(&file, dequeued->buffer,
				       width, height, pitch);
		fs_close(&file);

		if (ret != 0) {
			printk("Write to %s failed (%d)\n", path, ret);
			goto cleanup;
		}

		printk("Saved %s\n", path);
		image_counter++;
	}

	ret = 0;

cleanup:
	if (streaming) {
		video_stream_stop(camera, VIDEO_BUF_TYPE_OUTPUT);
	}
	for (uint8_t i = 0; i < buffer_count; i++) {
		if (buffers[i]) {
			shared_multi_heap_free(buffers[i]);
		}
	}
	return ret;
}

/* ------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------*/
int main(void)
{
	int err;
	struct bt_le_adv_param adv_params = BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_CONN,
		BT_GAP_ADV_FAST_INT_MIN_2,
		BT_GAP_ADV_FAST_INT_MAX_2,
		NULL);

	printk("Veea wearable device starting...\n");

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
	printk("Continuous capture every %d ms – BLE streams when connected, "
	       "SD card otherwise\n", CAPTURE_INTERVAL_MS);

	while (true) {
		int ret = capture_and_route();

		if (ret != 0) {
			printk("Capture error (%d), retrying next cycle\n", ret);
		}
		k_sleep(K_MSEC(CAPTURE_INTERVAL_MS));
	}

	return 0;
}
