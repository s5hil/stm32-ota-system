# **stm32-ota-system**

## **Introduction**

This project is an over-the-air (OTA) firmware update system for the STM32F401RE. When a commit is tagged and pushed, GitHub Actions builds the firmware and publishes it as a release. A Raspberry Pi downloads the release, an ESP32 relays it over WiFi to the STM32, and a custom bootloader on the STM32 verifies the image before running it. If the new firmware is corrupt or does not work, the device rolls back to the previous version on its own.

## **Architecture**

```
developer machine
      │  git tag vN
      ▼
GitHub Actions        builds bootloader and both slot variants,
      │               publishes release
      ▼
Raspberry Pi          polls releases, computes CRCs,
      │               serves firmware over HTTP
      ▼
ESP32 bridge          polls the Pi, relays image to STM32 over UART
      │
      ▼
STM32F401RE           bootloader validates, writes to inactive slot,
                      boots on trial, rolls back on failure
```

The main design rule is that **nobody downstream trusts anybody upstream**. The STM32 is the only device that cannot be recovered without physical access, so it makes every important decision: which slot is active, whether an image is intact, and when to roll back. The ESP32 only moves bytes. The Pi only serves files.

| Component | What it does |
| --- | --- |
| `stm32/b2_bootloader/` | Dual-slot flash layout, CRC32 validation, UART receive, trial boot, rollback |
| `stm32/b2_app/` | The application. Built twice from one source, once per slot |
| `esp32/ota_bridge/` | WiFi to UART bridge |
| `pi/` | Flask firmware server and GitHub release poller |
| `.github/workflows/` | Builds all binaries, verifies slot linkage, publishes releases on tag |

## **Flash Memory Map**

The STM32F401RE has 512 KB of flash split into unevenly sized sectors: four of 16 KB, one of 64 KB, and three of 128 KB. A sector is the smallest unit that can be erased. Each slot needs to be erasable on its own, so slot boundaries have to fall on sector boundaries. The three 128 KB sectors are the only ones that give two equal slots.

| Region | Address | Size |
| --- | --- | --- |
| Bootloader | `0x08000000` | 128 KB |
| Slot A | `0x08020000` | 128 KB |
| Slot B | `0x08040000` | 128 KB |
| Metadata | `0x08060000` | 128 KB |

The **metadata** sector holds a 48 byte record with the active slot, a boot confirmation flag, an attempt counter, and the size, CRC, and version for each slot.

## **Safety Mechanisms**

* **CRC32 validation:** The STM32 computes a CRC32 over the image using its hardware CRC unit, after the image is written to flash. If it does not match the expected value, the image is discarded before it ever boots. This catches corrupted bytes from the transfer or a bad flash write.

* **Boot confirmation:** A new image boots in **trial mode**. The application clears the trial flag after running for 10 seconds. If the image fails to confirm itself three times, the bootloader switches back to the previous slot. This catches an image that is intact but does not run correctly.

* **Watchdog:** The IWDG resets the chip if the application hangs. Without it, a hung application would never reboot, so the attempt counter would never increment and rollback would never trigger.

* **Atomic commit:** Metadata is only written after the full image is verified in flash. If a transfer is interrupted, nothing about what boots has changed.

## **Update Protocol**

The ESP32 and STM32 bootloader talk over UART at 115200 baud. Every boot, the bootloader opens a short window to check for an update before running the application.

**Step 1: The STM32 asks if there is an update**

On boot, the bootloader sends `?` along with its current build number and which slot is free. It then waits 1 second for a reply. If nothing comes back, it boots the current application as normal. This keeps the startup delay small when there is no update.

**Step 2: The ESP32 offers an image**

If the ESP32 has an image with a higher build number than the one the STM32 reported, it replies with `U`. Otherwise it stays silent.

**Step 3: The ESP32 describes the image**

The ESP32 sends a header with three fields: the image size in bytes, its CRC32, and its build number.

**Step 4: The STM32 prepares the slot**

The STM32 checks the header. If the size is valid, it replies `OK` and erases the free slot. Erasing a 128 KB sector takes 1 to 2 seconds, so when the erase is finished the STM32 sends `R` to signal that it is ready to receive data.

