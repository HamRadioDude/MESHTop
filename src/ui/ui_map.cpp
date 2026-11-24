// MAP screen: baked base layer (tiles, radius ring, nodes) on a canvas + animated streak overlay.
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ui_int.h"

#if defined(SCOPETHING_DEVICE)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#define MAP_YIELD() vTaskDelay(1)
#else
#define MAP_YIELD() ((void)0)
#endif

namespace ui {
using namespace st;

static constexpr int TILE = 256;
static constexpr int MAX_ANIMS = 36;
static constexpr int MAX_PULSES = 40;

static lv_obj_t *root, *canvas, *fx;
static lv_obj_t *feedPanel, *tools, *btnPause, *legend, *attrib, *pausedBadge;
static lv_obj_t *card, *cardRole, *cardName, *cardMeta, *btnStar, *btnWatch;
// the live traffic panel folds down to one line (Settings.feedFolded): a pull tab folds it, the line unfolds it
static lv_obj_t *feedTab, *feedLine, *feedLineTag, *feedLineWho, *feedLineAgo;
struct FeedRow {
  lv_obj_t *box, *tag, *ago, *who, *det;
};
static FeedRow frows[5];
static lv_obj_t *feedEmpty;

static uint16_t *buf = nullptr;
static int W = SCR_W, H = MAIN_H;
static double vLat = 41.8781, vLon = -87.6298;
static int vZ = 9;
static double cwx = 0, cwy = 0;  // world px of view center
static int dragDx = 0, dragDy = 0;
static bool dragging = false, dragMoved = false;
static lv_point_t dragStart;
static bool baseDirty = true, paused = false, visible = true;
static uint32_t lastBake = 0;
static std::string selKey;

struct Anim {
  std::vector<LatLon> pts;
  uint32_t color;
  uint32_t t0, dur;
  bool mine, pulsed;
  lv_area_t box;
  bool hasBox;
};
struct Pulse {
  LatLon ll;
  uint32_t color;
  uint32_t t0;
  bool small;
  lv_area_t box;
  bool hasBox;
};
static std::vector<Anim> anims;
static std::vector<Pulse> pulses;
static std::vector<PathStop> pinStops;  // a route pinned from MESH (see mapShowPath)
static bool pinStar = false;
static std::string pinTitle;
static lv_obj_t *pinBar, *pinLbl;
static void showPinBar();
static uint32_t fxNow = 0;

struct Hit {
  int idx;
  int16_t x, y;
};
static std::vector<Hit> hits;
static int onMap = 0;

// ---------------------------------------------------------------- projection
static void worldXY(double lat, double lon, int z, double &x, double &y) {
  double s = TILE * std::pow(2.0, z);
  double sl = std::sin(lat * M_PI / 180.0);
  sl = std::fmin(std::fmax(sl, -0.9999), 0.9999);
  x = (lon + 180.0) / 360.0 * s;
  y = (0.5 - std::log((1 + sl) / (1 - sl)) / (4 * M_PI)) * s;
}
static void unproject(double x, double y, int z, double &lat, double &lon) {
  double s = TILE * std::pow(2.0, z);
  lon = x / s * 360.0 - 180.0;
  double n = M_PI - 2 * M_PI * y / s;
  lat = 180.0 / M_PI * std::atan(0.5 * (std::exp(n) - std::exp(-n)));
}
static void updateCenterW() { worldXY(vLat, vLon, vZ, cwx, cwy); }
static inline void toScreen(double lat, double lon, float &sx, float &sy) {
  double x, y;
  worldXY(lat, lon, vZ, x, y);
  sx = (float)(x - cwx + W / 2.0 + dragDx);
  sy = (float)(y - cwy + H / 2.0 + dragDy);
}
static double metersPerPixel(double lat, int z) { return 156543.03392 * std::cos(lat * M_PI / 180) / std::pow(2.0, z); }
static int fitZoom(double lat, int radiusMi) {
  for (int z = 15; z >= 3; z--)
    if (radiusMi * 1609.34 / metersPerPixel(lat, z) <= H / 2.0 - 16) return z;
  return 3;
}

// ---------------------------------------------------------------- raster helpers (direct to the canvas buffer)
static inline uint16_t c565(uint32_t c) {
  return (uint16_t)((((c >> 16) & 0xFF) >> 3) << 11 | (((c >> 8) & 0xFF) >> 2) << 5 | ((c & 0xFF) >> 3));
}
static inline void blend(uint16_t &d, uint16_t s, int a) {  // a 0..256
  if (a >= 256) {
    d = s;
    return;
  }
  int dr = d >> 11, dg = (d >> 5) & 63, db = d & 31;
  int sr = s >> 11, sg = (s >> 5) & 63, sb = s & 31;
  d = (uint16_t)(((dr + (((sr - dr) * a) >> 8)) << 11) | ((dg + (((sg - dg) * a) >> 8)) << 5) | (db + (((sb - db) * a) >> 8)));
}
static inline void px(int x, int y, uint16_t c, int a = 256) {
  if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) blend(buf[y * W + x], c, a);
}
static void fillCircle(int cx, int cy, float r, uint16_t c) {
  int ri = (int)std::ceil(r) + 1;
  for (int dy = -ri; dy <= ri; dy++)
    for (int dx = -ri; dx <= ri; dx++) {
      float d = std::sqrt((float)(dx * dx + dy * dy));
      if (d <= r - 0.5f) px(cx + dx, cy + dy, c);
      else if (d < r + 0.5f) px(cx + dx, cy + dy, c, (int)((r + 0.5f - d) * 256));
    }
}
static void ring(int cx, int cy, float r, float w, uint16_t c, int alpha = 256) {
  int ri = (int)std::ceil(r + w) + 1;
  for (int dy = -ri; dy <= ri; dy++)
    for (int dx = -ri; dx <= ri; dx++) {
      float d = std::fabs(std::sqrt((float)(dx * dx + dy * dy)) - r);
      if (d < w / 2 + 0.5f) {
        float cov = std::fmin(1.f, w / 2 + 0.5f - d);
        px(cx + dx, cy + dy, c, (int)(cov * alpha));
      }
    }
}
static void diamond(int cx, int cy, int s, uint16_t c) {
  for (int dy = -s; dy <= s; dy++)
    for (int dx = -s; dx <= s; dx++) {
      int d = std::abs(dx) + std::abs(dy);
      if (d < s) px(cx + dx, cy + dy, c);
      else if (d == s) px(cx + dx, cy + dy, c, 110);
    }
}
static void square(int cx, int cy, int h, uint16_t c) {
  for (int dy = -h; dy <= h; dy++)
    for (int dx = -h; dx <= h; dx++) px(cx + dx, cy + dy, c);
}
static void dashedCircle(int cx, int cy, double r, uint16_t c, int alpha) {
  if (r < 4 || r > 4000) return;
  double circ = 2 * M_PI * r;
  int steps = (int)circ;
  for (int i = 0; i < steps; i++) {
    double along = std::fmod((double)i, 10.0);
    if (along >= 4) continue;  // 4 px on, 6 px off
    double a = (double)i / r;
    int x = cx + (int)std::lround(std::cos(a) * r), y = cy + (int)std::lround(std::sin(a) * r);
    px(x, y, c, alpha);
    px(x + 1, y, c, alpha / 2);
    px(x, y + 1, c, alpha / 2);
  }
}

