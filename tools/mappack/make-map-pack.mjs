#!/usr/bin/env node
// MESHtop map pack maker: offline maps for the panel's memory card. WadaMesh reads the same packs.
//
// It draws the map tiles on your own computer from OpenStreetMap data, so no tile server gets
// scraped (OpenStreetMap's tile servers don't allow downloading areas for offline use):
//   1. downloads just your area's map data from the Protomaps daily build (OpenStreetMap, updated daily)
//   2. draws every tile, zoom by zoom, with MapLibre (light, dark, white, grayscale or black style)
//   3. writes   <out>/maps/<name>/<name>.pmtiles   one file per pack (the default), and/or
//               <out>/maps/osm/<z>/<x>/<y>.png       the folder layout, with --format folders or both
// Copy the "maps" folder to the root of the card.
//
//   npm install                      (once)
//   node make-map-pack.mjs           Chicago, 90 miles each way, zoom 6-14
//   node make-map-pack.mjs --help    everything else
//
// Map data © OpenStreetMap contributors (ODbL), drawn with the Protomaps basemap (BSD / CC0 styles).
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { createRequire } from "node:module";
import { Compression, Reader, TileType, coalesce, decompress, fileReader, hashOf, inMemory, tileId, writePmtiles } from "./pmtiles.mjs";

const VERSION = "1.0";
const UA = `MESHtop-map-pack/${VERSION}`;
const ASSETS = "https://protomaps.github.io/basemaps-assets";                         // fonts and icons
const ASSETS_MIRROR = "https://raw.githubusercontent.com/protomaps/basemaps-assets/main";  // the same files on GitHub
const STYLES = ["light", "dark", "white", "grayscale", "black"];
const MAX_PACK = 1.9e9;  // the panel reads the first 2 GB of a file, so bigger packs are split by zoom
const META = 4;          // tiles are drawn 4 x 4 at a time...
const BUF = 64;          // ...with 64 pixels more all round, so names near the edges come out whole

// ------------------------------------------------------------------ options
const HELP = `MESHtop map pack maker ${VERSION}: offline maps for the panel's memory card (WadaMesh reads them too)

  node make-map-pack.mjs [options]

Area (default: 90 miles each way around Chicago):
  --name <name>           pack name: letters, digits, - and _ (default chicago)
  --center <lat>,<lon>    middle of the area (default 41.8781,-87.6298)
  --radius <miles>        how far out to go each way (default 90)
  --radius-km <km>        the same in kilometres
  --bbox <W>,<S>,<E>,<N>  an exact box instead (degrees)
  --zoom <min>-<max>      zoom levels, 2-16 (default 6-14). Each level up has 4x the tiles.

Look and output:
  --style <name>          ${STYLES.join(", ")} (default light; the panel darkens light maps itself at night)
  --format <f>            pmtiles (one file, the default), folders (maps/osm/z/x/y.png), or both
  --out <folder>          where the "maps" folder goes (default ./card). Can be the card itself.
  --lang <code>           label language, e.g. en, es, de (default en)

Other:
  --dry-run               show the tile count, sizes and the download, then stop
  --source <url|file>     map data: a Protomaps v4 .pmtiles URL or file (default: newest daily build)
  --assets <url|folder>   fonts and icons (default: Protomaps' basemaps-assets, from GitHub)
  --jobs <n>              tiles drawn at once (default: half your CPU cores, 1-4)
  --work <folder>         downloads and drawn tiles, kept so a re-run picks up where it stopped
                          (default ./.map-work; delete it when you're done)
  --help                  this

Examples:
  node make-map-pack.mjs
  node make-map-pack.mjs --name milwaukee --center 43.0389,-87.9065 --radius 40 --zoom 6-15
  node make-map-pack.mjs --style dark --format both --out E:\\
`;

