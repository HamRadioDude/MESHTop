# MESHTop


THE Ultimate Open Source MESHcore Desktop Project
MESHtop

A desk screen for your MeshCore mesh. MESHtop turns the Elecrow CrowPanel Advance 7" (ESP32-S3, 800x480 touch) into a live map and messenger for MeshCore. It pairs with your own MeshCore companion radio over Bluetooth, so you can read and send messages without picking up your phone, and it pulls in live traffic from a CoreScope server, so you see what your radio hears and what the whole network hears, on one screen. No computer needed once it's set up.

Free and open source (MIT). By DudeTested (HamRadioDude).

Features
Live map: every packet drawn as a streak from the sender, through the repeaters, to whoever heard it. Tap a repeater to see who it is, save it, or watch it. Day and night modes.
Offline maps from a microSD card, so the map works without internet and zooms instantly (see Offline maps).
FEED: all traffic as a list, with filters (adverts, channel, DM, trace, path, control).
CHATTER: your area's channels from CoreScope, so you can catch messages your own radio missed.
MY NODES: your saved nodes at a glance (packets today, observers that heard them). Watch a node and get a pop-up when it goes quiet or comes back.
MESH: a MeshCore companion on the panel, connected to your radio over Bluetooth:
chats and channels with a full keyboard, delivery ticks, "heard by N repeaters" with signal and path
people and repeaters, login and status, repeater neighbours and remote admin for repeaters you run
trace route, find a route, "who can hear me?", radio health
join hashtag and private channels, region presets, transmit power, Bluetooth PIN
STATUS: connection, packets per minute, nodes known, data source, map card, dimming.
Data source: CoreScope and your radio, CoreScope only, or your radio only.
Settings page in your browser (scan the QR code on STATUS): CoreScope server, your area and radius, night map style, alerts and more.
Privacy

Nothing from your own radio is ever sent to CoreScope or anywhere else on the network: MESHtop only reads from CoreScope. In radio-only mode CoreScope is never contacted at all. This is checked on every build by tools/privacy_check.py.

What you need
Elecrow CrowPanel Advance 7" (ESP32-S3), hardware V1.3, V1.4 or V1.5 (on the board's label).
A MeshCore companion radio with the Companion Radio (BLE) firmware from flasher.meshcore.io, for example a Seeed XIAO nRF52840 with a Wio-SX1262. Optional: without one, MESHtop still shows CoreScope's map and channels.
Optional microSD card (FAT32 or exFAT) for offline maps.
Optional case: 3D-printable files are published separately (see the release notes for the link).
Install

Use Chrome or Edge with the panel plugged in by USB-C, and a web flasher that lets you set the address, such as esptool-js or Adafruit WebSerial ESPTool. On Windows, Espressif's Flash Download Tool works too. Download the files from the GitHub release:

New panel: meshtop-vX.Y.Z-FULL-at-0x0.bin at 0x0. Erasing first is a good idea. This wipes saved settings.
Updating: meshtop-vX.Y.Z-UPDATE-at-0x10000.bin at 0x10000. Don't press Erase. Keeps Wi-Fi, area, PIN and all settings.

The address matters: some flashers default to 0x1000, which leaves the panel in a boot loop ("Invalid image block, can't boot"). To recover, erase and flash the FULL file at 0x0.

First start
The panel shows a Wi-Fi setup screen. Pick your network on the screen and type the password, or join the panel's setup hotspot from your phone.
Set your area (ZIP or postcode) and how far around it to watch. If you're not near Chicago, set your own area's CoreScope server on the settings page (the default is the Chicagoland Mesh server).
To use your radio: disconnect it from the phone app first (a companion talks to one device at a time), then go to MESH and enter the radio's Bluetooth PIN (default 123456).
Offline maps
Set the switches on the back of the panel so the SD card slot is connected: the first switch to 1 1, the second to 1 0. Without this the panel reports "no card found".
Make a map pack for your area on your computer with the map pack maker. It draws the map tiles on your computer from OpenStreetMap data.
Copy its folder into /maps/ on the card, so you have /maps/<name>/<name>.pmtiles, and put the card in. STATUS › Map card shows the packs found.

The panel also reads /maps/osm/z/x/y.png and WadaMesh's /tiles/z/x/y.jpg, so one card works in both. Tiles the panel downloads while online are saved in /maps/osm, so places you've looked at keep working offline.

Build

PlatformIO with the pioarduino platform (pinned in platformio.ini):

pio run -e crowpanel7                  # build
pio run -e crowpanel7 -t upload        # build and flash over USB

The first build takes a while (10-20 minutes): custom_sdkconfig makes pioarduino recompile the ESP-IDF libraries with this project's settings, and tools/fatfs_exfat.py switches on exFAT in FatFs first. Later builds are quick. tools/privacy_check.py runs before every build and fails it if radio code could reach the network.

Outputs in .pio/build/crowpanel7/: firmware.bin (the UPDATE file, at 0x10000) and firmware.factory.bin (the FULL install, at 0x0).

Tests:

Simulator (Linux, demo mode, fake MeshCore radio; uses the LVGL that pio run or pio pkg install -e crowpanel7 downloads): make -C sim -j8 && ./sim/build/scopething-sim, run from the project folder. It runs the self-checks and writes screenshots to sim/out (python3 tools/frames_to_png.py sim/out turns them into PNGs).
Map pack reader and decoders: make -C sim tiles-test (needs Python with pmtiles and Pillow).
Map pack maker, end to end: tools/mappack/test/run-test.sh.

TLS uses the Mozilla CA bundle in data/x509_crt_bundle (regenerate with python3 tools/gen_crt_bundle.py -i cacert.pem using a fresh certifi cacert.pem, then copy x509_crt_bundle into data/).

Settings live in the scopething NVS namespace and the /scopething SD folder (names from before the MESHtop rename). Keep them, so updates keep everyone's settings.

Layout
src/core - model: packet paths, hop resolution, feed, channels, alerts, My Nodes, map tiles; companion.* is the MeshCore companion protocol (portable C++)
src/ui - LVGL 9.1 UI (map canvas + streak overlay, screens, overlays)
src/device - board (display, touch, backlight, buzzer), network task, Bluetooth radio link, settings storage, settings web page
src/app - platform interface and demo mode
sim - desktop simulator (with a fake MeshCore radio speaking the real frame format)
tools - build checks (privacy, exFAT), CA bundle generator, screenshot converter
tools/mappack - map pack maker for the memory card (Node.js)
Map data

Map tiles © OpenStreetMap contributors. Online tiles come from the OpenStreetMap tile servers, only for the area on screen, and are kept on the card. Please respect the OSM tile usage policy: for offline maps of a whole area, use the map pack maker rather than downloading tiles in bulk.

Credits and licence

MESHtop is MIT-licensed (see LICENSE). It builds on a lot of other people's work: MeshCore, CoreScope, LVGL, NimBLE and more, all listed with their licences in CREDITS.md. It's an independent project, not affiliated with MeshCore, CoreScope, Elecrow or Seeed Studio.

See CHANGELOG.md for what's new, and CONTRIBUTING.md to help.

Enjoying it? https://buymeacoffee.com/hamradiodude
