#include "tiles.h"

#include <mutex>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#define LODEPNG_NO_COMPILE_CPP  // the C API only (its C++ wrappers clash inside LVGL's extern "C")
#include "lvgl.h"
extern "C" {
#include "src/libs/lodepng/lodepng.h"
#include "src/libs/tjpgd/tjpgd.h"
}

namespace st {

// ---------------------------------------------------------------- decoding
static bool decodePng(const uint8_t *data, size_t len, uint8_t *rgb) {
  unsigned char *out = nullptr;
  unsigned w = 0, h = 0;
  bool ok = false;
  // LVGL's lodepng hands back an lv_draw_buf_t. Its header says ARGB8888 with a 1024-byte stride, but
  // decode24 fills it with packed RGB888 rows of 768 bytes: ignore the header and copy it straight.
  if (lodepng_decode24(&out, &w, &h, data, len) == 0 && out && w == 256 && h == 256) {
    lv_draw_buf_t *b = (lv_draw_buf_t *)out;
    if (b->data && b->data_size >= 256 * 256 * 3) {
      memcpy(rgb, b->data, 256 * 256 * 3);
      ok = true;
    }
  }
  if (out) lv_draw_buf_destroy((lv_draw_buf_t *)out);
  return ok;
}

struct JpgCtx {
  const uint8_t *src;
  size_t len, pos;
  uint8_t *rgb;
};
static size_t jpgIn(JDEC *jd, uint8_t *buf, size_t n) {
  JpgCtx *c = (JpgCtx *)jd->device;
  n = std::min(n, c->len - c->pos);
  if (buf) memcpy(buf, c->src + c->pos, n);
  c->pos += n;
  return n;
}
// LVGL's copy of TJpgDec hands out blocks in B, G, R order (LVGL's own byte order): swap to R, G, B.
static int jpgOut(JDEC *jd, void *bitmap, JRECT *r) {
  JpgCtx *c = (JpgCtx *)jd->device;
  const uint8_t *s = (const uint8_t *)bitmap;
  int w = r->right - r->left + 1;
  for (int y = r->top; y <= r->bottom && y < 256; y++, s += w * 3) {
    if (r->left >= 256) continue;
    uint8_t *d = c->rgb + (y * 256 + r->left) * 3;
    for (int x = 0, n = std::min(w, 256 - (int)r->left); x < n; x++) {
      d[x * 3] = s[x * 3 + 2];
      d[x * 3 + 1] = s[x * 3 + 1];
      d[x * 3 + 2] = s[x * 3];
    }
  }
  return 1;
}
static bool decodeJpeg(const uint8_t *data, size_t len, uint8_t *rgb) {
  static void *pool = nullptr;  // TJpgDec's work area, kept for the next tile
  const size_t POOL = 5120;
  if (!pool) pool = extMalloc(POOL);
  if (!pool) return false;
  JDEC jd;
  JpgCtx c = {data, len, 0, rgb};
  if (jd_prepare(&jd, jpgIn, pool, POOL, &c) != JDR_OK || jd.width != 256 || jd.height != 256) return false;
  return jd_decomp(&jd, jpgOut, 0) == JDR_OK;
}

bool decodeTileRGB(const uint8_t *data, size_t len, uint8_t *rgb) {
  if (!data || len < 8 || !rgb) return false;
  static std::mutex mtx;  // the decoders keep shared work areas; the card worker and the network task both decode
  std::lock_guard<std::mutex> g(mtx);
  if (data[0] == 0x89 && data[1] == 'P') return decodePng(data, len, rgb);
  if (data[0] == 0xFF && data[1] == 0xD8) return decodeJpeg(data, len, rgb);  // also JPEGs saved as .png
  return false;
}

int tileBrightness(const uint8_t *rgb) {
  uint32_t sum = 0, n = 0;
  for (int i = 0; i < 256 * 256; i += 61, n++) sum += (rgb[i * 3] * 3 + rgb[i * 3 + 1] * 6 + rgb[i * 3 + 2]) / 10;
  return n ? (int)(sum / n) : 0;
}

bool gunzip(const uint8_t *in, size_t n, std::vector<uint8_t> &out) {
  if (n < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8) return false;
  uint8_t fl = in[3];
  size_t i = 10;
  if (fl & 4) {  // FEXTRA
    if (i + 2 > n) return false;
    i += 2 + (in[i] | in[i + 1] << 8);
  }
  if (fl & 8)  // FNAME
    while (i < n && in[i++]) {
    }
  if (fl & 16)  // FCOMMENT
    while (i < n && in[i++]) {
    }
  if (fl & 2) i += 2;  // FHCRC
  if (i + 8 > n) return false;
  unsigned char *o = nullptr;
  size_t on = 0;
  unsigned err = lodepng_inflate(&o, &on, in + i, n - i - 8, &lodepng_default_decompress_settings);
  if (!err && o) out.assign(o, o + on);
  if (o) lv_free(o);
  return !err;
}

// ---------------------------------------------------------------- PMTiles v3
static uint64_t le64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
  return v;
}
static int32_t le32s(const uint8_t *p) { return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24); }
static bool varint(const uint8_t *&p, const uint8_t *end, uint64_t &v) {
  v = 0;
  for (int shift = 0; p < end && shift < 64; shift += 7) {
    uint8_t b = *p++;
    v |= (uint64_t)(b & 0x7F) << shift;
    if (!(b & 0x80)) return true;
  }
  return false;
}