static uint32_t roleColor(Role r) { return rcol(r); }

// ---------------------------------------------------------------- bake
static void bake() {
  baseDirty = false;
  lastBake = lv_tick_get();
  const uint16_t bg = c565(C_BG);
  updateCenterW();

  // Which tiles land where. Nothing is drawn yet.
  struct TileDraw {
    const uint16_t *tp;
    int ox, oy;
    int sh = 0, sx = 0, sy = 0;  // stretched: a lower-zoom tile enlarged 2^sh times, from (sx, sy) in it
  };
  std::vector<TileDraw> draws;
  // day mode always uses the normal (light) street map; at night the user's choice
  std::string style = M->cfg.mapStyle == "none" ? "none" : (dayMode() ? "light" : M->cfg.mapStyle);
  if (style != "none") {
    int n = 1 << vZ;
    int tx0 = (int)std::floor((cwx - W / 2.0) / TILE), tx1 = (int)std::floor((cwx + W / 2.0) / TILE);
    int ty0 = (int)std::floor((cwy - H / 2.0) / TILE), ty1 = (int)std::floor((cwy + H / 2.0) / TILE);
    // centre tiles first so they are requested first
    std::vector<std::pair<int, int>> need;
    for (int ty = ty0; ty <= ty1; ty++)
      for (int tx = tx0; tx <= tx1; tx++) need.push_back({tx, ty});
    double ccx = cwx / TILE, ccy = cwy / TILE;
    std::sort(need.begin(), need.end(), [&](auto &a, auto &b) {
      double da = std::hypot(a.first + .5 - ccx, a.second + .5 - ccy), db = std::hypot(b.first + .5 - ccx, b.second + .5 - ccy);
      return da < db;
    });
    for (auto &t : need) {
      int tx = t.first, ty = t.second;
      if (ty < 0 || ty >= n) continue;
      int wx = ((tx % n) + n) % n;
      const uint16_t *tp = P->tile(vZ, wx, ty, style);
      int ox = (int)std::lround(tx * (double)TILE - cwx + W / 2.0), oy = (int)std::lround(ty * (double)TILE - cwy + H / 2.0);
      if (tp) {
        draws.push_back({tp, ox, oy});
        continue;
      }
      // Not here (yet): enlarge the nearest lower-zoom tile we have, up to 5 levels down. When this one
      // can't be had (offline, or past a map pack's top zoom), ask for the next level down.
      bool missing = P->tileFailed(vZ, wx, ty, style);
      for (int d = 1; d <= 5 && vZ - d >= 0; d++) {
        int px = wx >> d, py = ty >> d;
        const uint16_t *pp = P->tilePeek(vZ - d, px, py, style);
        if (!pp && missing) pp = P->tile(vZ - d, px, py, style);
        if (pp) {
          int part = TILE >> d;
          draws.push_back({pp, ox, oy, d, (wx - (px << d)) * part, (ty - (py << d)) * part});
          break;
        }
        if (missing && !P->tileFailed(vZ - d, px, py, style)) break;  // asked for it: wait before going lower
      }
    }
  }
  const bool drewTile = !draws.empty();
  const bool knockBack = style == "light" && T.dark;  // light tiles are bright: knock them back so streaks pop
  const uint16_t gc = c565(T.grid), dk = c565(C_BG);
  int gox = (int)std::fmod(-cwx + W / 2.0, 40.0), goy = (int)std::fmod(-cwy + H / 2.0, 40.0);
  if (gox < 0) gox += 40;
  if (goy < 0) goy += 40;

  // Build the base map in bands of rows with a short pause between them. Rewriting the whole 768 KB
  // canvas in one burst used more PSRAM bandwidth than the RGB panel could spare, so the picture
  // jumped every time the map was opened.
  static constexpr int BAND = 32;
  for (int y0 = 0; y0 < H; y0 += BAND) {
    int y1 = std::min(H, y0 + BAND);
    for (int i = y0 * W, e = y1 * W; i < e; i++) buf[i] = bg;
    for (auto &d : draws) {
      int x0 = std::max(0, d.ox), x1 = std::min(W, d.ox + TILE);
      if (x1 <= x0) continue;
      int ya = std::max(y0, d.oy), yb = std::min(y1, d.oy + TILE);
      if (!d.sh) {
        for (int y = ya; y < yb; y++) std::memcpy(&buf[y * W + x0], &d.tp[(y - d.oy) * TILE + (x0 - d.ox)], (x1 - x0) * 2);
        continue;
      }
      for (int y = ya; y < yb; y++) {
        const uint16_t *src = &d.tp[(d.sy + ((y - d.oy) >> d.sh)) * TILE + d.sx];
        uint16_t *dst = &buf[y * W];
        for (int x = x0; x < x1; x++) dst[x] = src[(x - d.ox) >> d.sh];
      }
    }
    if (!drewTile) {
      for (int x = gox; x < W; x += 40)
        for (int y = y0; y < y1; y++) buf[y * W + x] = gc;
      for (int y = goy; y < H; y += 40)
        if (y >= y0 && y < y1)
          for (int x = 0; x < W; x++) buf[y * W + x] = gc;
    }
    if (knockBack)
      for (int i = y0 * W, e = y1 * W; i < e; i++) blend(buf[i], dk, 90);
    MAP_YIELD();  // let the panel's DMA catch up
  }

  // watch radius (area mode only)
  if (M->repeaterKey.empty()) {
    float cx, cy;
    int sdx = dragDx, sdy = dragDy;
    dragDx = dragDy = 0;
    toScreen(M->center.lat, M->center.lon, cx, cy);
    dragDx = sdx;
    dragDy = sdy;
    double r = M->cfg.radiusMi * 1609.34 / metersPerPixel(M->center.lat, vZ);
    dashedCircle((int)cx, (int)cy, r, c565(C_ACC), T.dark ? 110 : 170);
  }

  // nodes: companions, then repeaters, then mine / watched on top
  hits.clear();
  onMap = 0;
  struct Lbl {
    int idx, x, y;
    bool hi;
  };
  std::vector<Lbl> labels;
  int saveDx = dragDx, saveDy = dragDy;
  dragDx = dragDy = 0;
  for (int pass = 0; pass < 3; pass++) {
    for (size_t i = 0; i < M->nodes.size(); i++) {
      const Node &nd = M->nodes[i];
      if (!nd.hasLoc) continue;
      bool mine = M->isMine(nd.key), watched = !M->repeaterKey.empty() && nd.key == M->repeaterKey;
      bool rpt = nd.role == ROLE_REPEATER;
      int layer = (mine || watched) ? 2 : rpt ? 1 : 0;
      if (layer != pass) continue;
      float sx, sy;
      toScreen(nd.lat, nd.lon, sx, sy);
      if (sx < -20 || sx > W + 20 || sy < -20 || sy > H + 20) continue;
      int x = (int)std::lround(sx), y = (int)std::lround(sy);
      uint16_t col = c565(roleColor(nd.role));
      if (rpt) diamond(x, y, watched ? 6 : 5, col);
      else if (nd.role == ROLE_ROOM) square(x, y, 3, col);
      else fillCircle(x, y, 3.2f, col);
      if (mine || watched) ring(x, y, 8, 2, c565(T.ring));
      hits.push_back({(int)i, (int16_t)x, (int16_t)y});
      onMap++;
      if ((mine || watched || (rpt && vZ >= 11)) && labels.size() < 80) labels.push_back({(int)i, x, y, mine || watched});
    }
  }
  dragDx = saveDx;
  dragDy = saveDy;

  if (!labels.empty()) {
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    for (auto &L : labels) {
      const Node &nd = M->nodes[L.idx];
      lv_point_t sz;
      lv_text_get_size(&sz, nd.name.c_str(), &lv_font_montserrat_12, 0, 0, 400, LV_TEXT_FLAG_NONE);
      lv_area_t a = {L.x + 9, L.y - 8, L.x + 9 + sz.x + 7, L.y + 7};
      lv_draw_rect_dsc_t rd;
      lv_draw_rect_dsc_init(&rd);
      rd.bg_color = lv_color_hex(C_BG);
      rd.bg_opa = 190;
      rd.radius = 3;
      lv_draw_rect(&layer, &rd, &a);
      lv_draw_label_dsc_t ld;
      lv_draw_label_dsc_init(&ld);
      ld.font = &lv_font_montserrat_12;
      ld.color = lv_color_hex(L.hi ? T.labelHi : T.labelFg);
      ld.text = nd.name.c_str();
      lv_area_t ta = {a.x1 + 4, a.y1 + 1, a.x2, a.y2};
      lv_draw_label(&layer, &ld, &ta);
    }
    lv_canvas_finish_layer(canvas, &layer);
  }
  lv_obj_set_pos(canvas, 0, 0);
  dragDx = dragDy = 0;
  lv_obj_invalidate(canvas);
  lv_obj_invalidate(fx);
}