function die(msg) {
  console.error(`\nError: ${msg}`);
  process.exit(1);
}
function parseArgs(argv) {
  const o = {
    name: "chicago", center: [41.8781, -87.6298], radius: 90, bbox: null, zoom: [6, 14], style: "light", format: "pmtiles",
    out: "card", lang: "en", dryRun: false, source: null, assets: null, work: ".map-work",
    jobs: Math.max(1, Math.min(4, Math.floor((os.availableParallelism?.() ?? os.cpus().length) / 2))),
  };
  for (let i = 0; i < argv.length; i++) {
    let a = argv[i], v;
    const eq = a.indexOf("=");
    if (a.startsWith("--") && eq > 0) [a, v] = [a.slice(0, eq), a.slice(eq + 1)];
    const val = () => {
      if (v === undefined) v = argv[++i];
      if (v === undefined) die(`${a} needs a value`);
      return v;
    };
    const nums = (n) => {
      const xs = val().split(",").map(Number);
      if (xs.length !== n || xs.some((x) => !Number.isFinite(x))) die(`${a} needs ${n} numbers separated by commas`);
      return xs;
    };
    switch (a) {
      case "--name": o.name = val(); break;
      case "--center": o.center = nums(2); break;
      case "--radius": o.radius = Number(val()); break;
      case "--radius-km": o.radius = Number(val()) / 1.609344; break;
      case "--bbox": o.bbox = nums(4); break;
      case "--zoom": {
        const m = /^(\d+)(?:-(\d+))?$/.exec(val());
        if (!m) die("--zoom needs a range like 6-14");
        o.zoom = [Number(m[1]), Number(m[2] ?? m[1])];
        break;
      }
      case "--style": o.style = val(); break;
      case "--format": o.format = val(); break;
      case "--out": o.out = val(); break;
      case "--lang": o.lang = val(); break;
      case "--dry-run": o.dryRun = true; break;
      case "--source": o.source = val(); break;
      case "--assets": o.assets = val(); break;
      case "--jobs": o.jobs = Number(val()); break;
      case "--work": o.work = val(); break;
      case "-h": case "--help": console.log(HELP); process.exit(0);
      default: die(`unknown option ${a} (see --help)`);
    }
  }
  if (!/^[A-Za-z0-9_-]{1,40}$/.test(o.name)) die("--name: use letters, digits, - and _ (up to 40)");
  if (!(o.radius > 0 && o.radius <= 1000)) die("--radius: 1 to 1000 miles");
  if (!(o.zoom[0] >= 2 && o.zoom[1] <= 16 && o.zoom[0] <= o.zoom[1])) die("--zoom: between 2 and 16, low-high");
  if (!STYLES.includes(o.style)) die(`--style: one of ${STYLES.join(", ")}`);
  if (!["pmtiles", "folders", "both"].includes(o.format)) die("--format: pmtiles, folders or both");
  if (!(o.jobs >= 1 && o.jobs <= 16)) die("--jobs: 1 to 16");
  const [lat, lon] = o.center;
  if (Math.abs(lat) > 84 || Math.abs(lon) > 180) die("--center: latitude, longitude in degrees");
  if (o.bbox) {
    const [w, s, e, n] = o.bbox;
    if (!(w < e && s < n && w >= -180 && e <= 180 && s >= -85 && n <= 85)) die("--bbox: west,south,east,north in degrees");
  }
  return o;
}

// ------------------------------------------------------------------ tile maths (Web Mercator, OSM tile numbering)
const lon2x = (lon, z) => ((lon + 180) / 360) * 2 ** z;
const lat2y = (lat, z) => {
  const r = (lat * Math.PI) / 180;
  return ((1 - Math.log(Math.tan(r) + 1 / Math.cos(r)) / Math.PI) / 2) * 2 ** z;
};
const x2lon = (x, z) => (x / 2 ** z) * 360 - 180;
const y2lat = (y, z) => (180 / Math.PI) * Math.atan(Math.sinh(Math.PI - (2 * Math.PI * y) / 2 ** z));

