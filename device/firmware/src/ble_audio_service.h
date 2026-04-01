#ifndef BLE_AUDIO_SERVICE_H
#define BLE_AUDIO_SERVICE_H

#include <zephyr/types.h>
#include <stdbool.h>

/* LC3 audio parameters */
#define AUDIO_SAMPLE_RATE_HZ        16000
#define AUDIO_FRAME_DURATION_US     10000   /* 10 ms */
#define AUDIO_BITRATE_BPS           32000   /* 32 kbps */
#define AUDIO_FRAME_BYTES           40      /* 32000 / 8 * 0.010 = 40 bytes per LC3 frame */
#define AUDIO_CHANNELS              1       /* Mono */
#define AUDIO_PCM_SAMPLES_PER_FRAME 160     /* 16000 Hz * 0.010 s */

/*
 * BLE Audio UUIDs – under the omiGlass service (19B10000-...)
 * Audio Data:    19B10001-E8F2-537E-4F6C-D104768A1214
 * Audio Control: 19B10002-E8F2-537E-4F6C-D104768A1214
 */
#define BLE_AUDIO_DATA_CHAR_UUID \
	BT_UUID_128_ENCODE(0x19B10001, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214)

#define BLE_AUDIO_CTRL_CHAR_UUID \
	BT_UUID_128_ENCODE(0x19B10002, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214)

/* Control commands (phone → firmware) */
#define AUDIO_CMD_START  0x01
#define AUDIO_CMD_STOP   0x02

/* Status notifications (firmware → phone) */
#define AUDIO_STATUS_RECORDING  0x01
#define AUDIO_STATUS_STOPPED    0x02
#define AUDIO_STATUS_ERROR      0xFF

/* Frame header: [seq_lo, seq_hi, len_lo, len_hi, ...LC3 data...] */
#define AUDIO_FRAME_HEADER_SIZE 4

void ble_audio_service_init(void);
void ble_audio_service_notify_frame(const uint8_t *lc3_data, uint16_t len,
				    uint16_t seq);
bool ble_audio_service_is_streaming(void);

#endif /* BLE_AUDIO_SERVICE_H */