// ---------------------------------------------------------------- streaks
static void pointAt(const std::vector<lv_point_precise_t> &p, const std::vector<float> &cum, float d, float &x, float &y) {
  for (size_t i = 1; i < p.size(); i++)
    if (cum[i] >= d) {
      float seg = cum[i] - cum[i - 1];
      float f = seg > 0 ? (d - cum[i - 1]) / seg : 0;
      x = p[i - 1].x + (p[i].x - p[i - 1].x) * f;
      y = p[i - 1].y + (p[i].y - p[i - 1].y) * f;
      return;
    }
  x = p.back().x;
  y = p.back().y;
}
// LVGL 9.1's software line renderer divides by zero on (near) zero-length lines: skip those
static void seg(lv_layer_t *layer, lv_draw_line_dsc_t &d, lv_point_precise_t a, lv_point_precise_t b) {
  if (std::fabs(a.x - b.x) < 1.0f && std::fabs(a.y - b.y) < 1.0f) return;
  d.p1 = a;
  d.p2 = b;
  lv_draw_line(layer, &d);
}
static void strokeRange(lv_layer_t *layer, const std::vector<lv_point_precise_t> &p, const std::vector<float> &cum, float a,
                        float b, uint32_t color, int width, lv_opa_t opa) {
  if (b <= a || opa < 4) return;
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.color = lv_color_hex(color);
  d.width = width;
  d.opa = opa;
  d.round_start = 1;
  d.round_end = 1;
  float x, y;
  pointAt(p, cum, a, x, y);
  lv_point_precise_t prev = {x, y};
  for (size_t i = 1; i < p.size(); i++) {
    if (cum[i] > a && cum[i] < b) {
      seg(layer, d, prev, p[i]);
      prev = p[i];
    }
  }
  pointAt(p, cum, b, x, y);
  seg(layer, d, prev, {x, y});
}

static void screenPts(const std::vector<LatLon> &ll, int ox, int oy, std::vector<lv_point_precise_t> &out, std::vector<float> &cum) {
  out.clear();
  cum.clear();
  cum.push_back(0);
  for (auto &l : ll) {
    float x, y;
    toScreen(l.lat, l.lon, x, y);
    out.push_back({x + ox, y + oy});
  }
  for (size_t k = 1; k < out.size(); k++)
    cum.push_back(cum[k - 1] + std::hypot(out[k].x - out[k - 1].x, out[k].y - out[k - 1].y));
}

