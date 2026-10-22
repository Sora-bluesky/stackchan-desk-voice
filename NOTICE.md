# Third-party notices

stackchan-desk-voice is released under the MIT License (see [LICENSE](LICENSE)). It builds on the work below. Each item keeps its own license.

## Stack-chan

ｽﾀｯｸﾁｬﾝ (Stack-chan) is created by Shinya Ishikawa (ししかわ).
<https://github.com/stack-chan/stack-chan>

This firmware is an unofficial fan-made project for the M5Stack StackChan kit. It is not part of the Stack-chan project and is not endorsed by it.

## Referenced code (not bundled)

These projects were used as references. Parameters and protocol details follow them; their source files are not copied into this repository.

| Project | License | What follows it |
|---|---|---|
| M5Stack StackChan factory firmware (<https://github.com/m5stack/StackChan>) | MIT | Servo bus protocol details: servo IDs, zero positions and their NVS keys, 0.3125 deg per step (see `main/servo.h`). Audio codec and I2S setup (see `main/audio.cpp`). Head sensor register setup and touch position calculation (see `main/head_touch.h`). |
| M5Stack StackChan-BSP | MIT | Head sensor sensitivity values |

## Build dependencies (fetched by the ESP-IDF component manager)

Versions are pinned in `dependencies.lock`. Licenses below are taken from the license files shipped with each component.

| Component | Version | License |
|---|---|---|
| ESP-IDF (Espressif) | 5.4.2 | Apache-2.0 |
| espressif/usb_device_uac | 1.3.2 | Apache-2.0 |
| espressif/esp_codec_dev | 1.5.11 | Apache-2.0 |
| espressif/tinyusb (dependency of usb_device_uac) | 0.19.0~3 | MIT |
| espressif/cmake_utilities (dependency of usb_device_uac) | 1.1.1 | Apache-2.0 |
| m5stack/m5gfx | 0.2.33 | MIT |
| m5stack/m5unified | 0.2.26 | MIT |

Binaries built from this repository include code from these components. If you distribute a binary, include the license texts of these components with it.
