# Credits

MESHtop (formerly the ScopeThing panel, renamed at v3.0) is MIT-licensed (see `LICENSE`), by DudeTested /
HamRadioDude, and grew out of ScopeThing for the Car Thing. It stands on a lot of other people's work. This file records
what we use, what we learned from, and under what licence. Please keep it up to date with every change
that draws on outside work.

## Protocols and reference implementations

| Project | Licence | How MESHtop uses it |
|---|---|---|
| [MeshCore](https://github.com/meshcore-dev/MeshCore) (Scott Powell / ripplebiz and the MeshCore contributors) | MIT | The companion-radio protocol MESHtop speaks over Bluetooth (command and push codes, frame layouts), the hashtag channel-key rule, and the path-length encoding (low 6 bits = hops, top 2 bits = hash size − 1, from `src/Packet.h`). Also the stats request (`CMD_GET_STATS`, v3.0 radio health) and node discovery (`CTL_TYPE_NODE_DISCOVER_REQ` / `_RESP` via `CMD_SEND_CONTROL_DATA` and push `0x8E`, v3.0 "Who can hear me?"). Reference read at commit `75b04ef`. No firmware code is compiled into MESHtop; the protocol client in `src/core/companion.cpp` is our own implementation. |
| [meshcore-open](https://github.com/zjs81/meshcore-open) (zjs81) | MIT | Reference for app behaviour: the radio presets table (USA/Canada, EU/UK, AU, NZ…) and how a companion app counts channel-message repeats from the radio's raw-receive log (`PUSH_CODE_LOG_RX_DATA`). Also the trace-route convention (stored route + target, mirrored back), and the message-route view (v2.1.35): every heard copy kept as a path variant, and repeater IDs resolved by walking the path back from your own location, picking the nearest match for each hop (their `PathHopResolver`). Reference read at commit `34ea0cb`. No Dart code copied. |
| [CoreScope](https://github.com/Kpa-clawbot/CoreScope) (Kpa-clawbot and contributors), a self-hosted MeshCore network analyzer | GPL-3.0-or-later | Live network data (nodes, observers, packets, channels) read from a CoreScope server's public API and WebSocket feed, over the network. **No CoreScope code is copied or compiled into MESHtop**, so its GPL does not apply to MESHtop's code. Default server: the Chicagoland Mesh instance, `corescope.chicagolandmesh.org`, run by its community; thanks to them, and to every observer operator who feeds it. Point MESHtop at your own area's server in the settings. |
| WADAMESH | GPL-3.0 | Looked at for ideas only. **No code copied** (GPL is not compatible with shipping inside an MIT project). |

## Libraries compiled into the firmware

| Library | Licence |
|---|---|
| [LVGL](https://github.com/lvgl/lvgl) v9.1 (incl. its bundled lodepng, TJpgDec and QR code generator) | MIT (lodepng: zlib; TJpgDec: ChaN's BSD-style licence; qrcodegen: MIT) |
| [LovyanGFX](https://github.com/lovyan03/LovyanGFX) (GT911 touch driver) | FreeBSD (BSD-2-Clause) |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | MIT |
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) | Apache-2.0 |
| [arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets) | LGPL-2.1 |
| [WiFiManager](https://github.com/tzapu/WiFiManager) | MIT |
| [Arduino-ESP32](https://github.com/espressif/arduino-esp32) core, via [pioarduino](https://github.com/pioarduino/platform-espressif32) | LGPL-2.1 (core), Apache-2.0 (platform) |
| [ESP-IDF](https://github.com/espressif/esp-idf) v5.5 (rebuilt with the settings below; its FatFs with exFAT switched on) | Apache-2.0 (FatFs: ChaN's BSD-style licence) |
| Montserrat font (as shipped with LVGL) | SIL Open Font License 1.1 |

## Data and services

- Map tiles © [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors (ODbL), served by
  the OpenStreetMap Foundation tile servers under their [tile usage policy](https://operations.osmfoundation.org/policies/tiles/).
  Optional dark tiles © [CARTO](https://carto.com/attributions).
- Map packs (`/maps/<name>/<name>.pmtiles`) in the [PMTiles v3](https://github.com/protomaps/PMTiles/blob/main/spec/v3/spec.md)
  format; the panel's reader (`src/core/tiles.cpp`) is written from the spec. The map pack maker in `tools/mappack`
  draws them from the [Protomaps basemap](https://github.com/protomaps/basemaps) daily build (OpenStreetMap data, ODbL;
  styles BSD-3-Clause / CC0, icons MIT, Noto Sans fonts under the SIL OFL) with
  [MapLibre Native](https://github.com/maplibre/maplibre-native) (BSD-2-Clause) and [sharp](https://sharp.pixelplumbing.com)
  (Apache-2.0); those are installed by npm on your computer, not shipped in the firmware.
- Place search by [Nominatim](https://nominatim.org/) / OpenStreetMap (usage policy: one request at a time,
  identifying User-Agent).

## Tools mentioned in the install steps (not shipped)

- [esptool-js](https://github.com/espressif/esptool-js) (Espressif, Apache-2.0) and
  [Adafruit WebSerial ESPTool](https://github.com/adafruit/Adafruit_WebSerial_ESPTool) (Adafruit, MIT): web
  flashers for installing the firmware.
- [MeshCore web flasher](https://flasher.meshcore.io) for putting Companion Radio firmware on your radio.

## Hardware know-how

- Elecrow CrowPanel Advance 7" wiki and examples: display pins and timings, the STC8H coprocessor's
  backlight/touch commands, and Elecrow support's advice to run PSRAM at 120 MHz to stop RGB-panel flicker.
- Espressif's RGB LCD driver documentation and source (`esp_lcd_panel_rgb.c`) for the
  restart-on-VSYNC and bounce-buffer behaviour.

## Not affiliated

MESHtop is an independent community project. It is not made, endorsed or supported by the MeshCore project,
CoreScope, Chicagoland Mesh, Elecrow, Seeed Studio or Espressif. Their names are used only to say what MESHtop
works with. Thank you to all of them for building things in the open.
