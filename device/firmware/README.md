# Zephyr firmware for XIAO ESP32-S3 Sense

Wearable device firmware targeting the Seeed Studio XIAO ESP32-S3 Sense
(board name `xiao_esp32s3`).  The device captures images continuously at a
configurable interval and routes each image based on BLE connectivity:

| BLE state | Action |
|-----------|--------|
| Connected + notifications enabled | Stream image over BLE (omiGlass-compatible protocol) |
| Not connected / notifications disabled | Save image to SD card (`/SD:/IMG00000.JPG`) |

## Features

- **Continuous capture** – takes a picture every `CAPTURE_INTERVAL_MS`
  (default 30 s) without stopping.
- **JPEG-first** – attempts JPEG format (native OV2640 hardware compression)
  for the best image quality and smallest file size.  Falls back to RGB565 →
  BMP if the Zephyr driver does not expose JPEG caps.
- **BLE streaming** – exposes an omiGlass-compatible GATT service so the
  same mobile app can receive images over BLE without modification.
  - Service UUID: `19B10000-E8F2-537E-4F6C-D104768A1214`
  - Photo data characteristic: `19B10005-E8F2-537E-4F6C-D104768A1214` (Notify)
- **SD card storage** – numbered files (`IMG00000.JPG`, `IMG00001.JPG`, …)
  when no BLE connection is active.  Files are not overwritten until the
  counter wraps at 100 000.
- **Camera quality** – OV2640 is initialised with full auto-exposure / AGC /
  AWB and JPEG quantisation scale 0x10 (medium-high quality, equivalent to
  approximately quality 50 in Arduino `esp_camera` terms).

## Build

From an existing Zephyr workspace (with `ZEPHYR_BASE` set):

```sh
# Using the canonical Zephyr board qualifier:
west build -b xiao_esp32s3/esp32s3/procpu/sense device/firmware

# The CMakeLists.txt also maps the alias 'veea-device' to the same board:
west build -b veea-device device/firmware
```

## Flash

```sh
west flash
```

## Monitor

```sh
west espressif monitor
```

## Configuration

Key constants in `src/main.c`:

| Symbol | Default | Description |
|--------|---------|-------------|
| `CAPTURE_INTERVAL_MS` | `30000` | Milliseconds between captures |
| `BLE_PHOTO_CHUNK_SIZE` | `200` | Bytes of photo data per BLE notification |

Camera JPEG quality is controlled by the `QS` register in
`ov2640_jpeg_regs[]` (register `0x44`): lower value = better quality /
larger file.

## Next steps

- **Microphone**: add an I2S microphone node in the board overlay and enable
  `CONFIG_I2S` / `CONFIG_AUDIO_DMIC`.
- **Audio streaming**: add an audio data characteristic (UUID `19B10003-…`)
  to stream Opus-encoded audio alongside photos.
- **Mobile app**: implement the matching BLE client for image reassembly and
  AI summarisation.
