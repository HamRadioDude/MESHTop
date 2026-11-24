// PMTiles v3: the one-file tile archive MESHtop and WadaMesh read from the memory card.
// Spec: https://github.com/protomaps/PMTiles/blob/main/spec/v3/spec.md
// Reading (header, directories, tile lookup) and writing (deduplicated, run-length coded, gzipped
// directories, leaf directories when the root would pass 16 KB). No dependencies.
import crypto from "node:crypto";
import fs from "node:fs";
import zlib from "node:zlib";

export const Compression = { unknown: 0, none: 1, gzip: 2, brotli: 3, zstd: 4 };
export const TileType = { unknown: 0, mvt: 1, png: 2, jpeg: 3, webp: 4, avif: 5 };
export const HEADER_BYTES = 127;
const ROOT_MAX = 16384 - HEADER_BYTES;  // header + root directory fit in the first 16 KB

// ---- tile ids: position along a Hilbert curve, zoom by zoom (plain Numbers: exact to zoom 26)
export function tileId(z, x, y) {
  let acc = (4 ** z - 1) / 3;
  let d = 0;
  for (let s = 2 ** (z - 1); s >= 1; s /= 2) {
    const rx = x & s ? 1 : 0;  // s <= 2^25 here, so bit tests are safe
    const ry = y & s ? 1 : 0;
    d += s * s * ((3 * rx) ^ ry);
    if (ry === 0) {
      if (rx === 1) {
        x = s - 1 - x;
        y = s - 1 - y;
      }
      [x, y] = [y, x];
    }
  }
  return acc + d;
}

// ---- varints (values can pass 2^32, so no bit operators above 31 bits)
function readVarint(buf, p) {
  let v = 0, mul = 1, b;
  do {
    if (p.i >= buf.length) throw new Error("PMTiles directory is cut short");
    b = buf[p.i++];
    v += (b & 0x7f) * mul;
    mul *= 128;
  } while (b & 0x80);
  return v;
}
function writeVarint(out, v) {
  while (v >= 128) {
    out.push((v % 128) | 0x80);
    v = Math.floor(v / 128);
  }
  out.push(v);
}

// ---- directories: [{id, offset, length, run}]; run 0 = points at a leaf directory
export function deserializeDir(buf) {
  const p = { i: 0 };
  const n = readVarint(buf, p);
  const e = Array.from({ length: n }, () => ({ id: 0, offset: 0, length: 0, run: 0 }));
  let last = 0;
  for (const x of e) x.id = last += readVarint(buf, p);
  for (const x of e) x.run = readVarint(buf, p);
  for (const x of e) x.length = readVarint(buf, p);
  e.forEach((x, i) => {
    const v = readVarint(buf, p);
    x.offset = v === 0 && i > 0 ? e[i - 1].offset + e[i - 1].length : v - 1;
  });
  return e;
}
export function serializeDir(entries) {
  const out = [];
  writeVarint(out, entries.length);
  let last = 0;
  for (const x of entries) {
    writeVarint(out, x.id - last);
    last = x.id;
  }
  for (const x of entries) writeVarint(out, x.run);
  for (const x of entries) writeVarint(out, x.length);
  entries.forEach((x, i) => {
    const prev = entries[i - 1];
    writeVarint(out, i > 0 && x.offset === prev.offset + prev.length ? 0 : x.offset + 1);
  });
  return Buffer.from(out);
}
// the entry holding tile `id`: a tile entry (run > 0) or the leaf directory to look in (run 0)
export function findEntry(entries, id) {
  let lo = 0, hi = entries.length - 1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (entries[mid].id < id) lo = mid + 1;
    else if (entries[mid].id > id) hi = mid - 1;
    else return entries[mid];
  }
  if (hi >= 0) {
    const e = entries[hi];
    if (e.run === 0) return e;
    if (id - e.id < e.run) return e;
  }
  return null;
}

export function decompress(buf, how) {
  if (how === Compression.none || how === Compression.unknown) return buf;
  if (how === Compression.gzip) return zlib.gunzipSync(buf);
  if (how === Compression.brotli) return zlib.brotliDecompressSync(buf);
  throw new Error(`PMTiles compression ${how} isn't supported`);
}

