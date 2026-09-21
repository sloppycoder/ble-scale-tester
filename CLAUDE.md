# ble-scale-tester

PlatformIO test programs for ESP32 boards. The current focus is **GaggiMate**
(`../gaggimate`), an open-source espresso machine controller that acts as a
BLE **central**: its display firmware scans for and connects to Bluetooth
coffee scales via the `esp-arduino-ble-scales` library, then reads
weight/flow-rate notifications and sends tare/timer commands back.

The test programs in this repo run on a physical ESP32 board and act as a
**BLE peripheral emulating a scale** (advertising the right name, GATT
service/characteristic UUIDs, and notification byte format), so GaggiMate's
scale-connection logic can be exercised on the bench without an actual Acaia,
Bookoo, Decent, Felicita, etc. scale in hand. Programs may also act as a BLE
**central/scanner** to independently verify what a real scale (or the
emulator) is broadcasting.

## Primary dev board

**M5Stack AtomS3R** ([docs](https://docs.m5stack.com/en/core/AtomS3R))
- ESP32-S3-PICO-1-N8R8, 8MB flash, 8MB octal PSRAM (PSRAM unused by this
  BLE-only test firmware)
- PlatformIO board id: **no dedicated `m5stack-atoms3r` profile exists yet**
  in this platform install (`pio boards | grep -i atom`), so this repo builds
  it as `board = m5stack-atoms3` — same ESP32-S3, native-USB-CDC layout, 8MB
  flash; the AtomS3R's extra PSRAM/IMU/display/ToF pins are simply unused by
  this program
- Native USB (USB-CDC), no separate USB-serial chip/driver needed on macOS —
  set `-DARDUINO_USB_CDC_ON_BOOT=1` so `Serial`/logs come over the native USB
  port
- Previously bench-tested on a Seeed Studio XIAO ESP32S3
  (`seeed_xiao_esp32s3`, also used as `env:display-headless-8m` in
  `../gaggimate/platformio.ini`); that board profile remains a known-good
  reference if switching back

## Sibling repos (context, not vendored into this repo)

| Path | Purpose |
|---|---|
| `../gaggimate` | GaggiMate firmware (display + controller). `platformio.ini` pins the exact toolchain/library versions to match: `espressif32@6.12.0`, `h2zero/NimBLE-Arduino@^1.4.0`, C++17. `src/display/plugins/BLEScalePlugin.cpp` is where scale scanning/connection lives. |
| `../esp-arduino-ble-scales` | The BLE scale client library GaggiMate depends on. `src/scales/*.{h,cpp}` has one class per supported scale (Acaia, Bookoo, Decent, Difluid, Eclair, Eureka, Felicita, Timemore, Varia, WeighMyBrew, MyScale, Dot) — each documents that scale's GATT service/characteristic UUIDs and wire protocol. **This is the spec to read when writing an emulator** for a given scale. |
| `../references/esp-arduino-ble-scales` | This is the form of the same scale plugin from [gaggiuino](https://gaggiuino.github.io/#/) project. It contains Timremore dot support and we may need to borrow some code from it |

GaggiMate `master` pulls the scales library via
`https://github.com/gaggimate/esp-arduino-ble-scales#v1.0.3` (a pinned tag cut
from the library's **`v1.x`** branch), not the local checkout. This tester
builds against the local sibling checkout instead, which **must be on `v1.x`
or a branch based on it** so it matches what GaggiMate `master` builds.

The library's `main` branch is a separate line for the NimBLE 2.x /
pioarduino migration (`feature/pioarduino-move` in gaggimate, not yet on
`master`). It does not compile against NimBLE-Arduino 1.4.x, so it cannot be
used with this tester's current toolchain.

`v1.x` has no `library.json`, so a `symlink://` entry in `lib_deps` fails with
`MissingPackageManifestError`. `platformio.ini` uses
`lib_extra_dirs = lib_links` instead, where
`lib_links/esp-arduino-ble-scales` is a symlink to the sibling checkout. If a
stale `.pio/libdeps/*/esp-arduino-ble-scales.pio-link` from an older config
triggers the same error, delete that file.

Toolchain matches GaggiMate `master`: `espressif32@6.12.0`,
`h2zero/NimBLE-Arduino@^1.4.0`, `-std=gnu++17`, plus its
`CONFIG_NIMBLE_CPP_LOG_LEVEL` / `CONFIG_BT_NIMBLE_PINNED_TO_CORE` flags.

## Tools required to write/run a test program

1. **PlatformIO Core (CLI)** — already installed at
   `~/.platformio/penv/bin/pio` (not currently on `PATH`; either add
   `~/.platformio/penv/bin` to `PATH` or invoke it by full path). VS Code +
   the PlatformIO IDE extension is an alternative front-end over the same
   Core install.
2. **Python 3** — required by PlatformIO itself (already present:
   Python 3.14 on this machine, plus PlatformIO's bundled `penv`).
3. **A USB-C cable** (data-capable, not charge-only) to flash/monitor the
   AtomS3R.
4. **NimBLE-Arduino library, pinned to `^1.4.0`** — declare it in this
   project's `platformio.ini` `lib_deps` so the emulator speaks the same
   NimBLE API version GaggiMate uses.
5. **espressif32 platform**, ideally pinned to `6.12.0` to match GaggiMate's
   toolchain exactly (`platform = espressif32@6.12.0` in `platformio.ini`).
6. **A second BLE inspector, independent of this repo's code**, to verify
   what the emulator is actually advertising/notifying before testing against
   GaggiMate itself:
   - **nRF Connect for Mobile** (iOS/Android, Nordic) — browse
     advertisements, connect, read/write/subscribe to characteristics.
   - **LightBlue** (macOS/iOS) — similar GATT browser, handy if working at a
     Mac.
   - **`bleak`** (Python) — for scripting a quick BLE central from the host
     machine, e.g. to assert notification payloads in an automated test
     rather than eyeballing an app.
7. **A BLE sniffer**, only needed if reverse-engineering a *real* scale's
   protocol from scratch rather than following `esp-arduino-ble-scales`'s
   existing implementation — e.g. Wireshark + Nordic's nRF Sniffer for
   Bluetooth LE, or macOS's `PacketLogger` (via Additional Tools for Xcode).
8. **A serial monitor** — `pio device monitor`, `screen`, or the VS Code
   PlatformIO monitor pane; GaggiMate's own `platformio.ini` also enables
   `esp32_exception_decoder` as a monitor filter, worth reusing here.

## Suggested `platformio.ini` starting point

```ini
[env:m5stack_atoms3r]
platform = espressif32@6.12.0
board = m5stack-atoms3
framework = arduino
monitor_speed = 115200
build_unflags = -std=gnu++11
build_flags =
    -std=gnu++17
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DCORE_DEBUG_LEVEL=3
monitor_filters =
    esp32_exception_decoder
    time
    default
lib_deps =
    h2zero/NimBLE-Arduino@^1.4.0
```

## Common commands

```sh
pio run                          # build
pio run -t upload                # flash the AtomS3R
pio device monitor               # serial monitor
pio run -t upload -t monitor     # flash then monitor
```
(prefix with `~/.platformio/penv/bin/` if `pio` isn't on `PATH`)
