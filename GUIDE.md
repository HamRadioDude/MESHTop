# MESHtop beginner's guide

Everything from unboxing to a working desk screen, one step at a time. If you get stuck, see
[Troubleshooting](#7-troubleshooting) at the end.

> **Affiliate links:** some of the shopping links in this guide are affiliate links. If you buy through them I
> may earn a small commission, at no extra cost to you. It helps me eat, and keeps MESHtop free. Thank you!

## 1. What you need

| Item | Notes | Link |
|---|---|---|
| Elecrow CrowPanel Advance 7" (ESP32-S3) | The screen. Hardware V1.3, V1.4 or V1.5 (on the board's label). Comes with acrylic spacers. | https://www.amazon.com/dp/B0FX5447WD?ref=ppx_yo2ov_dt_b_fed_asin_title&th=1|
| USB-C cable (data, not charge-only) | To install MESHtop from your computer, and to power the panel. |  |
| MeshCore companion radio (optional) | **The companion you already use with the phone app works.** Or build a dedicated desk radio: a Seeed XIAO nRF52840 with a Wio-SX1262 fits in the case's radio pod. Without a radio, MESHtop still shows your area's CoreScope map and channels. | https://link.amazon/B0awZLLCK |
| Antenna for your radio (915 MHz in the US) | Never power a radio without its antenna. | https://link.amazon/B0eXYL1GO |
| microSD card (optional) | For offline maps. Any size; FAT32 or exFAT. 16 GB is plenty. | I don't trust MicroSD cards from Amazon, so I would tell you to pick up one from Walmart or Best Buy. |
| 3D-printed case (optional) | Files Provided here and eventually on Printables. I use ASA, which is overkill.  PLA in areas that wont' go too hot, is fine. I like POLYMAKERS filament.  . | https://link.amazon/B0eXYL1GO |
| M3 hardware for the case | 4 × M3 × 25 mm countersunk (flat-head) screws, 4 × M3 hex nuts (plain, not nylon lock nuts), 2 × M3 × 10 mm pan or socket head, 2 × M3 × 8 mm pan or socket head. | Ace Hardware.|

You also need a computer with **Chrome or Edge** (for installing), and Wi-Fi at your desk (2.4 GHz).

## 2. Install MESHtop on the panel

1. Download the latest release from the GitHub **Releases** page:
   - a **new panel**: `meshtop-vX.Y.Z-FULL-at-0x0.bin`
   - **updating** a panel that already runs MESHtop: `meshtop-vX.Y.Z-UPDATE-at-0x10000.bin`
2. Plug the panel into your computer with the USB-C cable.
3. Open a web flasher in Chrome or Edge:
   [Adafruit WebSerial ESPTool](https://adafruit.github.io/Adafruit_WebSerial_ESPTool/) or
   [esptool-js](https://espressif.github.io/esptool-js/).
4. Click **Connect** and pick the panel's port (often "USB Serial" or "CH340").
5. Set the address and choose the file:

   | Installing | Address (offset) | File | Erase first? |
   |---|---|---|---|
   | New panel | `0x0` (type `0`) | FULL file | Yes, it's a good idea |
   | Update | `0x10000` (type `10000`) | UPDATE file | **No.** Erasing wipes your settings |

6. Click **Program** and wait for it to finish (a minute or two), then unplug and replug the panel.

**Get the address right.** Some flashers start at `0x1000`, which leaves the panel restarting over and over
("Invalid image block, can't boot"). If that happens, erase and flash the FULL file at `0x0`.

## 3. First start

1. **Wi-Fi.** The panel shows *Welcome to MESHtop* with a list of networks. Tap yours and type the password
   on the screen keyboard. Or, from your phone, scan the QR code (or join the **MESHtop-Setup** Wi-Fi) and pick
   your network on the page that opens.
2. The panel connects, sets its clock and starts loading the map. The first load of nodes can take a minute.
3. **Your area.** On the panel, go to **STATUS** and scan the settings QR code with your phone (or type the
   address shown into a browser on the same Wi-Fi). On the settings page:
   - **Area**: your city, zip code, or "lat, lon"
   - **Radius**: how many miles around it to watch
   - **CoreScope server**: the default is the Chicagoland Mesh server. If your area runs its own CoreScope,
     put its address here. Ask your local mesh group.
   - **Time zone**, **theme** (auto day/night), **dimming**, **alerts**
   - Press **Save**.

## 4. Connect your MeshCore radio (optional)

You can use **the MeshCore companion you already have** (the one paired with your phone app), or a **dedicated
radio** just for the desk, such as a Seeed XIAO nRF52840 + Wio-SX1262 in the case's radio pod (powered by its
own USB-C cable). The steps are the same either way. With your existing companion, the panel and the phone app
take turns: whichever connects first has it.

1. Your radio needs the **Companion Radio (BLE)** firmware. Install it from
   [flasher.meshcore.io](https://flasher.meshcore.io) if it doesn't have it.
2. **Disconnect the radio from the MeshCore phone app** (or turn off your phone's Bluetooth). A companion
   radio talks to one device at a time.
3. On the panel, open **MESH** and tap **Find my radio**. It connects to the nearest MeshCore radio using the
   radio's **Bluetooth PIN**: MeshCore's default is 123456. If you changed your radio's PIN, tap **Change PIN**
   and enter yours.
4. Once it says the radio is connected, the setup guide walks you through your name, region preset and saying
   hello to the mesh.
5. Your radio's battery shows on the top bar while it's connected.

If you have more than one MeshCore radio nearby, put part of your radio's Bluetooth name in **Radio name** on
the settings page so the panel picks the right one.

## 5. Offline maps (optional)

Offline maps make zooming instant and keep the map working without internet.

### Set the switches
The panel's microSD slot shares wiring with other features, chosen by the small switches on the back (toward the
middle of the board). The card needs **S1 = 1**:
- **1 1**: microphone and card
- **1 0**: card only
- **0 1** connects the UART port instead and **turns the card off**

The setting used on the developer's panel: first switch **1 1**, second switch **1 0**. If the panel says
"no card found ... switches", check these first.

### Make a map pack for your area
1. On your computer, install [Node.js](https://nodejs.org) (the LTS version).
2. Download the **map pack maker** (`tools/mappack` in this repository, also in the release).
3. Follow its [README](tools/mappack/README.md). The ready-made scripts make a Chicago pack; for anywhere
   else, give it your area's coordinates and a distance. It draws the maps on your computer from
   OpenStreetMap data (roughly 10 to 40 minutes).
4. Copy the **`maps`** folder it makes to the top level of the microSD card.
5. Put the card in the panel and restart it. **STATUS › Map card** should list your pack.

Even without a pack, map tiles the panel downloads while online are saved to the card, so places you've looked
at keep working offline.

## 6. A tour of the screen

- **Top bar**: alerts bell, connection (LIVE / CONNECTING / OFFLINE, or RADIO in radio-only mode), your area,
  your radio's battery, packets per minute, the time and UTC.
- **MAP**: live traffic as streaks from the sender, through the repeaters, to whoever heard it. Use + / − to
  zoom, tap a node for details (save it to My Nodes, or watch it). The **DAY / NIGHT** button switches
  the theme. The traffic list on the left folds down to one line.
- **FEED**: the same traffic as a list. Filter out adverts, channel messages and so on.
- **CHATTER**: your area's channels as heard by CoreScope's observers. Handy for catching messages your own
  radio missed.
- **MY NODES**: your saved nodes with packets today and how many observers heard them. Watched nodes give a
  pop-up when they go quiet or come back. **Show on map** takes you there.
- **MESH**: your own radio. Chats and channels with a keyboard; tap a message you sent to see which repeaters
  heard it and the path it took. People, repeaters (log in, neighbours, remote admin), find a route, trace route,
  "who can hear me?", radio health, and your radio's settings.
- **STATUS**: connection, packets per minute, nodes known, where the data comes from (CoreScope, your radio, or
  both), the map card, dimming, and the settings QR code.

Not every packet becomes a streak on the map: it needs at least part of its path to have a known location.
CHATTER and FEED show everything.

## 7. Troubleshooting

| Problem | What to try |
|---|---|
| The panel keeps restarting after install | Flashed at the wrong address. Erase, then flash the FULL file at `0x0`. |
| Settings gone after an update | The flasher erased first. For updates, flash the UPDATE file at `0x10000` without Erase. |
| "No card found" | Check the switches (section 5), and that the card is FAT32 or exFAT. |
| The radio won't connect | Disconnect it from the phone app, check the PIN, and restart the panel. Make sure the radio has Companion Radio **(BLE)** firmware. |
| CONNECTING for a long time | The panel can't reach the CoreScope server. Check the server address on the settings page and your internet. The map and your radio still work. |
| The map is blank or just a grid | Give it a moment, especially while it says CONNECTING. Check that your pack covers the area and zoom you're looking at. |
| Wi-Fi keeps dropping | MESHtop needs 2.4 GHz Wi-Fi. Restarting the router often clears it. |

### Getting a log (for bug reports)
1. Plug the panel into your computer by USB-C.
2. Open a serial monitor at **115200** baud (the web flashers above have one, or use the Arduino IDE).
3. Restart the panel and reproduce the problem, then copy the text.
4. **Before posting it, remove your Wi-Fi name, location and anything else private.**

Then open an issue on GitHub with the log, your MESHtop version (on STATUS) and what happened.