// ---- header
const u64 = (b, o) => Number(b.readBigUInt64LE(o));
export function parseHeader(b) {
  if (b.length < HEADER_BYTES || b.toString("latin1", 0, 7) !== "PMTiles") throw new Error("not a PMTiles file");
  if (b[7] !== 3) throw new Error(`PMTiles version ${b[7]}; only version 3 is supported`);
  return {
    rootOffset: u64(b, 8), rootLength: u64(b, 16),
    metadataOffset: u64(b, 24), metadataLength: u64(b, 32),
    leafOffset: u64(b, 40), leafLength: u64(b, 48),
    dataOffset: u64(b, 56), dataLength: u64(b, 64),
    addressedTiles: u64(b, 72), tileEntries: u64(b, 80), tileContents: u64(b, 88),
    clustered: b[96] === 1, internalCompression: b[97], tileCompression: b[98], tileType: b[99],
    minZoom: b[100], maxZoom: b[101],
    minLon: b.readInt32LE(102) / 1e7, minLat: b.readInt32LE(106) / 1e7,
    maxLon: b.readInt32LE(110) / 1e7, maxLat: b.readInt32LE(114) / 1e7,
    centerZoom: b[118], centerLon: b.readInt32LE(119) / 1e7, centerLat: b.readInt32LE(123) / 1e7,
  };
}
function headerBytes(h) {
  const b = Buffer.alloc(HEADER_BYTES);
  b.write("PMTiles", 0, "latin1");
  b[7] = 3;
  const w = (o, v) => b.writeBigUInt64LE(BigInt(v), o);
  w(8, h.rootOffset); w(16, h.rootLength); w(24, h.metadataOffset); w(32, h.metadataLength);
  w(40, h.leafOffset); w(48, h.leafLength); w(56, h.dataOffset); w(64, h.dataLength);
  w(72, h.addressedTiles); w(80, h.tileEntries); w(88, h.tileContents);
  b[96] = h.clustered ? 1 : 0;
  b[97] = h.internalCompression;
  b[98] = h.tileCompression;
  b[99] = h.tileType;
  b[100] = h.minZoom;
  b[101] = h.maxZoom;
  const e7 = (o, v) => b.writeInt32LE(Math.round(v * 1e7), o);
  e7(102, h.minLon); e7(106, h.minLat); e7(110, h.maxLon); e7(114, h.maxLat);
  b[118] = h.centerZoom;
  e7(119, h.centerLon); e7(123, h.centerLat);
  return b;
}

// ---- reading: `read(offset, length)` resolves to a Buffer (a file, or an HTTP range request)
export class Reader {
  constructor(read) {
    this.read = read;
    this.leaves = new Map();  // leaf directory offset -> entries
  }
  async open() {
    const first = await this.read(0, 16384);
    this.header = parseHeader(first);
    const h = this.header;
    const rootRaw = h.rootOffset + h.rootLength <= first.length ? first.subarray(h.rootOffset, h.rootOffset + h.rootLength)
                                                                 : await this.read(h.rootOffset, h.rootLength);
    this.root = deserializeDir(decompress(rootRaw, h.internalCompression));
    return h;
  }
  async metadata() {
    const h = this.header;
    if (!h.metadataLength) return {};
    return JSON.parse(decompress(await this.read(h.metadataOffset, h.metadataLength), h.internalCompression).toString("utf8"));
  }
  // Where each tile's bytes are, for many tiles at once: leaf directories are fetched as needed
  // (neighbouring ones in one request). Returns Map id -> {offset, length} (absolute file offsets);
  // tiles not in the archive are left out.
  async locate(ids, { gap = 256 * 1024, onProgress } = {}) {
    ids = [...ids].sort((a, b) => a - b);
    const h = this.header;
    const out = new Map();
    for (let depth = 0; depth < 4 && ids.length; depth++) {
      const needLeaves = new Map();  // leaf offset -> length
      const rest = [];
      for (const id of ids) {
        let dir = this.root, e = findEntry(dir, id);
        while (e && e.run === 0 && this.leaves.has(e.offset)) e = findEntry(this.leaves.get(e.offset), id);
        if (!e) continue;  // not in the archive
        if (e.run > 0) out.set(id, { offset: h.dataOffset + e.offset, length: e.length });
        else {
          needLeaves.set(e.offset, e.length);
          rest.push(id);
        }
      }
      ids = rest;
      if (!needLeaves.size) break;
      const ranges = [...needLeaves].map(([off, len]) => ({ start: off, end: off + len, parts: [{ off, len }] }));
      for (const chunk of coalesce(ranges, gap, 8 << 20)) {
        const bytes = await this.read(h.leafOffset + chunk.start, chunk.end - chunk.start);
        for (const part of chunk.parts)
          this.leaves.set(part.off, deserializeDir(decompress(bytes.subarray(part.off - chunk.start, part.off - chunk.start + part.len), h.internalCompression)));
        onProgress?.(chunk.end - chunk.start);
      }
    }
    return out;
  }
  // every tile in the archive: [{id, offset, length}] (offsets inside the tile data section)
  async allEntries() {
    const h = this.header, out = [];
    const walk = async (dir, depth) => {
      for (const e of dir) {
        if (e.run > 0) for (let i = 0; i < e.run; i++) out.push({ id: e.id + i, offset: e.offset, length: e.length });
        else if (depth < 4) {
          let leaf = this.leaves.get(e.offset);
          if (!leaf) {
            leaf = deserializeDir(decompress(await this.read(h.leafOffset + e.offset, e.length), h.internalCompression));
            this.leaves.set(e.offset, leaf);
          }
          await walk(leaf, depth + 1);
        }
      }
    };
    await walk(this.root, 0);
    return out;
  }
  async get(z, x, y) {
    const loc = (await this.locate([tileId(z, x, y)])).values().next().value;
    if (!loc) return null;
    return decompress(await this.read(loc.offset, loc.length), this.header.tileCompression);
  }
}
// sorted ranges {start, end, parts} -> fewer, bigger ones (gaps up to `gap` bytes are read and thrown away)
export function coalesce(ranges, gap, maxBytes) {
  ranges = [...ranges].sort((a, b) => a.start - b.start);
  const out = [];
  for (const r of ranges) {
    const last = out[out.length - 1];
    if (last && r.start - last.end <= gap && Math.max(last.end, r.end) - last.start <= maxBytes) {
      last.end = Math.max(last.end, r.end);
      last.parts.push(...r.parts);
    } else out.push({ start: r.start, end: r.end, parts: [...r.parts] });
  }
  return out;
}
export function fileReader(path) {
  const fd = fs.openSync(path, "r");
  const read = async (offset, length) => {
    const b = Buffer.alloc(length);
    let got = 0;
    while (got < length) {
      const n = fs.readSync(fd, b, got, length - got, offset + got);
      if (n === 0) break;
      got += n;
    }
    return got === length ? b : b.subarray(0, got);
  };
  read.close = () => fs.closeSync(fd);
  return read;
}

