cmake_minimum_required(VERSION 3.20.0)

if(DEFINED BOARD AND BOARD STREQUAL "veea-device")
  set(BOARD "xiao_esp32s3/esp32s3/procpu/sense" CACHE STRING "" FORCE)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(veea_wearable)

# Override Zephyr's I2S ESP32 driver with our PDM-patched version.
# This replaces the upstream driver to add PDM RX support on I2S0
# without modifying the Zephyr tree.
if(CONFIG_I2S_ESP32_PDM_RX)
  get_target_property(i2s_sources drivers__i2s SOURCES)
  list(FILTER i2s_sources EXCLUDE REGEX "i2s_esp32\\.c$")
  set_target_properties(drivers__i2s PROPERTIES SOURCES "${i2s_sources}")
  target_sources(drivers__i2s PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/i2s_esp32_pdm.c)
endif()

target_sources(app PRIVATE
  src/main.c
  src/ble_audio_service.c
  src/mic_driver.c
)