function regionBox(o) {
  if (o.bbox) return o.bbox;
  const [lat, lon] = o.center;
  const dLat = o.radius / 69.0;
  const dLon = o.radius / (69.17 * Math.cos((lat * Math.PI) / 180));
  return [Math.max(-180, lon - dLon), Math.max(-85, lat - dLat), Math.min(180, lon + dLon), Math.min(85, lat + dLat)];
}
function tileRange(b, z) {
  const n = 2 ** z;
  const x0 = Math.max(0, Math.floor(lon2x(b[0], z))), x1 = Math.min(n - 1, Math.floor(lon2x(b[2], z) - 1e-9));
  const y0 = Math.max(0, Math.floor(lat2y(b[3], z))), y1 = Math.min(n - 1, Math.floor(lat2y(b[1], z) - 1e-9));
  return { z, x0, x1, y0, y1, count: (x1 - x0 + 1) * (y1 - y0 + 1) };
}
// the drawing jobs for one zoom: blocks of up to 4 x 4 tiles, lined up with the tile grid
function metatiles(r) {
  const m = Math.min(META, 2 ** r.z);
  const list = [];
  for (let my = Math.floor(r.y0 / m); my <= Math.floor(r.y1 / m); my++)
    for (let mx = Math.floor(r.x0 / m); mx <= Math.floor(r.x1 / m); mx++) {
      const tiles = [];
      for (let y = my * m; y < my * m + m; y++)
        for (let x = mx * m; x < mx * m + m; x++) if (x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1) tiles.push([x, y]);
      list.push({ z: r.z, mx, my, m, tiles });
    }
  return list;
}
// MapLibre draws vector tiles 512 pixels wide, so a 256-pixel tile at zoom z is drawn from zoom z-1 data
function vectorTilesFor(mt, vmax) {
  const vz = Math.min(mt.z - 1, vmax);
  const size = 512 * 2 ** (mt.z - 1 - vz);  // one vector tile, in zoom-z pixels
  const n = 2 ** vz;
  const left = mt.mx * mt.m * 256 - BUF, right = (mt.mx * mt.m + mt.m) * 256 + BUF;
  const top = mt.my * mt.m * 256 - BUF, bottom = (mt.my * mt.m + mt.m) * 256 + BUF;
  const out = [];
  for (let y = Math.max(0, Math.floor(top / size)); y <= Math.min(n - 1, Math.floor((bottom - 1) / size)); y++)
    for (let x = Math.floor(left / size); x <= Math.floor((right - 1) / size); x++) out.push([vz, ((x % n) + n) % n, y]);
  return out;
}

// ------------------------------------------------------------------ downloads
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function retry(what, fn, tries = 4) {
  for (let i = 1; ; i++) {
    try {
      return await fn();
    } catch (e) {
      if (i >= tries || e.fatal) throw new Error(`${what}: ${e.message}`);
      await sleep(1500 * i * i);
    }
  }
}
// byte ranges of a file on a web server (it must answer range requests; the planet file is 120 GB)
function httpReader(url) {
  return (offset, length) =>
    retry(`downloading ${url}`, async () => {
      const r = await fetch(url, { headers: { Range: `bytes=${offset}-${offset + length - 1}`, "User-Agent": UA } });
      if (r.status !== 206) {
        await r.body?.cancel();
        const e = new Error(r.status === 200 ? "the server sent the whole file instead of a part" : `the server answered ${r.status}`);
        e.fatal = r.status === 200 || r.status === 404 || r.status === 403;
        throw e;
      }
      return Buffer.from(await r.arrayBuffer());
    });
}
async function newestBuild() {
  const now = new Date();
  for (let i = 0; i < 14; i++) {
    const d = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() - i));
    const key = d.toISOString().slice(0, 10).replaceAll("-", "");
    const url = `https://build.protomaps.com/${key}.pmtiles`;
    try {
      const r = await fetch(url, { method: "HEAD", headers: { "User-Agent": UA } });
      if (r.ok) return { url, key };
    } catch (e) {
      if (i === 0) process.stdout.write(`  (${e.cause?.code || e.message}) `);
    }
  }
  die("couldn't reach the Protomaps daily builds (build.protomaps.com). Check the internet connection, or pick a\n" +
      "build at https://maps.protomaps.com/builds and pass its URL with --source.");
}
async function openSource(o) {
  let src;
  if (!o.source) {
    process.stdout.write("Finding the newest map data... ");
    const b = await newestBuild();
    src = { read: httpReader(b.url), label: `Protomaps daily build ${b.key} (OpenStreetMap)`, key: b.key, remote: true };
  } else if (/^https?:\/\//.test(o.source)) {
    src = { read: httpReader(o.source), label: o.source, key: path.basename(new URL(o.source).pathname, ".pmtiles"), remote: true };
  } else {
    if (!fs.existsSync(o.source)) die(`--source: no file ${o.source}`);
    src = { read: fileReader(o.source), label: o.source, key: path.basename(o.source, ".pmtiles"), remote: false };
  }
  src.reader = new Reader(src.read);
  const h = await src.reader.open();
  if (h.tileType !== TileType.mvt) die(`${src.label} isn't vector map data (tile type ${h.tileType})`);
  console.log(src.label);
  try {  // the styles here are for version 4 of the Protomaps basemap (the current one since 2024)
    const v = String((await src.reader.metadata()).version || "");
    if (v && !v.startsWith("4."))
      console.log(`Note: this map data is Protomaps basemap version ${v}; this tool's styles are for version 4. If the\n` +
                  "maps come out without roads or names, pick a version 4 build at https://maps.protomaps.com/builds (--source).");
  } catch {}
  return src;
}

