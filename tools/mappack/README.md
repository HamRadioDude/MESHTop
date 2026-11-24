# MESHtop map pack maker

Makes offline maps for the panel's memory card, so the map works with no internet. WadaMesh reads the
same packs, so one card works in both.

It draws the maps **on your own computer** from OpenStreetMap data. It doesn't download map pictures
from the OpenStreetMap tile servers, which [don't allow offline use](https://operations.osmfoundation.org/policies/tiles/).

1. It downloads just your area's map data from the [Protomaps](https://protomaps.com) daily build
   (OpenStreetMap data, updated every day). It fetches only the pieces it needs, the way `pmtiles extract` does.
2. It draws every map tile at every zoom level with [MapLibre](https://maplibre.org), in a light, dark,
   white, grayscale or black style.
3. It writes them where the panel looks:
   - `maps/<name>/<name>.pmtiles`: one file per pack (the default, and the quickest to copy)
   - `maps/osm/<z>/<x>/<y>.png`: the folder layout Meshtastic and MeshCore devices use (`--format folders` or `both`)

The default is **Chicago, 90 miles each way, zoom 6 to 14**: 34,498 tiles, about 0.4 to 0.8 GB on the card.

## What you need

- A computer running Windows 10/11, macOS or Linux (Ubuntu 24.04)
- **Node.js 24 LTS** from [nodejs.org](https://nodejs.org) (22 and 20 work too; newer "Current" versions may not)
- An internet connection. The map data download for the default area is a few hundred MB.
- A microSD card formatted **FAT32 or exFAT** (most cards over 32 GB come as exFAT; both work)

## Make the Chicago pack

Unzip the folder somewhere with a few GB free (not inside the zip, and not on the memory card), then:

**Windows**: double-click `make-chicago-pack-windows.bat`.
**Mac**: double-click `make-chicago-pack-mac.command` (the first time, right-click it and choose Open).
**Linux**: `sudo apt install libopengl0 xvfb`, then `./make-map-pack.sh`.

The first run installs what it needs (about 100 MB, once). Then it shows the plan, downloads the map data
and draws the tiles, with a progress line and a time estimate. Drawing takes roughly 10 to 40 minutes,
depending on the computer. If you stop it, run it again: it carries on where it stopped.

When it's done, the pack is in the `card` folder next to the script:

```
card/
  maps/
    chicago/
      chicago.pmtiles
```

## Put it on the card

1. Copy the **`maps`** folder from `card` to the top level of the memory card. If the card already has a
   `maps` folder, merge them.
2. Put the card in the panel. On V1.3 boards and later, set the switches on the back so the card is connected
   (S1 = 1: "1 1" or "1 0"; "0 1" turns the card off). See the [beginner's guide](../../GUIDE.md#set-the-switches).
3. Restart the panel. **STATUS > Map card** shows the free space and `map pack chicago (zoom 6-14)`.

The panel looks for map tiles in this order: map packs, `maps/osm` folders, WadaMesh's `tiles` folder,
then the panel's older cache, and only then the internet. Maps it does download are saved in `maps/osm`
on the card, so places you've looked at keep working offline too.

At night the panel darkens light maps itself. A pack made with `--style dark` or `--style black` is used
as it is.

## Other areas and options

Run it from a terminal in this folder (after the first run has installed everything, or after `npm install`):

```
node make-map-pack.mjs --help
node make-map-pack.mjs --name milwaukee --center 43.0389,-87.9065 --radius 40
node make-map-pack.mjs --name chicago-dark --style dark
node make-map-pack.mjs --zoom 6-15 --format both --out E:\
node make-map-pack.mjs --dry-run          (shows the tile count and download size, then stops)
```

| Option | What it does |
|---|---|
| `--name` | Pack name (letters, digits, `-`, `_`). Default `chicago`. |
| `--center lat,lon`, `--radius miles` | The area: a box this many miles out from the middle each way. `--radius-km` for kilometres. |
| `--bbox W,S,E,N` | An exact box instead, in degrees. |
| `--zoom min-max` | Zoom levels, 2 to 16. Default 6-14. Each level up has 4 times the tiles of the one below. |
| `--style` | `light` (default), `dark`, `white`, `grayscale`, `black`. |
| `--format` | `pmtiles` (default), `folders` or `both`. |
| `--out` | Where the `maps` folder goes. Default `card`. Can be the memory card itself. |
| `--lang` | Label language, e.g. `en`, `es`, `de`. Default `en`. |
| `--source` | The map data: a Protomaps version 4 `.pmtiles` URL or file. Default: the newest daily build. |
| `--jobs` | How many tiles are drawn at once. Default half the CPU cores (1 to 4). |
| `--work` | Downloads and drawn tiles are kept here so a re-run is quick. Default `.map-work`; delete it when done. |

The panel reads up to 2 GB from each pack file. A bigger pack is split by zoom level into several packs
(`chicago`, `chicago-z15` and so on); the panel uses up to 3 packs at once.

## If something goes wrong

- **"the map drawing library didn't load"**: run `npm install` in this folder. On Windows, installing the
  Microsoft Visual C++ Redistributable (x64) usually fixes it. On Linux, install `libopengl0` and `xvfb` and run
  it with `xvfb-run -a node make-map-pack.mjs`. Use Node.js 24, 22 or 20.
- **"couldn't reach the Protomaps daily builds"**: check the internet connection. Or pick a build at
  [maps.protomaps.com/builds](https://maps.protomaps.com/builds) and pass its URL with `--source`.
- **Labels are missing**: the fonts and icons come from GitHub. If GitHub can't be reached, download
  [basemaps-assets](https://github.com/protomaps/basemaps-assets/archive/refs/heads/main.zip), unzip it and pass the
  folder with `--assets`.
- **The panel shows "No card found"**: the card must be FAT32 or exFAT, and the switches on the back set to TF card.

## Credits and licences

- Map data © [OpenStreetMap contributors](https://www.openstreetmap.org/copyright), ODbL. Packs made with this tool
  are Produced Works of OpenStreetMap data; the panel shows "Map (c) OpenStreetMap" on the map.
- [Protomaps basemap](https://github.com/protomaps/basemaps): tiles and styles (code BSD-3-Clause, visual design CC0,
  icons MIT); fonts Noto Sans (SIL Open Font License).
- [MapLibre Native](https://github.com/maplibre/maplibre-native) (BSD-2-Clause) draws the tiles;
  [sharp](https://sharp.pixelplumbing.com) (Apache-2.0) makes the PNG files.
- The PMTiles reader and writer here (`pmtiles.mjs`) follow the
  [PMTiles v3 spec](https://github.com/protomaps/PMTiles/blob/main/spec/v3/spec.md).

Part of MESHtop by DudeTested (HamRadioDude). MIT License.

## Testing (for developers)

In the MESHtop project, `tools/mappack/test/run-test.sh` makes stand-in map data, serves it the way build.protomaps.com does, makes light and dark packs
from it, and checks them with the panel's own PMTiles reader and decoders (`make -C sim tiles-test`).
Needs Python with `pmtiles`, `mapbox-vector-tile` and `shapely`.