**Step 5: The image is transferred**

The ESP32 sends the image in 256 byte chunks. After each chunk, the STM32 writes it to flash and replies `A`. The ESP32 waits for that `A` before sending the next chunk. This is needed because the STM32 cannot read from UART while it is writing to flash, so any bytes sent during a write would be lost.

**Step 6: The STM32 verifies and commits**

Once the last chunk is written, the STM32 computes the CRC32 over the image in flash and compares it to the value from the header. If they match, it updates the metadata to point at the new slot, marks the image as a trial, and resets. If they do not match, it discards the image and boots the old one.

## **Getting Started**

### **What You'll Need**

* **STM32CubeCLT** or the **Arm GNU Toolchain 14.3**
* **CMake** and **Ninja**
* **Arduino IDE** with ESP32 board support
* **Python 3** with the `flask` and `requests` packages

### **Hardware**

* NUCLEO-F401RE
* ESP32-S3 DevKitC
* Raspberry Pi, or any Linux machine on the same network

Connect the STM32 and ESP32 with three jumper wires:

| STM32 | | ESP32 |
| --- | --- | --- |
| PA9 (USART1_TX) | → | GPIO18 |
| PA10 (USART1_RX) | ← | GPIO17 |
| GND | ↔ | GND |

The ground wire is required. Both boards are powered by separate USB cables and need a shared reference. Do not connect the 3V3 or 5V pins together.

### **Building the Firmware**

```
cmake -B build/bootloader -G Ninja -DCMAKE_BUILD_TYPE=Debug stm32/b2_bootloader
cmake --build build/bootloader

cmake -B build/slot_a -G Ninja -DLINKER_SCRIPT=slot_a.ld -DCMAKE_BUILD_TYPE=Debug stm32/b2_app
cmake --build build/slot_a

cmake -B build/slot_b -G Ninja -DLINKER_SCRIPT=slot_b.ld -DCMAKE_BUILD_TYPE=Debug stm32/b2_app
cmake --build build/slot_b
```

### **Setting Up the ESP32**

Copy the example secrets file and fill in your values.

```
cp esp32/ota_bridge/secrets.h.example esp32/ota_bridge/secrets.h
```

Set your WiFi SSID, password, and the Pi's address. Then flash `ota_bridge.ino` with **USB CDC On Boot** enabled in the board settings.

### **Setting Up the Raspberry Pi**

```
cd pi
pip install flask requests
python3 app.py
```

This starts the firmware server on port 5000. To pull new releases automatically, run the poller on a schedule.

```
python3 github_poller.py
```

Set `REPO` at the top of `github_poller.py` to your own `owner/repo`.

### **Flashing the Board for the First Time**

The bootloader will halt if the metadata sector is blank, so a new board needs three things written over SWD using STM32CubeProgrammer:

1. `b2_bootloader.bin` at `0x08000000`
2. `app_slot_a.bin` at `0x08020000`
3. An initial metadata record at `0x08060000`. See `ota_metadata.h` for the layout.

After this the device updates itself. Tag a commit `vN`, push it, and the next time the STM32 boots it will pick up the new firmware.

## **Built With**

* [STM32CubeMX](https://www.st.com/en/development-tools/stm32cubemx.html) - Peripheral configuration and HAL code generation
* [CMake](https://cmake.org/) and [Ninja](https://ninja-build.org/) - Build system
* [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) - Cross compiler
* [GitHub Actions](https://github.com/features/actions) - CI and release publishing
* [Flask](https://flask.palletsprojects.com/) - Firmware server
* [Arduino ESP32](https://github.com/espressif/arduino-esp32) - Bridge firmware

## **Roadmap**

- [ ] Enforce downgrade protection on the STM32 instead of the ESP32
- [ ] Store two copies of the metadata so a power loss during a rewrite cannot brick the device
- [ ] Sign firmware images so the bootloader can verify who built them, not just that they arrived intact
- [ ] Move the shared header and flash write code into one module used by both STM32 projects