// The vector tiles the drawing needs, fetched in as few requests as possible (neighbouring tiles in
// one go, like "pmtiles extract") and kept in the work folder for next time.
async function vectorData(o, src, wanted, dryRun) {
  const h = src.reader.header;
  const tiles = new Map();  // id -> bytes as stored (compressed as h.tileCompression says)
  const cache = path.join(o.work, `data-${o.name}-${src.key}.pmtiles`);
  if (src.remote && fs.existsSync(cache)) {  // everything downloaded for this pack before
    try {
      const c = new Reader(fileReader(cache));
      const ch = await c.open();
      const blob = await c.read(ch.dataOffset, ch.dataLength);
      for (const e of await c.allEntries()) tiles.set(e.id, blob.subarray(e.offset, e.offset + e.length));
      c.read.close();
    } catch (e) {
      console.log(`  (the saved map data couldn't be read, so it's downloaded again: ${e.message})`);
      tiles.clear();
    }
  }
  const missing = [...wanted.keys()].filter((id) => !tiles.has(id));
  if (!missing.length) {
    console.log(`Map data: all ${fmt(wanted.size)} pieces already downloaded`);
    return tiles;
  }
  let dirBytes = 0;
  const locs = await src.reader.locate(missing, { onProgress: (n) => (dirBytes += n) });
  const byOffset = new Map();
  for (const [id, loc] of locs) {
    let r = byOffset.get(loc.offset);
    if (!r) byOffset.set(loc.offset, (r = { start: loc.offset, end: loc.offset + loc.length, parts: [] }));
    r.parts.push({ id, off: loc.offset, len: loc.length });
  }
  const chunks = coalesce([...byOffset.values()], src.remote ? 512 * 1024 : 0, src.remote ? 32 << 20 : 4 << 20);
  const total = chunks.reduce((s, c) => s + c.end - c.start, 0);
  const have = wanted.size - missing.length;
  if (!chunks.length) {
    if (!have) die(`${src.label} has no map data for this area.`);
    console.log(`Map data: all ${fmt(have)} pieces already downloaded`);
    return tiles;
  }
  console.log(`Map data: ${fmt(locs.size)} pieces, ${mb(total + dirBytes)} to ${src.remote ? "download" : "read"}` +
              (src.remote ? ` in ${chunks.length} request${chunks.length === 1 ? "" : "s"}` : "") +
              (have ? ` (${fmt(have)} more already downloaded)` : ""));
  if (dryRun) return tiles;
  let done = 0, next = 0;
  const t0 = Date.now();
  const worker = async () => {
    while (next < chunks.length) {
      const c = chunks[next++];
      const b = await src.read(c.start, c.end - c.start);
      if (b.length !== c.end - c.start) throw new Error(`map data cut short (${b.length} of ${c.end - c.start} bytes)`);
      for (const p of c.parts) tiles.set(p.id, Buffer.from(b.subarray(p.off - c.start, p.off - c.start + p.len)));
      done += b.length;
      progress(`  downloaded ${mb(done)} of ${mb(total)}`, done / total, t0);
    }
  };
  await Promise.all(Array.from({ length: src.remote ? 4 : 1 }, worker));
  progressDone();
  if (src.remote) {  // keep it for next time (a different style or zoom, or a re-run after a stop)
    fs.mkdirSync(o.work, { recursive: true });
    writePmtiles(cache, [...tiles].map(([id, d]) => inMemory(id, d)), {
      tileType: TileType.mvt, tileCompression: h.tileCompression, minZoom: h.minZoom, maxZoom: h.maxZoom,
      bounds: [h.minLon, h.minLat, h.maxLon, h.maxLat], metadata: { source: src.label, note: "download cache of make-map-pack" },
    });
  }
  return tiles;
}

