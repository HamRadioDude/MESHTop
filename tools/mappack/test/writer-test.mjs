// The pack writer on a pack as big as the Chicago one (34,498 tiles, so the directory needs leaf
// directories), with repeated tiles (stored once, and runs of them). Read back here and, through the
// files it leaves in sim/tiletest/out, by the panel's own reader.   node test/writer-test.mjs <out dir>
import fs from "node:fs";
import path from "node:path";
import { Reader, fileReader, inMemory, tileId, writePmtiles } from "../pmtiles.mjs";

const out = process.argv[2];
let seed = 7;
const rnd = () => {  // mulberry32
  let t = (seed = (seed + 0x6d2b79f5) | 0);
  t = Math.imul(t ^ (t >>> 15), t | 1);
  t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
};
const water = Buffer.from("all water ".repeat(30));
const lon2x = (lon, z) => ((lon + 180) / 360) * 2 ** z;
const lat2y = (lat, z) => { const r = (lat * Math.PI) / 180; return ((1 - Math.log(Math.tan(r) + 1 / Math.cos(r)) / Math.PI) / 2) * 2 ** z; };
const [W, S, E, N] = [-89.38, 40.57, -85.88, 43.18];
const tiles = [], want = new Map();
for (let z = 6; z <= 14; z++)
  for (let x = Math.floor(lon2x(W, z)); x <= Math.floor(lon2x(E, z)); x++)
    for (let y = Math.floor(lat2y(N, z)); y <= Math.floor(lat2y(S, z)); y++) {
      const data = x > lon2x(-87.6, z) && rnd() < 0.8 ? water : Buffer.from(Array.from({ length: 400 + Math.floor(rnd() * 4000) }, () => Math.floor(rnd() * 256)));
      tiles.push(inMemory(tileId(z, x, y), data));
      want.set(`${z}/${x}/${y}`, data);
    }
const file = path.join(out, "writer-big.pmtiles");
const r = writePmtiles(file, tiles, { tileType: 2, minZoom: 6, maxZoom: 14, bounds: [W, S, E, N], metadata: { name: "writer-big" } });
const h = r.header;
console.log(`${tiles.length} tiles, ${h.tileEntries} entries, ${h.tileContents} stored, leaf directories ${h.leafLength} bytes, root ${h.rootLength} bytes`);
const rd = new Reader(fileReader(file));
await rd.open();
let ok = 0;
for (const [k, data] of want) {
  const [z, x, y] = k.split("/").map(Number);
  const got = await rd.get(z, x, y);
  if (got && got.equals(data)) ok++;
}
const all = await rd.allEntries();
console.log(`read back here: ${ok}/${want.size} byte for byte, ${all.length} tiles listed`);
if (ok !== want.size || all.length !== want.size || !(h.leafLength > 0) || !(h.tileContents < tiles.length)) process.exit(1);
// a sample for the panel's reader
const lines = [];
let i = 0;
fs.rmSync(path.join(out, "writer-big"), { recursive: true, force: true });
for (const [k, data] of want) {
  if (i++ % 50) continue;
  const f = `writer-big/${k}.bin`;
  fs.mkdirSync(path.dirname(path.join(out, f)), { recursive: true });
  fs.writeFileSync(path.join(out, f), data);
  const [z, x, y] = k.split("/").map(Number);
  lines.push(`${z} ${x} ${y} ${tileId(z, x, y)} out/${f}`);
  if (x + 300 < 2 ** z) lines.push(`${z} ${x + 300} ${y} ${tileId(z, x + 300, y)} -`);
}
fs.writeFileSync(path.join(out, "writer-big.expect"), lines.join("\n") + "\n");
console.log("WRITER TEST OK");
