# stm32-ota-system

Over-the-air firmware updates for an STM32F401 with A/B flash slots.

- `stm32/b2_bootloader` — bootloader: receives an image over UART, verifies CRC32, flashes the inactive slot.
- `stm32/b2_app` — application firmware, built per slot.
- `esp32/ota_bridge` — ESP32 sketch: polls the Pi over WiFi, downloads the image, forwards it to the STM32 over UART.
- `pi/app.py` — Flask server on the Raspberry Pi that serves firmware metadata and binaries.

## Setup

Before building the ESP32 sketch, create its local config file:

```sh
cp esp32/ota_bridge/secrets.h.example esp32/ota_bridge/secrets.h
```

Then edit `esp32/ota_bridge/secrets.h` and fill in:

| Macro | Value |
| --- | --- |
| `WIFI_SSID` | Your WiFi network name |
| `WIFI_PASS` | Your WiFi password |
| `PI_HOST` | Base URL of the Pi firmware server, e.g. `http://192.168.1.100:5000` |

`secrets.h` is gitignored so real credentials stay out of the repo. `secrets.h.example`
is tracked and is the reference for which macros must be defined.