// fonts and icons for the labels: from --assets (a folder or URL), else Protomaps' GitHub pages,
// else the same files straight from GitHub. Kept in the work folder.
function assetLoader(o) {
  const dir = path.join(o.work, "assets");
  const local = o.assets && !/^https?:\/\//.test(o.assets) ? o.assets : null;
  const bases = o.assets && !local ? [o.assets.replace(/\/$/, "")] : [ASSETS, ASSETS_MIRROR];
  let good = null;
  const get = async (rel) => {
    rel = decodeURIComponent(rel);
    if (local) {
      const f = path.join(local, rel);
      return fs.existsSync(f) ? fs.readFileSync(f) : null;
    }
    const f = path.join(dir, rel);
    if (fs.existsSync(f)) return fs.readFileSync(f);
    let lastErr;
    for (const base of good ? [good] : bases) {
      try {
        const r = await fetch(`${base}/${rel.split("/").map(encodeURIComponent).join("/")}`, { headers: { "User-Agent": UA } });
        if (r.status === 404) {
          good = base;
          return null;  // e.g. a font range nobody has: fine, those characters just aren't drawn
        }
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        const b = Buffer.from(await r.arrayBuffer());
        good = base;
        fs.mkdirSync(path.dirname(f), { recursive: true });
        fs.writeFileSync(f, b);
        return b;
      } catch (e) {
        lastErr = e;
      }
    }
    throw new Error(`couldn't get ${rel} (${lastErr?.message})`);
  };
  return { get, prefix: `${ASSETS}/` };
}

// ------------------------------------------------------------------ drawing
let mbgl, sharp;
async function loadRenderer() {
  const major = Number(process.versions.node.split(".")[0]);
  if (![20, 22, 24].includes(major))
    console.log(`Note: this is Node.js ${process.versions.node}. The map drawing library ships for Node.js 20, 22 and 24;\n` +
                "if it won't load, install Node.js 24 LTS from https://nodejs.org and run npm install again.");
  const require = createRequire(import.meta.url);
  try {
    mbgl = require("@maplibre/maplibre-gl-native");
    sharp = require("sharp");
  } catch (e) {
    const linux = process.platform === "linux"
      ? "\nOn Linux it needs these first:  sudo apt install libopengl0 xvfb  (then run it with:  xvfb-run -a node make-map-pack.mjs)" : "";
    const win = process.platform === "win32" ? "\nOn Windows, installing the Microsoft Visual C++ Redistributable (x64) usually fixes this." : "";
    die(`the map drawing library didn't load: ${e.message.split("\n")[0]}\nRun "npm install" in this folder first.${linux}${win}`);
  }
  mbgl.on?.("message", (m) => {
    if (m.severity === "ERROR" && process.env.MAPPACK_DEBUG) console.error(`\n[maplibre] ${m.class}: ${m.text}`);
  });
}
async function styleFor(o, vmax) {
  const { layers, namedFlavor } = await import("@protomaps/basemaps");
  return {
    version: 8,
    glyphs: `${ASSETS}/fonts/{fontstack}/{range}.pbf`,
    sprite: `${ASSETS}/sprites/v4/${o.style}`,
    sources: { protomaps: { type: "vector", tiles: ["mvt://protomaps/{z}/{x}/{y}"], minzoom: 0, maxzoom: vmax } },
    layers: layers("protomaps", namedFlavor(o.style), { lang: o.lang }),
  };
}

