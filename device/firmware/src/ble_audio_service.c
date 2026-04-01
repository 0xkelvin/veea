#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#include "ble_audio_service.h"
#include "mic_driver.h"

static struct bt_uuid_128 audio_svc_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10000, 0xE8F2, 0x537E,
					     0x4F6C, 0xD104768A1214));
static struct bt_uuid_128 audio_data_uuid =
	BT_UUID_INIT_128(BLE_AUDIO_DATA_CHAR_UUID);
static struct bt_uuid_128 audio_ctrl_uuid =
	BT_UUID_INIT_128(BLE_AUDIO_CTRL_CHAR_UUID);

static bool data_notify_enabled;
static bool ctrl_notify_enabled;

/* Forward declarations */
static void data_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
static void ctrl_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t ctrl_write_cb(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len,
			     uint16_t offset, uint8_t flags);

/*
 * GATT service – must be defined before any function references attrs[].
 *   [0] Primary service
 *   [1] Audio Data char decl
 *   [2] Audio Data char value  ← notify frames
 *   [3] Audio Data CCC
 *   [4] Audio Ctrl char decl
 *   [5] Audio Ctrl char value  ← notify status / write commands
 *   [6] Audio Ctrl CCC
 */
BT_GATT_SERVICE_DEFINE(veea_audio_svc,
	BT_GATT_PRIMARY_SERVICE(&audio_svc_uuid),

	BT_GATT_CHARACTERISTIC(&audio_data_uuid.uuid,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC(data_ccc_changed,
		     BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(&audio_ctrl_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_WRITE,
			       NULL, ctrl_write_cb, NULL),
	BT_GATT_CCC(ctrl_ccc_changed,
		     BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

static void data_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	data_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	printk("Audio data notify %s\n",
	       data_notify_enabled ? "enabled" : "disabled");
}

static void ctrl_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ctrl_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	printk("Audio ctrl notify %s\n",
	       ctrl_notify_enabled ? "enabled" : "disabled");
}

static void send_ctrl_status(uint8_t status)
{
	if (!ctrl_notify_enabled) {
		return;
	}
	const struct bt_gatt_attr *attr = &veea_audio_svc.attrs[5];
	bt_gatt_notify(NULL, attr, &status, sizeof(status));
}

static ssize_t ctrl_write_cb(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len,
			     uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	const uint8_t *cmd = buf;
	if (len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	switch (cmd[0]) {
	case AUDIO_CMD_START:
		printk("Audio CMD_START\n");
		mic_driver_start();
		send_ctrl_status(AUDIO_STATUS_RECORDING);
		break;
	case AUDIO_CMD_STOP:
		printk("Audio CMD_STOP\n");
		mic_driver_stop();
		send_ctrl_status(AUDIO_STATUS_STOPPED);
		break;
	default:
		printk("Unknown audio cmd: 0x%02x\n", cmd[0]);
		return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
	}
	return len;
}

void ble_audio_service_notify_frame(const uint8_t *lc3_data, uint16_t len,
				    uint16_t seq)
{
	if (!data_notify_enabled) {
		return;
	}
	if (len > AUDIO_FRAME_BYTES) {
		len = AUDIO_FRAME_BYTES;
	}

	uint8_t frame[AUDIO_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES];
	frame[0] = (uint8_t)(seq & 0xFF);
	frame[1] = (uint8_t)((seq >> 8) & 0xFF);
	frame[2] = (uint8_t)(len & 0xFF);
	frame[3] = (uint8_t)((len >> 8) & 0xFF);
	memcpy(&frame[AUDIO_FRAME_HEADER_SIZE], lc3_data, len);

	const struct bt_gatt_attr *attr = &veea_audio_svc.attrs[2];
	bt_gatt_notify(NULL, attr, frame, AUDIO_FRAME_HEADER_SIZE + len);
}

bool ble_audio_service_is_streaming(void)
{
	return data_notify_enabled;
}

void ble_audio_service_init(void)
{
	data_notify_enabled = false;
	ctrl_notify_enabled = false;
	printk("BLE Audio Service initialised\n");
}
