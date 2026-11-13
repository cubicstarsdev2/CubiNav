## 1.2.2
# TahVia

TahVia is ESP32 firmware for a compact, Bluetooth-connected navigation display. It receives navigation state, route geometry, positioning updates and maneuver information from a companion phone application, then renders a monochrome navigation interface on a 480 × 320 TFT.

The firmware targets the **NodeMCU-32S (ESP32)** and uses the Arduino framework, PlatformIO, TFT_eSPI and NimBLE-Arduino.

> **Project status:** The repository contains the device firmware and its BLE protocol implementation. A companion phone application is not included here.

## Features

- **Bluetooth Low Energy:** NimBLE GATT server with a custom navigation service, RX writes and TX notifications.
- **Secure pairing:** LE Secure Connections, bonding and MITM authentication are enabled by default. The display shows the passkey during pairing.
- **Navigation display:** Idle, navigation and firmware-update screens, with maneuver icons, street labels, route rendering and a vehicle position marker.
- **Incremental route transfer:** Routes are streamed in chunks, acknowledged by the device and committed from a shadow buffer so a replacement route can be prepared without immediately replacing the active one.
- **Position smoothing:** Dead reckoning between position updates, blended position fixes and smoothed heading.
- **Efficient monochrome rendering:** A custom 1-bit tiled renderer compares changed tiles against a shadow copy and sends only the changed screen regions to the TFT.
- **Battery reporting:** Optional ADC-based battery measurement or a fixed percentage for hardware without a connected battery. The level is exposed through the standard BLE Battery Service.
- **BLE firmware updates:** OTA image chunks are written to the inactive update partition and verified before the device switches firmware.

## Hardware

| Component | Configuration |
| --- | --- |
| MCU | ESP32, NodeMCU-32S PlatformIO target |
| Display | 480 × 320 TFT, configured for landscape |
| Display driver | TFT_eSPI (ILI9488 is the target display used by the firmware) |
| Wireless link | Bluetooth Low Energy |
| Battery input | Optional ADC1 input through a voltage divider |

The display's driver, SPI pins and other TFT_eSPI settings are **not configured in `platformio.ini`**. Configure them in the TFT_eSPI `User_Setup.h` used by your PlatformIO environment. The setup must enable `LOAD_GFXFF`, as the UI uses TFT_eSPI free fonts.

The firmware initializes the display with `tft.setRotation(1)` and expects a 480 × 320 landscape surface.

## Getting started

### Requirements