async function draw(o, plan, src, tiles, workTiles) {
  const h = src.reader.header;
  const assets = assetLoader(o);
  const style = await styleFor(o, h.maxZoom);
  // first, check the fonts and icons can be had (it's a confusing failure halfway through otherwise)
  try {
    await assets.get(`sprites/v4/${o.style}.json`);
  } catch (e) {
    die(`${e.message}\nThe labels' fonts and icons come from GitHub. Check the internet connection, or download\n` +
        "https://github.com/protomaps/basemaps-assets (Code > Download ZIP), unzip it and pass the folder with --assets.");
  }
  let strays = 0;
  const vectorTile = async (z, x, y) => {
    const id = tileId(z, x, y);
    let d = tiles.get(id);
    if (!d) {  // outside the planned area (rare): fetch it on its own
      const loc = (await src.reader.locate([id])).get(id);
      if (!loc) return null;
      d = await src.read(loc.offset, loc.length);
      tiles.set(id, d);
      strays++;
    }
    return decompress(d, h.tileCompression);
  };
  const request = (req, cb) => {
    const url = req.url;
    let job;
    if (url.startsWith("mvt://")) {
      const [z, x, y] = url.slice("mvt://".length).split("/").slice(1).map(Number);
      job = vectorTile(z, x, y);
    } else if (url.startsWith(assets.prefix)) job = assets.get(url.slice(assets.prefix.length));
    else job = Promise.reject(new Error(`unexpected request ${url}`));
    job.then((data) => cb(null, data ? { data } : {}), (err) => cb(err));
  };

  const queue = plan.flatMap((r) => metatiles(r));
  const totalTiles = plan.reduce((s, r) => s + r.count, 0);
  const tilePath = (z, x, y) => path.join(workTiles, String(z), String(x), `${y}.png`);
  let doneTiles = 0, drawn = 0, next = 0;
  const t0 = Date.now();
  const worker = async () => {
    const map = new mbgl.Map({ request, ratio: 1, mode: "static" });  // "tile" mode would draw one vector tile only
    map.load(style);
    try {
      while (next < queue.length) {
        const mt = queue[next++];
        const todo = mt.tiles.filter(([x, y]) => !fs.existsSync(tilePath(mt.z, x, y)));
        if (todo.length) {
          const W = mt.m * 256 + 2 * BUF;
          const c = mt.m / 2;
          const params = { zoom: mt.z - 1, center: [x2lon(mt.mx * mt.m + c, mt.z), y2lat(mt.my * mt.m + c, mt.z)], width: W, height: W, bearing: 0, pitch: 0 };
          const raw = await new Promise((res, rej) => map.render(params, (err, buf) => (err ? rej(err) : res(buf))));
          await Promise.all(todo.map(async ([x, y]) => {
            const left = BUF + (x - mt.mx * mt.m) * 256, top = BUF + (y - mt.my * mt.m) * 256;
            const png = await sharp(raw, { raw: { width: W, height: W, channels: 4 } })
              .extract({ left, top, width: 256, height: 256 })
              .removeAlpha()
              .png({ palette: true, colours: 256, dither: 0, effort: 7, compressionLevel: 9 })
              .toBuffer();
            const f = tilePath(mt.z, x, y);
            fs.mkdirSync(path.dirname(f), { recursive: true });
            fs.writeFileSync(f + ".part", png);
            fs.renameSync(f + ".part", f);
          }));
          drawn += todo.length;
        }
        doneTiles += mt.tiles.length;
        progress(`  zoom ${mt.z}: ${fmt(doneTiles)} of ${fmt(totalTiles)} tiles`, doneTiles / totalTiles, t0);
      }
    } finally {
      map.release();
    }
  };
  await Promise.all(Array.from({ length: o.jobs }, worker));
  progressDone();
  if (strays && process.env.MAPPACK_DEBUG) console.log(`  (${strays} extra map data pieces fetched while drawing)`);
  return drawn;
}

