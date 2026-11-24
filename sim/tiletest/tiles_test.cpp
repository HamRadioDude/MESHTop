// Checks src/core/tiles.cpp against fixtures written by the reference PMTiles library
// (tiletest/make_fixtures.py): Hilbert tile ids, directory and leaf walking, tile bytes, gzip,
// PNG/JPEG decoding and dark-pack detection.   make -C sim tiles-test
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/tiles.h"
#include "lvgl.h"

static int fails = 0;
static void check(bool ok, const std::string &what) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
  fails += !ok;
}
static std::vector<uint8_t> slurp(const std::string &p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static bool openPack(st::PmTiles &pm, const std::string &path, FILE *&fp) {
  fp = fopen(path.c_str(), "rb");
  if (!fp) return false;
  pm.read = [fp](uint64_t off, uint32_t len, uint8_t *dst) {
    return fseeko(fp, (off_t)off, SEEK_SET) == 0 && fread(dst, 1, len, fp) == len;
  };
  return pm.open();
}

int main() {
  lv_init();
  const std::string D = "tiletest/out/";
  {  // Hilbert ids match the reference over zooms 0..16
    std::ifstream f(D + "ids.txt");
    int z, n = 0, bad = 0;
    uint32_t x, y;
    unsigned long long id;
    while (f >> z >> x >> y >> id) {
      n++;
      if (st::PmTiles::tileId(z, x, y) != id) bad++;
    }
    check(n > 900 && bad == 0, "tile ids match the PMTiles reference (" + std::to_string(n) + " tiles, " + std::to_string(bad) + " wrong)");
  }
  {  // gzip
    std::vector<uint8_t> gz = slurp(D + "blob.gz"), raw = slurp(D + "blob.raw"), out;
    check(st::gunzip(gz.data(), gz.size(), out) && out == raw, "gzip decompresses");
  }
  // big / jpg / dark: from the reference library. mappack / mappack-dark: made by tools/mappack
  // (make-map-pack.mjs, light and dark styles), when tools/mappack/test/run-test.sh has been run.
  {  // decoded pixels match PIL's, for every PNG colour type and a JPEG
    static uint8_t px[256 * 256 * 3];
    for (const char *k : {"rgb", "pal", "rgba", "gray", "jpg"}) {
      std::string f = D + "pix_" + k + (std::string(k) == "jpg" ? ".jpg" : ".png");
      std::ifstream a(f, std::ios::binary), b(D + "pix_" + k + ".rgb", std::ios::binary);
      std::vector<uint8_t> in((std::istreambuf_iterator<char>(a)), {}), ref((std::istreambuf_iterator<char>(b)), {});
      bool dec = !in.empty() && ref.size() == sizeof px && st::decodeTileRGB(in.data(), in.size(), px);
      long diff = 0;
      int worst = 0;
      for (size_t i = 0; dec && i < sizeof px; i++) {
        int d = abs((int)px[i] - (int)ref[i]);
        diff += d;
        worst = std::max(worst, d);
      }
      double mean = dec ? (double)diff / sizeof px : 999;
      bool jpeg = std::string(k) == "jpg";
      check(dec && (jpeg ? mean < 4 : worst == 0), std::string("pixels of a ") + k + " tile match the reference (mean diff " +
                                                       std::to_string(mean).substr(0, 5) + ", worst " + std::to_string(worst) + ")");
    }
  }
  // writer-big: tools/mappack's pack writer at full Chicago size (leaf directories, repeated tiles).
  for (const char *name : {"big", "jpg", "dark", "mappack", "mappack-dark", "writer-big"}) {
    bool made = std::string(name).rfind("mappack", 0) == 0, writer = std::string(name) == "writer-big";
    if ((made || writer) && !std::ifstream(D + name + ".pmtiles")) {
      printf("SKIP %s.pmtiles (run tools/mappack/test/run-test.sh to make it)\n", name);
      continue;
    }
    st::PmTiles pm;
    FILE *fp = nullptr;
    bool opened = openPack(pm, D + name + ".pmtiles", fp);
    check(opened, std::string(name) + ".pmtiles opens (" + (opened ? std::to_string(pm.tiles) + " tiles, z" + std::to_string(pm.minZoom) + "-" + std::to_string(pm.maxZoom) : pm.error) + ")");
    if (!opened) continue;
    std::ifstream ex(D + name + ".expect");
    std::string line;
    int ok = 0, n = 0, missOk = 0, missN = 0;
    while (std::getline(ex, line)) {
      std::istringstream ss(line);
      int z;
      uint32_t x, y;
      unsigned long long id;
      std::string file;
      ss >> z >> x >> y >> id >> file;
      std::vector<uint8_t> got;
      bool found = pm.get(z, x, y, got);
      if (file == "-") {
        missN++;
        missOk += !found;
      } else {
        n++;
        std::vector<uint8_t> want = slurp("tiletest/" + file);
        bool same = found && got == want;
        ok += same;
        if (!same && n - ok <= 3)
          printf("  %s z%d/%u/%u: %s, got %zu bytes, want %zu\n", name, z, x, y, found ? "found" : "not found", got.size(), want.size());
      }
    }
    check(n > 0 && ok == n, std::string(name) + ": tiles read back byte for byte (" + std::to_string(ok) + "/" + std::to_string(n) + ")");
    check(missOk == missN, std::string(name) + ": tiles not in the pack aren't found");
    if (writer) {  // not pictures, just bytes
      fclose(fp);
      continue;
    }
    if (made) {  // every tile decodes, and the panel tells the dark style from the light one
      static uint8_t px[256 * 256 * 3];
      std::ifstream ex2(D + name + ".expect");
      int dec = 0, total = 0, darkN = 0;
      while (std::getline(ex2, line)) {
        std::istringstream ss(line);
        int z;
        uint32_t x, y;
        unsigned long long id;
        std::string file;
        ss >> z >> x >> y >> id >> file;
        std::vector<uint8_t> got;
        if (file == "-" || !pm.get(z, x, y, got)) continue;
        total++;
        if (st::decodeTileRGB(got.data(), got.size(), px)) {
          dec++;
          darkN += st::tileBrightness(px) < 100;
        }
      }
      bool wantDark = std::string(name) == "mappack-dark";
      check(total > 0 && dec == total, std::string(name) + ": every tile decodes (" + std::to_string(dec) + "/" + std::to_string(total) + ")");
      check(wantDark ? darkN == total : darkN == 0, std::string(name) + (wantDark ? ": all seen as dark (not darkened again at night)"
                                                                          : ": all seen as light (darkened at night)") +
                                                         " (" + std::to_string(darkN) + " dark of " + std::to_string(total) + ")");
      fclose(fp);
      continue;
    }
    // a real picture from it decodes; brightness tells a dark pack from a light one
    std::vector<uint8_t> t;
    static uint8_t rgb[256 * 256 * 3];
    bool dec = pm.get(6, 16, 23, t) && st::decodeTileRGB(t.data(), t.size(), rgb);
    int br = dec ? st::tileBrightness(rgb) : -1;
    bool dark = std::string(name) == "dark";
    check(dec && (dark ? br < 100 : br > 150), std::string(name) + ": a tile decodes (" + (pm.tileType == 3 ? "JPEG" : "PNG") + ", brightness " + std::to_string(br) + ")");
    fclose(fp);
  }
  {  // not PMTiles
    std::string junk = D + "blob.raw";
    st::PmTiles pm;
    FILE *fp = nullptr;
    bool refused = !openPack(pm, junk, fp);
    check(refused && !pm.error.empty(), "a file that isn't PMTiles is refused (" + pm.error + ")");
    if (fp) fclose(fp);
  }
  printf("%s (%d failed)\n", fails ? "TILES TEST FAILED" : "TILES TEST OK", fails);
  return fails ? 1 : 0;
}