- [PlatformIO](https://platformio.org/) (VS Code extension or PlatformIO Core CLI)
- An ESP32 / NodeMCU-32S board
- A compatible TFT display and a correctly configured TFT_eSPI setup
- A phone application that implements the TahVia BLE protocol

### Build

Clone the repository and build the configured environment:

```sh
git clone https://github.com/cubicstarsdev2/TahVia.git
cd TahVia
pio run
```

The PlatformIO environment is `nodemcu-32s`. To upload over the board's configured serial port:

```sh
pio run -t upload
```

To open the serial monitor:

```sh
pio device monitor -b 9600
```

The firmware prints a once-per-second runtime statistics line, including loop rate, rendered pixel throughput, tile budget, pending dirty tiles, BLE message counters and available route-buffer slots.

## Configuration

Build-time settings are defined primarily in `src/config.h`, `src/battery.h`, `src/ota.h` and the renderer headers. PlatformIO build flags can override the `#ifndef`-guarded options.

### BLE

| Setting | Default | Description |
| --- | --- | --- |
| `DEVICE_NAME` | `TahVia` | BLE advertised device name |
| `BLE_SERVICE_UUID` | `7a8f0001-3c5e-4b1a-9d27-5e1f4a6b7c80` | Navigation service |
| `BLE_RX_UUID` | `7a8f0002-3c5e-4b1a-9d27-5e1f4a6b7c80` | Phone-to-device write characteristic |
| `BLE_TX_UUID` | `7a8f0003-3c5e-4b1a-9d27-5e1f4a6b7c80` | Device-to-phone notification characteristic |
| `BLE_MTU` | `247` | Requested ATT MTU |
| `BLE_MAX_MSG` | `244` | Maximum accepted message size |
| `BLE_RXQ_DEPTH` | `32` | Incoming-message queue depth |
| `BLE_REQUIRE_PAIRING` | `1` | Require encrypted, authenticated pairing |

For bench testing only, pairing can be disabled with `-DBLE_REQUIRE_PAIRING=0`. Do not use an open BLE link for normal operation.

The NimBLE implementation detects the installed NimBLE-Arduino major version and supports both the 1.x and 2.x callback APIs. If needed, `TAHVIA_NIMBLE_V2` can be forced with a build flag.

### Battery

Battery reporting has three modes, in priority order:

1. **Fixed percentage:** define `BATT_STATIC_PCT` from 0 to 100. This is the default build configuration in `platformio.ini` (`100`) for operation without a battery connected; the ADC is not read.
2. **ADC measurement:** remove the fixed-percentage build flag and define `BATT_ADC_PIN` as an ADC1 pin. Set `BATT_DIVIDER` to the ratio `Vbat / Vpin` for the resistor divider. The default ratio is `2.0`.
3. **Unavailable:** if neither option is enabled, the UI reports an unknown level (`--`) and the BLE Battery Service is not periodically updated.

ADC readings are filtered and sampled every two seconds. The voltage-to-percentage conversion uses a built-in lookup table intended for a Li-ion/LiPo-style cell voltage range. Calibrate it for the actual battery chemistry and pack arrangement before relying on the displayed percentage.

Example build flags for an ADC-connected battery (adjust the pin and divider for your hardware):

```ini
build_flags =
    -DBATT_ADC_PIN=34
    -DBATT_DIVIDER=2.0f
```

### Display renderer and frame budget

The renderer is implemented in `src/monotileRenderer.h` and `src/monotileRenderer.cpp`.

- The base layer is a persistent 1-bit framebuffer.
- The overlay is a per-frame list of drawing commands.
- Dirty tiles are composed and compared against a shadow image of what was last sent to the display.
- Only the changed rectangle within each tile is transmitted.
- Navigation rendering is budgeted so remaining dirty tiles can be sent on subsequent frames.

Useful compile-time options include:

| Option | Default | Description |
| --- | --- | --- |
| `MTR_WIDTH` / `MTR_HEIGHT` | `480` / `320` | Logical display dimensions |
| `MTR_TILE` | `8` | Tile side length in pixels; must be a multiple of 8 and divide both display dimensions |
| `MTR_MAX_CMDS` | `96` | Maximum overlay commands |
| `MTR_STATS` | `1` | Measure renderer composition and display-push time |
| `NAV_MAX_TILES` | Derived | Maximum navigation tiles sent per frame; `0` enables adaptive budgeting |
| `NAV_MAX_PROC` | `0` | Maximum tiles composed/compared per navigation frame; `0` is unlimited |

The renderer's internal framebuffer and display shadow each use one bit per pixel. The display output path converts the monochrome pixels to the configured TFT colors.

### OTA

OTA parameters are in `src/ota.h`. Notable defaults:

| Setting | Default | Description |
| --- | --- | --- |
| `OTA_WINDOW` | `16` | Maximum unacknowledged chunks in flight |
| `OTA_ACK_EVERY` | `8` | Acknowledge every N successfully received chunks |
| `OTA_IDLE_TIMEOUT_MS` | `15000` | Abort after 15 seconds without a chunk |
| `OTA_MIN_BATT_PCT` | `30` | Refuse an update below this level when battery level is known |
| `OTA_ERROR_SHOW_MS` | `4000` | Error-screen duration |
| `OTA_REBOOT_DELAY_MS` | `1500` | Delay after successful completion before reboot |

The phone-side updater is expected to obtain `firmware.bin` (the source code comments describe GitHub Pages hosting) and send it over the BLE protocol. The device writes to the next OTA partition; the running image is not overwritten. The image size and MD5 are checked before the new partition is selected. A failed, interrupted or timed-out update leaves the previous firmware in place.

## Firmware architecture

The main loop owns the navigation model and renderer. BLE callbacks run in NimBLE's task and only copy incoming data into a FreeRTOS queue; message parsing and model updates happen later in `loop()`. This keeps the UI and navigation state out of the BLE callback context.

| File | Responsibility |
| --- | --- |
| `src/main.cpp` | Hardware initialization, boot sequence, main loop, screen selection, frame pacing and runtime statistics |
| `src/config.h` | BLE identity, UUIDs, screen dimensions and timing constants |
| `src/ble_nav.h/.cpp` | NimBLE GATT server, pairing, RX queue, protocol parsing, acknowledgements and battery notifications |
| `src/protocol.h` | Protocol message IDs, enums and firmware version encoding |
| `src/route_ring.h` | Fixed-capacity route-point ring buffer |
| `src/pose_filter.h` | Dead reckoning, position-fix blending and heading smoothing |
| `src/nav_model.h/.cpp` | Shared navigation state, route swapping, cursor tracking and route trimming |
| `src/monotileRenderer.h/.cpp` | 1-bit tiled framebuffer, overlay commands, dirty tracking and TFT output |
| `src/ui.h/.cpp` | Boot, idle, navigation and update screens |
| `src/tv_fonts.h` | Bitmap font data used by the UI |
| `src/battery.h` | Battery measurement and percentage reporting |
| `src/ota.h/.cpp` | BLE OTA update state machine and flash writes |

## BLE protocol overview

The wire protocol is defined in `src/protocol.h` and parsed in `src/ble_nav.cpp`. It is a binary, message-type-based protocol. Multi-byte numeric values are read and written in the ESP32's native little-endian representation.

The phone connects to the custom service and sends messages through the RX characteristic. The device sends acknowledgements and responses through TX notifications.

| Message | ID | Direction | Purpose |
| --- | ---: | --- | --- |
| `HELLO` | `0x01` | Phone → device | Start a session and negotiate protocol compatibility |
| `REBASE` | `0x03` | Phone → device | Shift the local route/position coordinate origin |
| `POSE` | `0x10` | Phone → device | Position, heading, speed and distance-to-turn update |
| `ROUTE_BEGIN` | `0x20` | Phone → device | Begin uploading a route into the shadow buffer |
| `ROUTE_CHUNK` | `0x21` | Phone → device | Append delta-encoded route points |
| `ROUTE_COMMIT` | `0x22` | Phone → device | Make the uploaded route active |
| `ROUTE_CLEAR` | `0x23` | Phone → device | Clear a route |
| `MANEUVER` | `0x30` | Phone → device | Set maneuver icon and street label |
| `CONFIG` | `0x40` | Phone → device | Set zoom and display preferences |
| `NAV_STATE` | `0x50` | Phone → device | Set navigation state |
| `OTA_BEGIN` | `0x60` | Phone → device | Begin firmware update |
| `OTA_CHUNK` | `0x61` | Phone → device | Transfer firmware data |
| `OTA_END` | `0x62` | Phone → device | Finish and verify firmware update |
| `OTA_ABORT` | `0x63` | Phone → device | Abort firmware update |
| `HELLO_ACK` | `0x81` | Device → phone | Return protocol version, firmware version, display size and route capacity |
| `ROUTE_ACK` | `0x82` | Device → phone | Report route transfer status, next expected point and free slots |
| `OTA_ACK` | `0x83` | Device → phone | Report OTA transfer status/progress |

### Route and coordinate model

- Route points use signed 32-bit local coordinates in **decimeters** (`x` east, `y` north).
- Route chunks contain signed 16-bit delta pairs (`dx`, `dy`) relative to the previous point.
- The route ring buffer holds up to **1,024 points**.
- A second ring buffer acts as a shadow route while a new route is being transferred.
- After a successful commit, the active and shadow buffers are swapped.
- As the navigation cursor advances, old points behind it are trimmed while retaining a short section behind the marker. This frees capacity for continued streaming.

### Position model

The `POSE` message supplies position in decimeters, heading in tenths of a degree, speed in centimeters per second, a validity flag and distance to the next turn. The pose filter advances the position using speed and heading between fixes, blends incoming fixes, and smooths heading. A fix becomes stale after `POSE_TIMEOUT_MS` (3 seconds by default).

### Versioning

The firmware version is defined by `FW_VERSION_STRING` in `src/protocol.h` (currently `1.1.0`). It is encoded into a 16-bit value for BLE responses: major in bits 15–8, minor in bits 7–4 and patch in bits 3–0. The current encoding therefore supports major versions 0–255 and minor/patch values 0–15.

When preparing a release, update `FW_VERSION_STRING` and the `## x.y.z` version line at the top of this README to the same version. The phone application reads that README version when deciding whether an update is available.

## Repository layout

```text
TahVia/
├── include/             # Project header include directory
├── lib/                 # Project-local PlatformIO libraries
├── src/                 # Firmware source
│   ├── battery.h
│   ├── ble_nav.cpp
│   ├── ble_nav.h
│   ├── config.h
│   ├── main.cpp
│   ├── monotileRenderer.cpp
│   ├── monotileRenderer.h
│   ├── nav_model.cpp
│   ├── nav_model.h
│   ├── ota.cpp
│   ├── ota.h
│   ├── pose_filter.h
│   ├── protocol.h
│   ├── route_ring.h
│   ├── tv_fonts.h
│   ├── ui.cpp
│   └── ui.h
├── test/                # PlatformIO test directory
└── platformio.ini       # Build configuration
```

## License

No license file is currently present in this repository. Unless a license is added, the source remains subject to the default copyright rules; contact the repository owner before redistributing or reusing it.