uint64_t PmTiles::tileId(int z, uint32_t x, uint32_t y) {
  uint64_t acc = (((uint64_t)1 << (2 * z)) - 1) / 3;  // tiles in all lower zooms
  uint64_t d = 0;
  for (uint64_t s = ((uint64_t)1 << z) / 2; s > 0; s /= 2) {
    uint64_t rx = (x & s) ? 1 : 0, ry = (y & s) ? 1 : 0;
    d += s * s * ((3 * rx) ^ ry);
    if (ry == 0) {  // rotate the quadrant
      if (rx == 1) {
        x = (uint32_t)(s - 1 - x);
        y = (uint32_t)(s - 1 - y);
      }
      std::swap(x, y);
    }
  }
  return acc + d;
}

bool PmTiles::readDir(uint64_t off, uint64_t len, Dir &d) {
  if (len == 0 || len > 4 * 1024 * 1024) return false;
  std::vector<uint8_t, ExtAlloc<uint8_t>> raw(len);
  if (!read(off, (uint32_t)len, raw.data())) return false;
  std::vector<uint8_t> plain;
  const uint8_t *p, *end;
  if (internalComp_ == 2) {
    if (!gunzip(raw.data(), raw.size(), plain)) return false;
    p = plain.data();
    end = p + plain.size();
  } else if (internalComp_ == 1 || internalComp_ == 0) {
    p = raw.data();
    end = p + raw.size();
  } else return false;  // brotli / zstd: not on a microcontroller
  uint64_t n, v;
  if (!varint(p, end, n) || n > 2000000) return false;
  d.assign(n, Entry{0, 0, 0, 0});
  uint64_t last = 0;
  for (uint64_t i = 0; i < n; i++) {
    if (!varint(p, end, v)) return false;
    last += v;
    d[i].id = last;
  }
  for (uint64_t i = 0; i < n; i++) {
    if (!varint(p, end, v)) return false;
    d[i].run = (uint32_t)v;
  }
  for (uint64_t i = 0; i < n; i++) {
    if (!varint(p, end, v)) return false;
    d[i].len = (uint32_t)v;
  }
  for (uint64_t i = 0; i < n; i++) {
    if (!varint(p, end, v)) return false;
    d[i].off = (v == 0 && i > 0) ? d[i - 1].off + d[i - 1].len : v - 1;
  }
  return true;
}

