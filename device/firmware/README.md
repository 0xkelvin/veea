# Zephyr base for XIAO ESP32-S3 Sense

This folder is a minimal Zephyr application targeting the Seeed Studio
XIAO ESP32-S3 Sense board (board name: `xiao_esp32s3`). It brings up the
console, starts BLE advertising, and captures a single camera frame at
boot. The image is stored as `/SD:/capture.png` on the microSD card. A
board overlay sets the device model/compatible to `veea-device` and the
BLE device name defaults to `veea-device`.

## Build

From an existing Zephyr workspace (with `ZEPHYR_BASE` set):

```sh
west build -b veea-device /Users/kelvin/Veea/veea/device/firmware
```

## Flash

```sh
west flash
```

## Next steps

- **Microphone**: add an I2S microphone node in a board overlay and enable
  the I2S driver config options.
- **Camera**: add a camera sensor node and enable the camera driver once
  your selected sensor is supported by Zephyr on ESP32-S3.
- **BLE services**: add GATT services (battery, device info, custom data)
  in `src/main.c`.
- **Storage**: ensure the microSD is FAT formatted so `/SD:/capture.png`
  can be written.
