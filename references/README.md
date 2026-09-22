# Official M5Stack StopWatch references

Downloaded from M5Stack's official documentation and GitHub organization on 2026-09-14.

## Documentation

`docs/` contains:

- `C152-StopWatch-product-manual.pdf` — product overview, specifications, pin map, and links
- `C152-StopWatch-Arduino-quick-start.pdf` — Arduino setup and upload flow
- `C152-StopWatch-schematic.pdf` — complete schematic
- `C152-StopWatch-model-size.pdf` — mechanical dimensions
- `ESP32-S3-datasheet.pdf` — MCU datasheet
- `CO5300-display-datasheet.pdf` — 466x466 AMOLED controller
- `CST820B-touch-datasheet.pdf` — touch controller
- `M5PM1-datasheet.pdf` — power-management controller
- `M5IOE1-datasheet.pdf` — IO expander
- `ES8311-audio-codec-datasheet.pdf` — audio codec
- `RX8130CE-rtc-datasheet.pdf` and `RX8130CE-register-manual.pdf` — RTC

Primary source: https://docs.m5stack.com/en/core/StopWatch

## Projects

`projects/` contains shallow local clones:

- `M5StopWatch-UserDemo` — official factory/evaluation firmware; its pinned dependencies have been fetched into `components/`
- `M5_Hardware` — sparse checkout containing `Products/C152_StopWatch`, including structure files
- `M5Unified` — recommended Arduino device abstraction
- `M5GFX` — display/graphics library
- `M5PM1` — power-management library, checked out at `1.0.6`
- `M5IOE1` — IO-expander library, checked out at `1.0.8`

Sources:

- https://github.com/m5stack/M5StopWatch-UserDemo
- https://github.com/m5stack/M5_Hardware
- https://github.com/m5stack/M5Unified
- https://github.com/m5stack/M5GFX
- https://github.com/m5stack/M5PM1
- https://github.com/m5stack/M5IOE1

## Build paths

### ESP-IDF evaluation firmware

The official demo requires ESP-IDF 5.5.4. From `projects/M5StopWatch-UserDemo`:

```sh
idf.py build
idf.py flash
```

Dependencies are already present. Do not run `fetch_repos.py` again unless intentionally refreshing them; the M5PM1 and M5IOE1 copies include patches applied by the official script.

### Arduino

Select the `M5StopWatch` board and install `M5Unified` plus `M5GFX`. The official docs recommend starting with the `BarGraph` example. The product page also provides this PlatformIO environment:

```ini
[env:m5stack-stopwatch]
platform = espressif32 @ 6.12.0
board = esp32s3box
framework = arduino
board_build.partitions = default_16MB.csv
board_upload.flash_size = 16MB
board_upload.maximum_size = 16777216
board_build.arduino.memory_type = qio_opi
monitor_speed = 115200
build_flags =
    -DESP32S3
    -DBOARD_HAS_PSRAM
    -DCORE_DEBUG_LEVEL=5
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DARDUINO_USB_MODE=1
lib_deps =
    M5Unified = https://github.com/m5stack/M5Unified
    M5GFX = https://github.com/m5stack/M5GFX
    M5PM1 = https://github.com/m5stack/M5PM1
    M5IOE1 = https://github.com/m5stack/M5IOE1
```