const PmTiles::Entry *PmTiles::find(const Dir &d, uint64_t id) {
  // the last entry whose id <= the one we want
  auto it = std::upper_bound(d.begin(), d.end(), id, [](uint64_t v, const Entry &e) { return v < e.id; });
  if (it == d.begin()) return nullptr;
  const Entry &e = *(it - 1);
  if (e.run == 0) return &e;               // a leaf directory covers it
  if (id - e.id < e.run) return &e;        // a run of identical tiles (e.g. open water)
  return nullptr;
}

bool PmTiles::open() {
  uint8_t h[127];
  if (!read || !read(0, sizeof h, h)) {
    error = "can't read the file";
    return false;
  }
  if (memcmp(h, "PMTiles", 7) != 0 || h[7] != 3) {
    error = "not a PMTiles v3 archive";
    return false;
  }
  uint64_t rootOff = le64(h + 8), rootLen = le64(h + 16);
  leafOff_ = le64(h + 40);
  dataOff_ = le64(h + 56);
  bytes = dataOff_ + le64(h + 64);
  tiles = le64(h + 72);
  internalComp_ = h[97];
  tileComp_ = h[98];
  tileType = h[99];
  minZoom = h[100];
  maxZoom = h[101];
  minLon = le32s(h + 102) / 1e7;
  minLat = le32s(h + 106) / 1e7;
  maxLon = le32s(h + 110) / 1e7;
  maxLat = le32s(h + 114) / 1e7;
  if (tileType != 2 && tileType != 3) {
    error = tileType == 1 ? "vector tiles (only PNG or JPEG pictures can be shown)" : "tiles aren't PNG or JPEG";
    return false;
  }
  if (!readDir(rootOff, rootLen, root_)) {
    error = "can't read its directory";
    return false;
  }
  return true;
}

bool PmTiles::covers(int z, uint32_t x, uint32_t y) const {
  if (z < minZoom || z > maxZoom) return false;
  double n = std::pow(2.0, z);
  double lon0 = x / n * 360 - 180, lon1 = (x + 1) / n * 360 - 180;
  double lat0 = std::atan(std::sinh(M_PI * (1 - 2 * (y + 1) / n))) * 180 / M_PI;  // south edge
  double lat1 = std::atan(std::sinh(M_PI * (1 - 2 * y / n))) * 180 / M_PI;        // north edge
  return lon1 >= minLon && lon0 <= maxLon && lat1 >= minLat && lat0 <= maxLat;
}

bool PmTiles::get(int z, uint32_t x, uint32_t y, std::vector<uint8_t> &tile) {
  // only the zoom range: packs often hold tiles reaching past the bounds in their header
  if (z < minZoom || z > maxZoom) return false;
  uint64_t id = tileId(z, x, y);
  const Dir *dir = &root_;
  for (int depth = 0; depth < 4; depth++) {
    const Entry *e = find(*dir, id);
    if (!e) return false;
    if (e->run > 0) {
      if (e->len == 0 || e->len > 1024 * 1024) return false;
      std::vector<uint8_t> raw(e->len);
      if (!read(dataOff_ + e->off, e->len, raw.data())) return false;
      if (tileComp_ == 2) return gunzip(raw.data(), raw.size(), tile);
      tile.swap(raw);
      return true;
    }
    // a leaf directory: reuse it if it was read lately
    uint64_t off = leafOff_ + e->off;
    Leaf *l = nullptr;
    for (auto &c : leaves_)
      if (c.off == off) l = &c;
    if (!l) {
      if (leaves_.size() < 4) leaves_.emplace_back();
      l = &leaves_[0];
      for (auto &c : leaves_)
        if (c.used < l->used) l = &c;  // the least recently used one
      l->off = 0;
      if (!readDir(off, e->len, l->dir)) return false;
      l->off = off;
    }
    l->used = ++useCtr_;
    dir = &l->dir;
  }
  return false;
}

}  // namespace st
