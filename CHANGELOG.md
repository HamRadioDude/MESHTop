# Changelog

Updates keep your settings: flash the UPDATE file at 0x10000 without erasing. A new panel needs the FULL file at 0x0.

## 3.1.4
- **Chats saved on the SD card** (setting: "Keep my chats on the SD card", on by default). Messages you send and
  receive through your radio are kept in `/scopething/chats/`, one file per radio, and put back after a restart
  or update.
- **Messages page** on the settings web page (`/messages`): read the panel's saved chats from your phone's browser.
- **"Leave messages for my phone"** (setting, off by default): the panel no longer collects messages from your
  radio's inbox, so the MeshCore phone app still gets them. MESHtop shows channel messages from what your radio
  hears instead. Direct messages only reach the phone app in this mode, and messages sent from MESHtop never appear
  in the phone app (MeshCore has no way to share that).

## 3.1.3
- **The map no longer goes blank (just a grid) while the panel says CONNECTING.** Map tiles shared one worker
  with the CoreScope downloads, so while the panel was stuck reaching CoreScope, even tiles already on the SD card
  had to wait. The card now has its own worker: tiles on the card always load straight away, and only tiles the
  card doesn't have wait for the internet.
- With a CARTO key set, night mode now still uses your SD card map packs (it used to skip them).

## 3.1.2
- **Your radio's battery on the top bar**: a battery icon and percentage while your companion radio is connected
  (red below 20%). It's worked out from the battery voltage the radio reports every 2 minutes, so treat it as a
  guide; it reads high while charging.
- **Bluetooth link to the companion radio no longer backs up.** When the radio sent data faster than the panel
  took it, the Bluetooth stack ran out of receive buffers ("Failed to allocate buffer, retrying" in the log) and
  stalled. The panel now waits at most 5 ms for room (was 50 ms), holds 256 pending frames (was 64), empties the
  queue in ~10 ms slices instead of 16 frames at a time, and has more Bluetooth buffers (40 for incoming data, was 24; 32 general, was 12).
- Log: `[mem]` lines end with `radio frames lost` (should stay 0); the map tile log starts again after a
  day/night switch.
- README: install steps list more than one web flasher.

## 3.1.1
- Fixed a restart loop on a panel with no saved Wi-Fi (every full install): the setup hotspot was stopped for a
  radio check and only started again if a network had been saved.

## 3.1.0
- **Repeater neighbours**: the repeaters a repeater hears directly, with signal, as a list and on the map.
- **Remote admin**: send owner commands to your repeater and see the answers (after logging in).
- **Find a route**: a fresh route to anyone, there and back, shown on the map.
- **Stretched tiles**: where a map tile isn't available (offline, or past a map pack's top zoom), the next lower
  zoom is enlarged instead of a blank square.
- STATUS › Dimming: dim at night, always, or never.

## 3.0.7
- Map tiles drawn correctly again (3.0.6 showed static and coloured lines: PNG rows read at the wrong width, JPEG
  colours swapped). Tiles appear before they're saved to the card. The tile tests compare every pixel.

## 3.0.6
- **Offline maps on a microSD card** (FAT32 or exFAT), in the same places WadaMesh reads: map packs
  (`/maps/<name>/<name>.pmtiles`, PNG or JPEG), `/maps/osm`, and WadaMesh's `/tiles`. Downloaded tiles are saved
  in `/maps/osm`. STATUS › Map card shows free space, packs and tiles saved.
- New **map pack maker** for your computer (`tools/mappack`).
- Tap your own message that wasn't delivered: Send again, or Copy.

## 3.0.5
- **Data source choice**: CoreScope and your radio, CoreScope only, or your radio only. Radio only never contacts
  CoreScope (enforced at build time by `tools/privacy_check.py`).

## 3.0.4
- No screensaver: when left alone the panel goes back to the map and dims at night. The map's live traffic panel
  folds down to one line.

## 3.0.1 – 3.0.3
- Wi-Fi reconnects paced after a drop, with reasons in plain words.
- Trace route names the right repeater when two share a code, shows progress, and stays live while open.

## 3.0
- Renamed **MESHtop** (from the ScopeThing panel). Routes and traces drawn on the map, "Who can hear me?", radio
  health.

## 2.1.x (ScopeThing panel)
- 2.1.0 added the MESH tab (a MeshCore companion on the panel: chats, people, channels, my radio, setup guide).
- Later 2.1 releases: "heard by N repeaters", trace route, message routes, channel mute, Bluetooth PIN change,
  correct hop counts, and many display, memory and Wi-Fi stability fixes for the CrowPanel.