// ------------------------------------------------------------------ output
function pack(o, plan, region, workTiles, buildLabel) {
  // every drawn tile in the area, zoom by zoom
  const byZoom = plan.map((r) => {
    const list = [];
    let bytes = 0;
    for (let x = r.x0; x <= r.x1; x++)
      for (let y = r.y0; y <= r.y1; y++) {
        const f = path.join(workTiles, String(r.z), String(x), `${y}.png`);
        if (!fs.existsSync(f)) continue;
        const data = fs.readFileSync(f);
        list.push({ id: tileId(r.z, x, y), z: r.z, x, y, file: f, size: data.length, hash: hashOf(data), read: () => fs.readFileSync(f) });
        bytes += data.length;
      }
    return { z: r.z, list, bytes };
  });
  const results = [];
  if (o.format !== "folders") {
    // one pack, or several split by zoom so none passes 2 GB
    const groups = [];
    for (const zl of byZoom) {
      if (zl.bytes > MAX_PACK) die(`zoom ${zl.z} alone is ${mb(zl.bytes)}; the panel reads packs up to 2 GB. Make the area smaller or leave out zoom ${zl.z}.`);
      const g = groups[groups.length - 1];
      if (g && g.bytes + zl.bytes <= MAX_PACK) {
        g.levels.push(zl);
        g.bytes += zl.bytes;
      } else groups.push({ levels: [zl], bytes: zl.bytes });
    }
    groups.forEach((g, i) => {
      const z0 = g.levels[0].z, z1 = g.levels[g.levels.length - 1].z;
      const name = groups.length === 1 || i === 0 ? o.name : `${o.name}-z${z0}${z1 > z0 ? `-${z1}` : ""}`;
      const dir = path.join(o.out, "maps", name);
      fs.mkdirSync(dir, { recursive: true });
      const file = path.join(dir, `${name}.pmtiles`);
      const tiles = g.levels.flatMap((l) => l.list);
      const [lat, lon] = o.bbox ? [(region[1] + region[3]) / 2, (region[0] + region[2]) / 2] : o.center;
      const r = writePmtiles(file, tiles, {
        tileType: TileType.png, tileCompression: Compression.none, minZoom: z0, maxZoom: z1, bounds: region,
        center: [lon, lat], centerZoom: Math.min(Math.max(z0, 10), z1),
        metadata: {
          name, format: "png", type: "baselayer", version: "1",
          description: `${o.name}: OpenStreetMap map, zoom ${z0}-${z1}, ${o.style} style, for MESHtop / WadaMesh`,
          attribution: "© OpenStreetMap contributors, © Protomaps",
          minzoom: z0, maxzoom: z1, bounds: region.map((v) => +v.toFixed(5)).join(","),
          generator: `MESHtop make-map-pack ${VERSION}`, source: buildLabel, style: o.style,
        },
      });
      results.push({ file, bytes: r.bytes, tiles: tiles.length, z0, z1 });
    });
  }
  if (o.format !== "pmtiles") {
    let n = 0, bytes = 0;
    for (const zl of byZoom)
      for (const t of zl.list) {
        const f = path.join(o.out, "maps", "osm", String(t.z), String(t.x), `${t.y}.png`);
        if (!(fs.existsSync(f) && fs.statSync(f).size === t.size)) {
          fs.mkdirSync(path.dirname(f), { recursive: true });
          fs.copyFileSync(t.file, f);
        }
        n++;
        bytes += t.size;
      }
    results.push({ file: path.join(o.out, "maps", "osm"), bytes, tiles: n, folders: true });
  }
  return results;
}