// ---- writing. tiles: [{id, size, hash, read()}] in any order; read() gives the bytes (already
// compressed as info.tileCompression says) when it's their turn, so big packs never sit in memory.
// Identical tiles (same hash) are stored once; neighbouring ids with the same tile become one run.
export function hashOf(data) {
  return crypto.createHash("sha1").update(data).digest("hex");
}
export function inMemory(id, data) {
  return { id, size: data.length, hash: hashOf(data), read: () => data };
}
export function writePmtiles(path, tiles, info) {
  tiles = [...tiles].sort((a, b) => a.id - b.id);
  const unique = [];      // tiles whose bytes get written, in order
  const seen = new Map();  // hash -> offset
  const entries = [];
  let dataLen = 0;
  for (const t of tiles) {
    let off = seen.get(t.hash);
    if (off === undefined) {
      off = dataLen;
      seen.set(t.hash, off);
      unique.push(t);
      dataLen += t.size;
    }
    const last = entries[entries.length - 1];
    if (last && last.offset === off && last.id + last.run === t.id) last.run++;
    else entries.push({ id: t.id, offset: off, length: t.size, run: 1 });
  }
  // root directory, or a root of leaf directories when it won't fit in the first 16 KB
  const gz = (b) => zlib.gzipSync(b, { level: 9 });
  let root = gz(serializeDir(entries)), leafBytes = Buffer.alloc(0);
  for (let per = 4096; root.length > ROOT_MAX; per *= 2) {
    const leaves = [], rootEntries = [];
    let at = 0;
    for (let i = 0; i < entries.length; i += per) {
      const leaf = gz(serializeDir(entries.slice(i, i + per)));
      rootEntries.push({ id: entries[i].id, offset: at, length: leaf.length, run: 0 });
      leaves.push(leaf);
      at += leaf.length;
    }
    root = gz(serializeDir(rootEntries));
    leafBytes = Buffer.concat(leaves);
  }
  const meta = gz(Buffer.from(JSON.stringify(info.metadata || {})));
  const h = {
    rootOffset: HEADER_BYTES, rootLength: root.length,
    metadataOffset: HEADER_BYTES + root.length, metadataLength: meta.length,
    leafOffset: HEADER_BYTES + root.length + meta.length, leafLength: leafBytes.length,
    dataOffset: HEADER_BYTES + root.length + meta.length + leafBytes.length, dataLength: dataLen,
    addressedTiles: tiles.length, tileEntries: entries.length, tileContents: unique.length,
    clustered: true, internalCompression: Compression.gzip,
    tileCompression: info.tileCompression ?? Compression.none, tileType: info.tileType,
    minZoom: info.minZoom, maxZoom: info.maxZoom,
    minLon: info.bounds[0], minLat: info.bounds[1], maxLon: info.bounds[2], maxLat: info.bounds[3],
    centerZoom: info.centerZoom ?? info.minZoom,
    centerLon: info.center?.[0] ?? (info.bounds[0] + info.bounds[2]) / 2,
    centerLat: info.center?.[1] ?? (info.bounds[1] + info.bounds[3]) / 2,
  };
  const fd = fs.openSync(path + ".part", "w");
  try {
    let pos = 0;
    const put = (b) => {
      fs.writeSync(fd, b, 0, b.length, pos);
      pos += b.length;
    };
    for (const b of [headerBytes(h), root, meta, leafBytes]) put(b);
    for (const t of unique) {
      const b = t.read();
      if (b.length !== t.size) throw new Error(`tile ${t.id} changed size while packing`);
      put(b);
    }
    fs.closeSync(fd);
  } catch (err) {
    try { fs.closeSync(fd); } catch {}
    throw err;
  }
  fs.renameSync(path + ".part", path);
  return { bytes: h.dataOffset + dataLen, header: h };
}