// the pinned route: a thick line (or a star from the first stop), a ringed dot and a name tag per stop
static void drawPin(lv_layer_t *layer, const lv_area_t &co) {
  std::vector<lv_point_precise_t> at;
  std::vector<const PathStop *> known;
  for (auto &st : pinStops)
    if (st.hasLoc) {
      float x, y;
      toScreen(st.lat, st.lon, x, y);
      at.push_back({x + co.x1, y + co.y1});
      known.push_back(&st);
    }
  if (known.empty()) return;
  static constexpr uint32_t PIN = 0xFFB547;
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.color = lv_color_hex(PIN);
  d.width = 4;
  d.opa = 235;
  d.round_start = 1;
  d.round_end = 1;
  for (size_t i = 1; i < at.size(); i++) seg(layer, d, pinStar ? at[0] : at[i - 1], at[i]);
  for (size_t i = 0; i < at.size(); i++) {
    bool end = known[i] == &pinStops.front() || (!pinStar && known[i] == &pinStops.back());  // sender / you
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_hex(end ? C_ACC : PIN);
    rd.bg_opa = LV_OPA_COVER;
    rd.radius = LV_RADIUS_CIRCLE;
    rd.border_width = 2;
    rd.border_color = T.dark ? lv_color_hex(0x0A0E13) : lv_color_white();
    int r = end ? 7 : 6;
    lv_area_t ar = {(int32_t)at[i].x - r, (int32_t)at[i].y - r, (int32_t)at[i].x + r, (int32_t)at[i].y + r};
    lv_draw_rect(layer, &rd, &ar);
    const char *txt = known[i]->label.c_str();
    if (!*txt) continue;
    lv_point_t sz;
    lv_text_get_size(&sz, txt, &lv_font_montserrat_12, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_area_t ta = {(int32_t)at[i].x + 10, (int32_t)at[i].y - 8, (int32_t)at[i].x + 10 + sz.x + 8, (int32_t)at[i].y + 8};
    lv_draw_rect_dsc_t bd;
    lv_draw_rect_dsc_init(&bd);
    bd.bg_color = lv_color_hex(C_BG);
    bd.bg_opa = 215;
    bd.radius = 4;
    lv_draw_rect(layer, &bd, &ta);
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &lv_font_montserrat_12;
    ld.color = lv_color_hex(C_TEXT);
    ld.text = txt;
    lv_area_t la = {ta.x1 + 4, ta.y1 + 1, ta.x2, ta.y2};
    lv_draw_label(layer, &ld, &la);
  }
}

static void fxDraw(lv_event_t *e) {
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t co;
  lv_obj_get_coords(fx, &co);
  if (!pinStops.empty()) drawPin(layer, co);
  std::vector<lv_point_precise_t> pts;
  std::vector<float> cum;
  for (auto &a : anims) {
    screenPts(a.pts, co.x1, co.y1, pts, cum);
    float L = cum.back();
    float prog = (float)(fxNow - a.t0) / a.dur;
    float fade = prog <= 1 ? 1 : std::fmax(0.f, 1 - (prog - 1) * a.dur / 1600.f);
    if (fade <= 0) continue;
    float head = std::fmin(1.f, prog) * L;
    strokeRange(layer, pts, cum, 0, head, a.color, a.mine ? 3 : 2, (lv_opa_t)((T.dark ? 0.30f : 0.50f) * fade * 255));
    if (prog <= 1.15f) {
      float tail = std::fmax(0.f, head - 70);
      strokeRange(layer, pts, cum, tail, head, a.color, 9, T.dark ? 56 : 80);
      strokeRange(layer, pts, cum, tail, head, a.color, 3, 255);
      float hx, hy;
      pointAt(pts, cum, head, hx, hy);
      lv_draw_rect_dsc_t rd;
      lv_draw_rect_dsc_init(&rd);
      rd.bg_color = T.dark ? lv_color_white() : lv_color_hex(0x10161D);
      rd.bg_opa = 242;
      rd.radius = LV_RADIUS_CIRCLE;
      lv_area_t ar = {(int32_t)hx - 3, (int32_t)hy - 3, (int32_t)hx + 3, (int32_t)hy + 3};
      lv_draw_rect(layer, &rd, &ar);
    }
  }
  for (auto &p : pulses) {
    float age = (float)(fxNow - p.t0) / (p.small ? 700 : 1100);
    if (age >= 1 || age < 0) continue;
    float x, y;
    toScreen(p.ll.lat, p.ll.lon, x, y);
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.color = lv_color_hex(p.color);
    ad.width = 2;
    ad.opa = (lv_opa_t)((1 - age) * 0.9f * 255);
    ad.start_angle = 0;
    ad.end_angle = 360;
    ad.center = {(int32_t)x + co.x1, (int32_t)y + co.y1};
    ad.radius = (uint16_t)((p.small ? 4 : 5) + age * (p.small ? 10 : 20));
    lv_draw_arc(layer, &ad);
  }
}

static bool boxOf(const std::vector<LatLon> &ll, int pad, lv_area_t &out) {
  lv_area_t co;
  lv_obj_get_coords(fx, &co);
  float x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
  for (auto &l : ll) {
    float x, y;
    toScreen(l.lat, l.lon, x, y);
    x0 = std::fmin(x0, x);
    y0 = std::fmin(y0, y);
    x1 = std::fmax(x1, x);
    y1 = std::fmax(y1, y);
  }
  out = {(int32_t)x0 - pad + co.x1, (int32_t)y0 - pad + co.y1, (int32_t)x1 + pad + co.x1, (int32_t)y1 + pad + co.y1};
  out.x1 = std::max(out.x1, co.x1);
  out.y1 = std::max(out.y1, co.y1);
  out.x2 = std::min(out.x2, co.x2);
  out.y2 = std::min(out.y2, co.y2);
  return out.x2 >= out.x1 && out.y2 >= out.y1;
}

static void fxTimer(lv_timer_t *) {
  if (!visible) return;
  fxNow = lv_tick_get();
  if (anims.empty() && pulses.empty()) return;
  for (size_t i = 0; i < anims.size();) {
    Anim &a = anims[i];
    float prog = (float)(fxNow - a.t0) / a.dur;
    float fade = prog <= 1 ? 1 : 1 - (prog - 1) * a.dur / 1600.f;
    if (a.hasBox) lv_obj_invalidate_area(fx, &a.box);
    if (fade <= 0) {
      anims.erase(anims.begin() + i);
      continue;
    }
    if (prog >= 1 && !a.pulsed) {
      a.pulsed = true;
      if (pulses.size() < MAX_PULSES) pulses.push_back({a.pts.back(), a.color, fxNow, false, {}, false});
    }
    a.hasBox = boxOf(a.pts, 12, a.box);
    if (a.hasBox) lv_obj_invalidate_area(fx, &a.box);
    i++;
  }
  for (size_t i = 0; i < pulses.size();) {
    Pulse &p = pulses[i];
    if (p.hasBox) lv_obj_invalidate_area(fx, &p.box);
    float age = (float)(fxNow - p.t0) / (p.small ? 700 : 1100);
    if (age >= 1) {
      pulses.erase(pulses.begin() + i);
      continue;
    }
    p.hasBox = boxOf({p.ll}, 30, p.box);
    if (p.hasBox) lv_obj_invalidate_area(fx, &p.box);
    i++;
  }
}

void mapRoute(const Route &r) {
  if (paused || r.pts.empty()) return;
  uint32_t now = lv_tick_get();
  uint32_t col = gcol(r.group);
  if (r.pts.size() >= 2) {
    int segs = (int)r.pts.size() - 1;
    anims.push_back({r.pts, col, now, (uint32_t)std::min(4200, 650 + segs * 520), r.mine, false, {}, false});
    if (anims.size() > MAX_ANIMS) {
      if (anims.front().hasBox) lv_obj_invalidate_area(fx, &anims.front().box);
      anims.erase(anims.begin());
    }
  } else {
    pulses.push_back({r.pts[0], col, now, false, {}, false});
  }
  pulses.push_back({r.pts[0], col, now, true, {}, false});
  while (pulses.size() > MAX_PULSES) pulses.erase(pulses.begin());
}

// ---------------------------------------------------------------- node card
static void hideCard() {
  selKey.clear();
  lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
}
void mapRefreshCard() {
  if (selKey.empty()) return;
  Node *n = M->findKey(selKey);
  if (!n) {
    hideCard();
    return;
  }
  std::string role = roleName(n->role);
  for (auto &c : role) c = (char)toupper(c);
  if (!M->repeaterKey.empty() && n->key == M->repeaterKey) role += "  |  WATCHED REPEATER";
  lv_label_set_text(cardRole, role.c_str());
  lv_label_set_text(cardName, n->name.c_str());
  char meta[160];
  snprintf(meta, sizeof meta, "%.12s...   %.4f, %.4f\nHeard %s", n->key.c_str(), n->lat, n->lon,
           n->last ? (ago(n->last) + " ago").c_str() : "-");
  lv_label_set_text(cardMeta, meta);
  bool mine = M->isMine(n->key);
  setBtnText(btnStar, mine ? LV_SYMBOL_OK " In My Nodes" : LV_SYMBOL_PLUS " My Nodes");
  setBtnColors(btnStar, mine ? T.starOnBg : C_BTN, C_STAR);
  const Watch *w = M->watchFor(n->key, n->name);
  setBtnText(btnWatch, w ? (w->src == 0 ? "@ Watching" : (w->src == 2 ? "@ Via My Node" : "@ Via settings")) : "@ Watch");
  setBtnColors(btnWatch, w ? T.atOnBg : C_BTN, C_AT);
}
void mapShowNode(const std::string &key) {
  Node *n = M->findKey(key);
  if (!n) return;
  selKey = key;
  if (n->hasLoc) mapSetView(n->lat, n->lon, std::max(vZ, 12));
  lv_obj_remove_flag(card, LV_OBJ_FLAG_HIDDEN);
  mapRefreshCard();
}
static void onStar(lv_event_t *) {
  Node *n = M->findKey(selKey);
  if (!n) return;
  M->setMyNode(n->key, n->name, !M->isMine(n->key));
  P->requestMyNodes();
  mapRefreshCard();
  mapMarkDirty();
}
static void onWatch(lv_event_t *) {
  Node *n = M->findKey(selKey);
  if (!n) return;
  const Watch *w = M->watchFor(n->key, n->name);
  if (w && w->src != 0) return;  // comes from My Nodes or the settings page
  M->setMention(n->key, n->name, !w);
  mapRefreshCard();
}

// ---------------------------------------------------------------- input
static void tapAt(int x, int y) {
  int best = -1, bd = 22 * 22;
  for (auto &h : hits) {
    int dx = h.x - x, dy = h.y - y, d = dx * dx + dy * dy;
    if (d < bd) {
      bd = d;
      best = h.idx;
    }
  }
  if (best >= 0 && best < (int)M->nodes.size()) {
    selKey = M->nodes[best].key;
    lv_obj_remove_flag(card, LV_OBJ_FLAG_HIDDEN);
    mapRefreshCard();
  } else hideCard();
}
static void fxInput(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t *in = lv_indev_active();
  if (!in) return;
  lv_point_t p;
  lv_indev_get_point(in, &p);
  lv_area_t co;
  lv_obj_get_coords(fx, &co);
  touched();
  if (code == LV_EVENT_PRESSED) {
    dragging = true;
    dragMoved = false;
    dragStart = p;
  } else if (code == LV_EVENT_PRESSING && dragging) {
    int dx = p.x - dragStart.x, dy = p.y - dragStart.y;
    if (!dragMoved && dx * dx + dy * dy > 100) dragMoved = true;
    if (dragMoved) {
      dragDx = dx;
      dragDy = dy;
      lv_obj_set_pos(canvas, dx, dy);
      lv_obj_invalidate(fx);
    }
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (!dragging) return;
    dragging = false;
    if (dragMoved) {
      double lat, lon;
      unproject(cwx - dragDx, cwy - dragDy, vZ, lat, lon);
      vLat = lat;
      vLon = lon;
      bake();
    } else if (code == LV_EVENT_RELEASED) {
      tapAt(p.x - co.x1, p.y - co.y1);
    }
  }
}

void mapSetView(double lat, double lon, int z) {
  vLat = std::fmax(-84, std::fmin(84, lat));
  vLon = lon;
  vZ = std::max(3, std::min(17, z));
  P->dropTileRequests();
  bake();
}
void mapRecenter() { mapSetView(M->center.lat, M->center.lon, fitZoom(M->center.lat, M->repeaterKey.empty() ? M->cfg.radiusMi : 25)); }

// The live traffic panel: whole, or folded down to one line; neither while a route is pinned (it would
// cover the left of the route).
static void showFeed() {
  if (!feedPanel || !feedLine || !feedTab) return;
  bool pinned = !pinStops.empty(), folded = M->cfg.feedFolded;
  if (pinned || folded) lv_obj_add_flag(feedPanel, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(feedPanel, LV_OBJ_FLAG_HIDDEN);
  if (pinned || folded) lv_obj_add_flag(feedTab, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(feedTab, LV_OBJ_FLAG_HIDDEN);
  if (pinned || !folded) lv_obj_add_flag(feedLine, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(feedLine, LV_OBJ_FLAG_HIDDEN);
}
void toggleTrafficPanel() {
  M->cfg.feedFolded = !M->cfg.feedFolded;
  P->saveSettings();
  showFeed();
}
bool trafficPanelFolded() { return M->cfg.feedFolded; }
void mapSourceChanged() {
  if (attrib)
    lv_label_set_text(attrib, M->cfg.radioOnly() && !M->cfg.demo ? "Map (c) OpenStreetMap  |  Data: your radio" : "Map (c) OpenStreetMap  |  Data: CoreScope");
}

static void showPinBar() {
  if (!pinBar) return;
  showFeed();
  if (pinStops.empty()) {
    lv_obj_add_flag(pinBar, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  int known = 0;
  for (auto &st : pinStops) known += st.hasLoc;
  std::string t = pinTitle;
  if (t.size() > 56) {  // one line (cut on a character boundary)
    size_t cut = 54;
    while (cut > 0 && ((unsigned char)t[cut] & 0xC0) == 0x80) cut--;
    t = t.substr(0, cut) + "...";
  }
  if (known < (int)pinStops.size())
    t += "  (" + std::to_string((int)pinStops.size() - known) + " not on map)";
  lv_label_set_text(pinLbl, t.c_str());
  lv_obj_remove_flag(pinBar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(pinBar);
}
void mapShowPath(const std::string &title, const std::vector<PathStop> &stops, bool star) {
  pinStops = stops;
  pinStar = star;
  pinTitle = title;
  // fit the known stops on screen, leaving room for the name tags
  double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180;
  int known = 0;
  for (auto &st : pinStops)
    if (st.hasLoc) {
      la0 = std::fmin(la0, st.lat);
      la1 = std::fmax(la1, st.lat);
      lo0 = std::fmin(lo0, st.lon);
      lo1 = std::fmax(lo1, st.lon);
      known++;
    }
  if (known) {
    int z = 14;
    for (; z > 3; z--) {
      double x0, y0, x1, y1;
      worldXY(la1, lo0, z, x0, y0);
      worldXY(la0, lo1, z, x1, y1);
      if (x1 - x0 <= W - 260 && y1 - y0 <= H - 120) break;
    }
    mapSetView((la0 + la1) / 2, (lo0 + lo1) / 2, z);
  }
  showPinBar();
  if (fx) lv_obj_invalidate(fx);
}
void mapClearPath() {
  pinStops.clear();
  showPinBar();
  if (fx) lv_obj_invalidate(fx);
}
static void onZoomIn(lv_event_t *) { mapSetView(vLat, vLon, vZ + 1); }
static void onZoomOut(lv_event_t *) { mapSetView(vLat, vLon, vZ - 1); }
static void onCenter(lv_event_t *) { mapRecenter(); }
static void onPause(lv_event_t *) {
  paused = !paused;
  setBtnText(btnPause, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
  if (paused) lv_obj_remove_flag(pausedBadge, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(pausedBadge, LV_OBJ_FLAG_HIDDEN);
}
static lv_obj_t *themeBtn;
static void onThemeBtn(lv_event_t *) { toggleTheme(); }
void mapResetObjects() {
  anims.clear();
  pulses.clear();
  selKey.clear();
  hits.clear();
  dragging = false;
  dragDx = dragDy = 0;
}

void mapMarkDirty() { baseDirty = true; }
void mapTick() {
  // rebuilding the base map moves ~1.5 MB through PSRAM; at most about once a second so the
  // display's own PSRAM reads keep up while nodes/tiles stream in
  if (baseDirty && visible && !dragging && lv_tick_elaps(lastBake) > 1000) bake();
}
void mapShownChanged(bool v) {
  visible = v;
  if (v) {
    bake();
  } else {
    anims.clear();
    pulses.clear();
  }
}
int mapNodesOnMap() { return onMap; }

// ---------------------------------------------------------------- map feed
void mapRenderFeed() {
  int shown = 0;
  const Route *latest = nullptr;
  for (auto &r : M->feed) {
    if (shown >= 5) break;
    if (!passes(r)) continue;
    if (!latest) latest = &r;
    FeedRow &fr = frows[shown++];
    uint32_t c = gcol(r.group);
    lv_label_set_text(fr.tag, r.label.c_str());
    lv_obj_set_style_text_color(fr.tag, lv_color_hex(c), 0);
    lv_label_set_text(fr.ago, ago(r.ts).c_str());
    lv_label_set_text(fr.who, r.who.c_str());
    lv_label_set_text(fr.det, feedDetail(r).c_str());
    lv_obj_set_style_border_color(fr.box, lv_color_hex(r.mine ? T.ring : c), 0);
    lv_obj_remove_flag(fr.box, LV_OBJ_FLAG_HIDDEN);
  }
  for (int i = shown; i < 5; i++) lv_obj_add_flag(frows[i].box, LV_OBJ_FLAG_HIDDEN);
  if (shown) lv_obj_add_flag(feedEmpty, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(feedEmpty, LV_OBJ_FLAG_HIDDEN);
  // the folded line: the latest packet
  if (feedLineTag) {
    if (latest) {
      lv_label_set_text(feedLineTag, latest->label.c_str());
      lv_obj_set_style_text_color(feedLineTag, lv_color_hex(gcol(latest->group)), 0);
      lv_label_set_text(feedLineWho, latest->who.c_str());
      lv_label_set_text(feedLineAgo, ago(latest->ts).c_str());
    } else {
      lv_label_set_text(feedLineTag, "LIVE");
      lv_obj_set_style_text_color(feedLineTag, lv_color_hex(C_ACC), 0);
      lv_label_set_text(feedLineWho, "Waiting for traffic");
      lv_label_set_text(feedLineAgo, "");
    }
  }
}

// ---------------------------------------------------------------- build
static lv_obj_t *toolBtn(const char *txt, lv_event_cb_t cb) {
  lv_obj_t *b = mkBtn(tools, txt, cb, nullptr, C_PANEL, C_TEXT, &lv_font_montserrat_24);
  lv_obj_set_size(b, 52, 52);
  lv_obj_set_style_bg_opa(b, 235, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_radius(b, 12, 0);
  return b;
}

void mapCreate(lv_obj_t *scr) {
  // LVGL aligns draw buffers to LV_DRAW_BUF_ALIGN (64): hand it an aligned pointer so our direct
  // pixel writes and LVGL's view of the canvas agree.
  if (!buf) {
    uint8_t *raw = (uint8_t *)malloc(SCR_W * SCR_H * 2 + 64);
    buf = (uint16_t *)(((uintptr_t)raw + 63) & ~(uintptr_t)63);
  }
  root = mkBox(scr, C_BG);
  lv_obj_set_pos(root, 0, TOP_H);
  lv_obj_set_size(root, SCR_W, MAIN_H);
  lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  canvas = lv_canvas_create(root);
  lv_canvas_set_buffer(canvas, buf, W, H, LV_COLOR_FORMAT_RGB565);
  lv_obj_set_pos(canvas, 0, 0);

  fx = lv_obj_create(root);
  lv_obj_remove_style_all(fx);
  lv_obj_set_pos(fx, 0, 0);
  lv_obj_set_size(fx, W, H);
  lv_obj_add_flag(fx, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(fx, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(fx, fxDraw, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_add_event_cb(fx, fxInput, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(fx, fxInput, LV_EVENT_PRESSING, nullptr);
  lv_obj_add_event_cb(fx, fxInput, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(fx, fxInput, LV_EVENT_PRESS_LOST, nullptr);

  // live mini feed (top-left)
  feedPanel = mkBox(root, C_BG, 225);
  lv_obj_set_pos(feedPanel, 8, 8);
  lv_obj_set_size(feedPanel, 232, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(feedPanel, 10, 0);
  lv_obj_set_style_border_width(feedPanel, 1, 0);
  lv_obj_set_style_border_color(feedPanel, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_pad_all(feedPanel, 6, 0);
  lv_obj_set_style_pad_row(feedPanel, 5, 0);
  lv_obj_set_flex_flow(feedPanel, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(feedPanel, LV_OBJ_FLAG_CLICKABLE);
  feedEmpty = mkLabel(feedPanel, &lv_font_montserrat_14, C_DIM, "Waiting for traffic...\nStreaks appear as packets are heard.");
  lv_obj_set_width(feedEmpty, 216);
  lv_label_set_long_mode(feedEmpty, LV_LABEL_LONG_WRAP);
  for (auto &fr : frows) {
    fr.box = mkBox(feedPanel, C_PANEL);
    lv_obj_set_size(fr.box, 218, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(fr.box, 7, 0);
    lv_obj_set_style_pad_hor(fr.box, 8, 0);
    lv_obj_set_style_pad_ver(fr.box, 5, 0);
    lv_obj_set_style_border_side(fr.box, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(fr.box, 3, 0);
    lv_obj_remove_flag(fr.box, LV_OBJ_FLAG_CLICKABLE);
    fr.tag = mkLabel(fr.box, &lv_font_montserrat_12, C_ACC);
    lv_obj_set_pos(fr.tag, 0, 0);
    fr.ago = mkLabel(fr.box, &lv_font_montserrat_12, C_DIM2);
    lv_obj_align(fr.ago, LV_ALIGN_TOP_RIGHT, 0, 0);
    fr.who = mkLabel(fr.box, &lv_font_montserrat_16, C_TEXT);
    lv_obj_set_pos(fr.who, 0, 15);
    lv_obj_set_width(fr.who, 200);
    oneLine(fr.who);
    fr.det = mkLabel(fr.box, &lv_font_montserrat_12, C_DIM);
    lv_obj_set_pos(fr.det, 0, 35);
    lv_obj_set_width(fr.det, 200);
    oneLine(fr.det);
    lv_obj_add_flag(fr.box, LV_OBJ_FLAG_HIDDEN);
  }
  // pull tab on the panel's top-right edge: folds it down to one line
  feedTab = mkBox(root, C_BG, 225);
  lv_obj_set_pos(feedTab, 8 + 232 - 1, 16);
  lv_obj_set_size(feedTab, 34, 48);
  lv_obj_set_style_radius(feedTab, 8, 0);
  lv_obj_set_style_border_width(feedTab, 1, 0);
  lv_obj_set_style_border_color(feedTab, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_border_side(feedTab, (lv_border_side_t)(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM), 0);
  lv_obj_set_ext_click_area(feedTab, 8);
  lv_obj_add_flag(feedTab, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(feedTab, lv_color_hex(T.listPressed), LV_STATE_PRESSED);
  lv_obj_add_event_cb(feedTab, [](lv_event_t *) { toggleTrafficPanel(); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *tabIcon = mkLabel(feedTab, &lv_font_montserrat_16, C_DIM, LV_SYMBOL_LEFT);
  lv_obj_center(tabIcon);
  // folded: one line with the latest packet; tap it to unfold
  feedLine = mkBox(root, C_BG, 225);
  lv_obj_set_pos(feedLine, 8, 8);
  lv_obj_set_size(feedLine, 232, 40);
  lv_obj_set_style_radius(feedLine, 10, 0);
  lv_obj_set_style_border_width(feedLine, 1, 0);
  lv_obj_set_style_border_color(feedLine, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_pad_hor(feedLine, 10, 0);
  lv_obj_set_style_pad_column(feedLine, 8, 0);
  lv_obj_set_flex_flow(feedLine, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(feedLine, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(feedLine, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(feedLine, lv_color_hex(T.listPressed), LV_STATE_PRESSED);
  lv_obj_add_event_cb(feedLine, [](lv_event_t *) { toggleTrafficPanel(); }, LV_EVENT_CLICKED, nullptr);
  feedLineTag = mkLabel(feedLine, &lv_font_montserrat_12, C_ACC, "LIVE");
  feedLineWho = mkLabel(feedLine, &lv_font_montserrat_14, C_TEXT, "");
  lv_obj_set_flex_grow(feedLineWho, 1);
  oneLine(feedLineWho);
  feedLineAgo = mkLabel(feedLine, &lv_font_montserrat_12, C_DIM2, "");
  mkLabel(feedLine, &lv_font_montserrat_14, C_DIM, LV_SYMBOL_RIGHT);
  for (lv_obj_t *o : {feedLineTag, feedLineWho, feedLineAgo}) lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  showFeed();

  // tools (right edge)
  tools = lv_obj_create(root);
  lv_obj_remove_style_all(tools);
  lv_obj_set_size(tools, 52, LV_SIZE_CONTENT);
  lv_obj_align(tools, LV_ALIGN_TOP_RIGHT, -10, 10);
  lv_obj_set_flex_flow(tools, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tools, 6, 0);
  toolBtn(LV_SYMBOL_PLUS, onZoomIn);
  toolBtn(LV_SYMBOL_MINUS, onZoomOut);
  toolBtn(LV_SYMBOL_GPS, onCenter);
  btnPause = toolBtn(paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE, onPause);
  themeBtn = toolBtn(dayMode() ? "NIGHT" : "DAY", onThemeBtn);
  lv_obj_set_style_text_font(lv_obj_get_child(themeBtn, 0), &lv_font_montserrat_12, 0);

  // legend (bottom-left)
  legend = mkBox(root, C_BG, 225);
  lv_obj_set_size(legend, LV_SIZE_CONTENT, 30);
  lv_obj_align(legend, LV_ALIGN_BOTTOM_LEFT, 8, -8);
  lv_obj_set_style_radius(legend, 8, 0);
  lv_obj_set_style_pad_hor(legend, 10, 0);
  lv_obj_set_style_pad_column(legend, 12, 0);
  lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  const Group lg[] = {G_CHANNEL, G_ADVERT, G_DM, G_TRACE, G_PATH};
  for (Group g : lg) {
    lv_obj_t *sw = mkBox(legend, gcol(g));
    lv_obj_set_size(sw, 10, 10);
    lv_obj_set_style_radius(sw, 2, 0);
    std::string n = groupName(g);
    for (size_t i = 1; i < n.size(); i++) n[i] = (char)tolower(n[i]);
    if (g == G_DM) n = "DM";
    lv_obj_t *l = mkLabel(legend, &lv_font_montserrat_12, C_DIM, n.c_str());
    lv_obj_set_style_margin_left(l, -6, 0);
  }

  attrib = mkLabel(root, &lv_font_montserrat_12, C_DIM2, "Map (c) OpenStreetMap  |  Data: CoreScope");
  lv_obj_align(attrib, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
  mapSourceChanged();

  pinBar = mkBox(root, C_BG, 235);
  lv_obj_set_size(pinBar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_align(pinBar, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_set_style_radius(pinBar, 10, 0);
  lv_obj_set_style_border_width(pinBar, 1, 0);
  lv_obj_set_style_border_color(pinBar, lv_color_hex(0xFFB547), 0);
  lv_obj_set_style_pad_hor(pinBar, 12, 0);
  lv_obj_set_style_pad_ver(pinBar, 6, 0);
  lv_obj_set_style_pad_column(pinBar, 10, 0);
  lv_obj_set_flex_flow(pinBar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(pinBar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  pinLbl = mkLabel(pinBar, &lv_font_montserrat_14, C_TEXT, "");
  lv_obj_t *pc = mkBtn(pinBar, LV_SYMBOL_CLOSE "  Clear", [](lv_event_t *) { mapClearPath(); }, nullptr, C_BTN, C_TEXT, &lv_font_montserrat_14);
  lv_obj_set_height(pc, 34);
  showPinBar();

  pausedBadge = mkLabel(root, &lv_font_montserrat_16, C_STAR, LV_SYMBOL_PAUSE "  PAUSED");
  lv_obj_align(pausedBadge, LV_ALIGN_TOP_MID, 0, 12);
  if (!paused) lv_obj_add_flag(pausedBadge, LV_OBJ_FLAG_HIDDEN);

  // node card
  card = mkBox(root, C_BG, 240);
  lv_obj_set_size(card, 340, LV_SIZE_CONTENT);
  lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 40, -12);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(T.border2), 0);
  lv_obj_set_style_pad_all(card, 12, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, 4, 0);
  cardRole = mkLabel(card, &lv_font_montserrat_12, C_ACC);
  cardName = mkLabel(card, &lv_font_montserrat_24, C_TEXT);
  lv_obj_set_width(cardName, 312);
  oneLine(cardName);
  cardMeta = mkLabel(card, &lv_font_montserrat_12, C_DIM);
  lv_obj_t *row = lv_obj_create(card);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, 316, LV_SIZE_CONTENT);
  lv_obj_set_style_margin_top(row, 6, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 8, 0);
  btnStar = mkBtn(row, LV_SYMBOL_PLUS " My Nodes", onStar, nullptr, C_BTN, C_STAR);
  lv_obj_set_size(btnStar, 140, 44);
  btnWatch = mkBtn(row, "@ Watch", onWatch, nullptr, C_BTN, C_AT);
  lv_obj_set_size(btnWatch, 116, 44);
  lv_obj_t *bx = mkBtn(row, LV_SYMBOL_CLOSE, [](lv_event_t *) { hideCard(); }, nullptr, C_BTN, C_DIM);
  lv_obj_set_size(bx, 44, 44);
  lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);

  static bool timerUp = false;
  if (!timerUp) {
    timerUp = true;
    lv_timer_create(fxTimer, 33, nullptr);
    mapRecenter();
  } else {
    lv_canvas_set_buffer(canvas, buf, W, H, LV_COLOR_FORMAT_RGB565);
    baseDirty = true;
  }
}

}  // namespace ui