// ------------------------------------------------------------------ progress and numbers
const fmt = (n) => n.toLocaleString("en-US");
const mb = (b) => (b >= 1e9 ? `${(b / 1e9).toFixed(2)} GB` : `${Math.max(0.1, b / 1e6).toFixed(1)} MB`);
let lastLine = 0;
function progress(text, frac, t0) {
  const now = Date.now();
  if (now - lastLine < 500 && frac < 1) return;
  lastLine = now;
  const el = (now - t0) / 1000;
  const eta = frac > 0.02 && frac < 1 ? `, about ${dur((el / frac) * (1 - frac))} to go` : "";
  const line = `${text} (${Math.floor(frac * 100)}%${eta})`;
  if (process.stdout.isTTY) process.stdout.write(`\r${line.padEnd(78)}`);
  else if (frac >= 1 || now - (progress.lastPrint || 0) > 15000) {
    progress.lastPrint = now;
    console.log(line);
  }
}
function progressDone() {
  if (process.stdout.isTTY) process.stdout.write("\n");
  lastLine = 0;
}
const dur = (s) => (s < 90 ? `${Math.ceil(s)} s` : s < 5400 ? `${Math.round(s / 60)} min` : `${(s / 3600).toFixed(1)} h`);

// ------------------------------------------------------------------ main
async function main() {
  const o = parseArgs(process.argv.slice(2));
  const region = regionBox(o);
  const plan = [];
  for (let z = o.zoom[0]; z <= o.zoom[1]; z++) plan.push(tileRange(region, z));
  const total = plan.reduce((s, r) => s + r.count, 0);
  const where = o.bbox ? `the box ${region.map((v) => v.toFixed(4)).join(", ")}`
                       : `${+o.radius.toFixed(1)} miles each way around ${o.center[0]}, ${o.center[1]}`;
  console.log(`MESHtop map pack maker ${VERSION}\n\n"${o.name}": ${where}`);
  console.log(`Zoom ${o.zoom[0]}-${o.zoom[1]}: ${fmt(total)} tiles (${plan.map((r) => `z${r.z} ${fmt(r.count)}`).join(", ")})`);
  console.log(`Style ${o.style}, ${o.format === "both" ? "a pack and folders" : o.format === "folders" ? "folders" : "one pack file"}, ` +
              `about ${mb(total * 11000)}-${mb(total * 22000)} on the card`);
  if (total > 400000) die(`${fmt(total)} tiles is too many to draw sensibly. Make the area smaller or the top zoom lower.`);

  if (!o.dryRun) await loadRenderer();  // a missing "npm install" shows up now, not after the download
  const src = await openSource(o);
  const vmax = src.reader.header.maxZoom;
  const wanted = new Map();
  for (const r of plan)
    for (const mt of metatiles(r))
      for (const [z, x, y] of vectorTilesFor(mt, vmax)) wanted.set(tileId(z, x, y), [z, x, y]);
  const tiles = await vectorData(o, src, wanted, o.dryRun);
  if (o.dryRun) {
    console.log("\nDry run: nothing downloaded or drawn.");
    return;
  }

  const work = path.join(o.work, `${o.name}-${o.style}-${o.lang}-${src.key}`);
  console.log(`Drawing ${fmt(total)} tiles, ${o.jobs} at a time (a re-run carries on where this stops)`);
  const t0 = Date.now();
  const drawn = await draw(o, plan, src, tiles, path.join(work, "tiles"));
  console.log(`  drew ${fmt(drawn)} tiles in ${dur((Date.now() - t0) / 1000)}`);

  const results = pack(o, plan, region, path.join(work, "tiles"), src.label);
  console.log("\nDone:");
  for (const r of results)
    console.log(`  ${r.file}${r.folders ? path.sep : ""}  ${mb(r.bytes)}, ${fmt(r.tiles)} tiles${r.folders ? "" : `, zoom ${r.z0}-${r.z1}`}`);
  const card = path.resolve(o.out);
  console.log(`\nCopy the "maps" folder in ${card} to the top level of the memory card (FAT32 or exFAT).\n` +
              "On the panel, the switches on the back go to TF card (both 1); restart it and STATUS shows the pack\n" +
              "under Map card. If the card already has a \"maps\" folder, merge them.\n" +
              `Map data © OpenStreetMap contributors (openstreetmap.org/copyright), © Protomaps.\n` +
              `The work folder ${path.resolve(o.work)} can be deleted (it's kept so a re-run is quick).`);
}

main().catch((e) => {
  progressDone();
  die(e.stack && process.env.MAPPACK_DEBUG ? e.stack : e.message);
});
