// Map tiles from a microSD card: PNG and JPEG decoding, and PMTiles v3 packs (the one-file format
// WadaMesh and other MeshCore / Meshtastic firmwares read: /maps/<name>/<name>.pmtiles).
// No hardware here, so the desktop simulator tests it too.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "pstr.h"

namespace st {

// Decode a 256x256 PNG or JPEG map tile into RGB888 (256 * 256 * 3 bytes). False if it isn't one.
bool decodeTileRGB(const uint8_t *data, size_t len, uint8_t *rgb);
// Average brightness 0..255. A tile drawn dark already (a CARTO-dark pack, say) isn't darkened again at night.
int tileBrightness(const uint8_t *rgb);
// gzip (RFC 1952) -> bytes
bool gunzip(const uint8_t *in, size_t n, std::vector<uint8_t> &out);

// A PMTiles v3 archive of raster tiles (PNG or JPEG), read through `read` (random access to the file).
// The spec: https://github.com/protomaps/PMTiles/blob/main/spec/v3/spec.md
class PmTiles {
 public:
  std::function<bool(uint64_t off, uint32_t len, uint8_t *dst)> read;
  std::string name, error;
  int minZoom = 0, maxZoom = 0;
  double minLon = -180, minLat = -85, maxLon = 180, maxLat = 85;
  uint8_t tileType = 0;  // 2 PNG, 3 JPEG
  uint64_t tiles = 0;    // tiles in the archive
  uint64_t bytes = 0;    // where its tile data ends (the file's size, near enough)

  bool open();                                              // header + root directory
  bool covers(int z, uint32_t x, uint32_t y) const;        // inside its zoom range and stated bounds (a hint)
  bool get(int z, uint32_t x, uint32_t y, std::vector<uint8_t> &tile);  // false = not in the pack
  static uint64_t tileId(int z, uint32_t x, uint32_t y);  // Hilbert-curve tile id

 private:
  struct Entry {
    uint64_t id, off;
    uint32_t len, run;  // run 0 = a leaf directory
  };
  using Dir = std::vector<Entry, ExtAlloc<Entry>>;
  struct Leaf {
    uint64_t off = 0;
    uint32_t used = 0;
    Dir dir;
  };
  bool readDir(uint64_t off, uint64_t len, Dir &d);
  static const Entry *find(const Dir &d, uint64_t id);
  Dir root_;
  std::vector<Leaf> leaves_;  // the last few leaf directories used
  uint32_t useCtr_ = 0;
  uint64_t leafOff_ = 0, dataOff_ = 0;
  uint8_t internalComp_ = 0, tileComp_ = 0;
};

}  // namespace st
