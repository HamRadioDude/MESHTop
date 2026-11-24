#include <map>
// Desktop simulator: runs the real UI + model in demo mode, scripted, and dumps 800x480 frames
// (RGB565 raw) that tools/frames_to_png.py turns into PNGs.
#include "core/aes128.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "app/demo.h"
#include <chrono>
#include "app/radio_bridge.h"
#include "app/platform.h"
#include "core/companion.h"
#include "core/chatlog.h"
#include "core/sha256.h"
#include "core/model.h"
#include <deque>
#include "lvgl.h"
#include "ui/ui.h"
#include "ui/ui_int.h"

static uint32_t vticks = 0;
static uint32_t simTick() { return vticks; }
static int64_t EPOCH0 = 1794870000000LL;  // a fixed evening for repeatable shots (MESHtop Desktop: the real time)
#ifdef MESHTOP_DESKTOP
// MESHtop Desktop (desktop.inc) hooks: real map tiles, the mouse, the local clock
static const uint16_t *(*dtTile)(int z, int x, int y, const std::string &style, bool ask) = nullptr;
static bool (*dtFailed)(int z, int x, int y, const std::string &style) = nullptr;
static int mouseX = 0, mouseY = 0;
static bool mouseDown = false;
static int desktopMain(int argc, char **argv);
static void desktopConfig(int argc, char **argv);  // reads meshtop-desktop.txt and the command line
#endif

alignas(64) static uint16_t fb[800 * 480];
static void flush(lv_display_t *d, const lv_area_t *, uint8_t *) { lv_display_flush_ready(d); }

static const int TOP = 40;
static int tapX = -1, tapY = -1, tapLeft = 0;
static void readTouch(lv_indev_t *, lv_indev_data_t *data) {
#ifdef MESHTOP_DESKTOP
  data->point.x = mouseX;
  data->point.y = mouseY;
  data->state = mouseDown ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  return;
#endif
  if (tapLeft > 0) {
    data->point.x = tapX;
    data->point.y = tapY;
    data->state = LV_INDEV_STATE_PRESSED;
    tapLeft--;
  } else data->state = LV_INDEV_STATE_RELEASED;
}

st::Model model;
st::Demo demo;
st::Companion radio;

// ---- a fake MeshCore companion radio speaking the real frame format
static std::deque<std::vector<uint8_t>> toPanel;
static int64_t simNow();
static void p32(std::vector<uint8_t> &v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }
static void pstr(std::vector<uint8_t> &v, const std::string &s, size_t n) { for (size_t i = 0; i < n; i++) v.push_back(i < s.size() ? (uint8_t)s[i] : 0); }
static std::vector<uint8_t> contactFrame(uint8_t code, uint8_t keyByte, const std::string &name, uint8_t type, int8_t path, double lat, double lon,
                                         uint8_t keyRest = 0) {
  std::vector<uint8_t> f = {code};
  for (int i = 0; i < 32; i++) f.push_back((uint8_t)(i && keyRest ? keyRest + i : keyByte + i));  // keyRest: same first byte, different key
  f.push_back(type); f.push_back(0); f.push_back((uint8_t)path);
  static const uint8_t hops[] = {0xA1, 0xB2, 0xC3, 0xD4};
  for (int i = 0; i < 64; i++) f.push_back(path > 0 && i < (path & 63) && i < 4 ? hops[i] : 0);
  pstr(f, name, 32);
  p32(f, (uint32_t)(simNow() / 1000 - 600)); p32(f, (uint32_t)(int32_t)(lat * 1e6)); p32(f, (uint32_t)(int32_t)(lon * 1e6)); p32(f, 0);
  return f;
}
static std::deque<std::vector<uint8_t>> pendingMsgs;
static bool fakeDropLongTraces = false;  // a stale saved route: the first repeater passes it on, then it's lost
static bool fakeTraceSilent = false;      // nothing comes back at all
static bool fakeTraceSilentDirect = false;  // straight to it gets nothing back; the saved route works
static bool fakeTraceNoReport = false;    // the radio hears it come back but never sends its trace report
static bool fakeTraceNoAir = false;       // only the trace report, no raw receive log
struct FakeSlot { std::string name; uint8_t sec[16]; };
static FakeSlot slots[8];
static void fakeInitSlots() {
  slots[0].name = "Public"; memcpy(slots[0].sec, st::PUBLIC_CHANNEL_KEY, 16);
  slots[1].name = "#chicago"; st::hashtagKey("#chicago", slots[1].sec);
  slots[2].name = "Desk Crew"; for (int i = 0; i < 16; i++) slots[2].sec[i] = (uint8_t)(0x40 + i * 7);
}
static void fakeRadioRx(const std::vector<uint8_t> &c) {
  uint8_t cmd = c[0];
  std::vector<uint8_t> r;
  switch (cmd) {
    case 22: r = {13, 9, 175, 8}; p32(r, 123456); pstr(r, "16 Nov 2026", 12); pstr(r, "Seeed Xiao nRF52840 + Wio-SX1262", 40); pstr(r, "v1.10.0", 20); r.push_back(0); r.push_back(0); break;
    case 1: {
      r = {5, 1, 17, 22}; for (int i = 0; i < 32; i++) r.push_back((uint8_t)(0xD0 + i));
      p32(r, 0); p32(r, 0); r.push_back(0); r.push_back(0); r.push_back(0); r.push_back(0);
      p32(r, 910525); p32(r, 62500); r.push_back(7); r.push_back(5);
      for (char ch : std::string("DudeXIAO")) r.push_back((uint8_t)ch);
      break;
    }
    case 4: {
      std::vector<uint8_t> st = {2}; p32(st, 6); toPanel.push_back(st);
      auto fav = contactFrame(3, 0x22, "KD9XYZ Mobile", 1, 2, 41.8, -87.6); fav[34] = 1; toPanel.push_back(fav);
      toPanel.push_back(contactFrame(3, 0x11, "Ridge-RPT", 2, 0, 41.9, -87.7));
      toPanel.push_back(contactFrame(3, 0x33, "N0CALL Base", 1, -1, 41.95, -87.8));
      toPanel.push_back(contactFrame(3, 0x44, "Tower-RPT Naperville", 2, 1, 41.75, -88.15));
      toPanel.push_back(contactFrame(3, 0x55, "Chicagoland Room", 3, 2, 41.88, -87.63));
      toPanel.push_back(contactFrame(3, 0x66, "Sarah HT", 1, 3, 0, 0));
      r = {4}; p32(r, 0);
      break;
    }
    case 31:
      if (c[1] < 8) { r = {18, c[1]}; pstr(r, slots[c[1]].name, 32); for (int i = 0; i < 16; i++) r.push_back(slots[c[1]].sec[i]); }
      else r = {1, 2};
      break;
    case 32:
      if (c.size() >= 50 && c[1] < 8) { slots[c[1]].name = std::string((const char *)&c[2], strnlen((const char *)&c[2], 32)); memcpy(slots[c[1]].sec, &c[34], 16); r = {0}; }
      else r = {1, 6};
      break;
    case 20: r = {12}; r.push_back(4012 & 255); r.push_back(4012 >> 8); p32(r, 120); p32(r, 2048); break;
    case 10:
      if (pendingMsgs.empty()) r = {10};
      else { r = pendingMsgs.front(); pendingMsgs.pop_front(); }
      break;
    case 3: r = {0}; break;
    case 2:
      if (c[1] == 1) {  // admin command (CLI text): sent, then the repeater's answer arrives as a type-1 message
        r = {6, 0}; p32(r, 0x5151); p32(r, 3000); toPanel.push_back(r);
        std::string cmdTxt((const char *)&c[13], c.size() - 13), ans = cmdTxt == "ver" ? "v1.18.0 (Build: 14-Nov-2026)" : cmdTxt == "clock" ? "06:12 - 19/11/2026 UTC" : "OK";
        std::vector<uint8_t> m = {16, 24, 0, 0}; for (int i = 0; i < 6; i++) m.push_back(c[7 + i]); m.push_back(1); m.push_back(1);
        p32(m, (uint32_t)(simNow() / 1000)); for (char ch : ans) m.push_back((uint8_t)ch);
        pendingMsgs.push_back(m);
        r = {0x83};
        break;
      }
      r = {6, 0}; p32(r, 0xABCD1234); p32(r, 4000); toPanel.push_back(r); r = {0x82}; p32(r, 0xABCD1234); p32(r, 1800); break;
    case 50: {  // binary request (neighbours): sent, then the list (Tower-RPT, a CoreScope repeater, an unknown one)
      r = {6, 0}; p32(r, 0x6060); p32(r, 4000); toPanel.push_back(r);
      r = {0x8C, 0}; p32(r, 0x6060); r.push_back(5); r.push_back(0); r.push_back(3); r.push_back(0);
      struct N { uint8_t k; uint32_t ago; int8_t snr4; } ns[] = {{0x44, 40, 38}, {0x90, 300, 10}, {0xEE, 5400, -22}};
      for (auto &n : ns) { for (int i = 0; i < 6; i++) r.push_back((uint8_t)(n.k + i)); p32(r, n.ago); r.push_back((uint8_t)n.snr4); }
      break;
    }
    case 52: {  // path discovery: sent, then the route there (Ridge, Tower) and back (Tower, Ridge)
      r = {6, 1}; p32(r, 0x5252); p32(r, 6000); toPanel.push_back(r);
      r = {0x8D, 0}; for (int i = 0; i < 6; i++) r.push_back(c[2 + i]);
      r.push_back(2); r.push_back(0x11); r.push_back(0x44); r.push_back(2); r.push_back(0x44); r.push_back(0x11);
      break;
    }
    case 26: case 27: {
      std::vector<uint8_t> sent = {6, 0}; p32(sent, 77); p32(sent, 3000); toPanel.push_back(sent);
      if (cmd == 26) { r = {0x85, 0}; for (int i = 0; i < 6; i++) r.push_back(c[1 + i]); p32(r, 0); r.push_back(0); r.push_back(1); }
      else {
        r = {0x87, 0}; for (int i = 0; i < 6; i++) r.push_back(c[1 + i]);
        auto p16 = [&](uint16_t v) { r.push_back(v & 255); r.push_back(v >> 8); };
        p16(4105); p16(0); p16((uint16_t)(int16_t)-112); p16((uint16_t)(int16_t)-78); p32(r, 18234); p32(r, 9120); p32(r, 3800); p32(r, 3 * 86400 + 5 * 3600 + 12 * 60);
        p32(r, 4000); p32(r, 5120); p32(r, 9000); p32(r, 9234); p16(2); p16((uint16_t)(int16_t)(6.5 * 4)); p16(10); p16(40);
      }
      break;
    }
    case 36: {  // trace: RESP_SENT; the radio hears repeaters pass it on (raw receive log), then its own trace report
      uint32_t tag = c[1] | c[2] << 8 | c[3] << 16 | (uint32_t)c[4] << 24;
      std::vector<uint8_t> sent = {6, 0}; p32(sent, tag); p32(sent, 2500); toPanel.push_back(sent);
      uint8_t flags = c[9], sz = flags & 3;
      size_t plen = c.size() - 10, hops = plen >> sz;
      static const int8_t snr[] = {38, 22, -14, 6, 30, 12};
      auto heard = [&](size_t done) {  // the packet as your radio hears it after `done` repeaters passed it on
        std::vector<uint8_t> f = {0x88, (uint8_t)(int8_t)26, (uint8_t)(int8_t)-70, (uint8_t)((0x09 << 2) | 0x02), (uint8_t)done};
        for (size_t i = 0; i < done; i++) f.push_back((uint8_t)snr[i % 6]);
        p32(f, tag); p32(f, 0); f.push_back(flags);
        for (size_t i = 0; i < plen; i++) f.push_back(c[10 + i]);
        return f;
      };
      if (fakeTraceSilent || (hops == 1 && fakeTraceSilentDirect)) return;
      if (hops > 1 && !fakeTraceNoAir) toPanel.push_back(heard(1));  // the first repeater is within earshot
      if (fakeDropLongTraces && hops > 1) return;
      if (!fakeTraceNoAir) toPanel.push_back(heard(hops));  // the last hop passes it back to you
      if (fakeTraceNoReport) return;
      r = {0x89, 0, (uint8_t)plen, flags}; p32(r, tag); p32(r, 0);
      for (size_t i = 0; i < plen; i++) r.push_back(c[10 + i]);
      for (size_t i = 0; i < hops; i++) r.push_back((uint8_t)snr[i % 6]);
      r.push_back((uint8_t)(int8_t)26);
      break;
    }
    case 56: {  // stats: core, radio, packets
      auto p16 = [&](uint16_t v) { r.push_back(v & 255); r.push_back(v >> 8); };
      if (c[1] == 0) { r = {24, 0}; p16(4012); p32(r, 3 * 86400 + 4 * 3600 + 17 * 60); p16(0); r.push_back(0); }
      else if (c[1] == 1) { r = {24, 1}; p16((uint16_t)(int16_t)-112); r.push_back((uint8_t)(int8_t)-78); r.push_back((uint8_t)(int8_t)26); p32(r, 1234); p32(r, 21000); }
      else { r = {24, 2}; for (uint32_t v : {18234u, 9120u, 4000u, 5120u, 9000u, 9234u, 40u}) p32(r, v); }
      break;
    }
    case 55: {  // discovery request: OK, then three repeaters in range answer
      r = {0};
      toPanel.push_back(r);
      struct A { uint8_t key; int8_t they, we, rssi; } ans[] = {{0x11, 34, 26, -71}, {0x44, -10, -6, -104}, {0x9A, 12, 18, -88}};
      for (auto &a : ans) {
        std::vector<uint8_t> f = {0x8E, (uint8_t)a.we, (uint8_t)a.rssi, 0, (uint8_t)(0x90 | 2), (uint8_t)a.they, c[3], c[4], c[5], c[6]};
        for (int i = 0; i < 8; i++) f.push_back((uint8_t)(a.key + i));
        toPanel.push_back(f);
      }
      return;
    }
    case 6: case 7: case 8: case 9: case 11: case 12: case 13: case 14: case 15: case 16: case 37: case 38: r = {0}; break;
    default: r = {1, 1};
  }
  toPanel.push_back(r);
}
static void queueChannelMsg(int idx, const std::string &text, int8_t snr4, uint8_t hops) {
  std::vector<uint8_t> f = {17, (uint8_t)snr4, 0, 0, (uint8_t)idx, hops, 0}; p32(f, (uint32_t)(simNow() / 1000)); for (char ch : text) f.push_back((uint8_t)ch);
  pendingMsgs.push_back(f);
}
static void queueChannelMsgTs(int idx, const std::string &text, int8_t snr4, uint8_t hops, uint32_t ts) {
  std::vector<uint8_t> f = {17, (uint8_t)snr4, 0, 0, (uint8_t)idx, hops, 0}; p32(f, ts); for (char ch : text) f.push_back((uint8_t)ch);
  pendingMsgs.push_back(f);
}
static void queueDm(uint8_t keyByte, const std::string &text) {
  std::vector<uint8_t> f = {16, (uint8_t)(int8_t)22, 0, 0}; for (int i = 0; i < 6; i++) f.push_back((uint8_t)(keyByte + i));
  f.push_back(2); f.push_back(0); p32(f, (uint32_t)(simNow() / 1000)); for (char ch : text) f.push_back((uint8_t)ch);
  pendingMsgs.push_back(f);
}

class SimPlatform : public st::Platform {
 public:
  int64_t nowMs() override { return EPOCH0 + vticks; }
  bool timeValid() override { return true; }
  void localClock(int64_t ms, int &h, int &m, int &s) override {
#ifdef MESHTOP_DESKTOP
    time_t t = (time_t)(ms / 1000);  // the computer's own time zone
    struct tm g;
    localtime_r(&t, &g);
#else
    time_t t = (time_t)(ms / 1000) - 6 * 3600;  // CST
    struct tm g;
    gmtime_r(&t, &g);
#endif
    h = g.tm_hour;
    m = g.tm_min;
    s = g.tm_sec;
  }
  // Tiles only when a test asks (simTiles): a coloured pattern up to zoom simMaxZ, nothing above it
  bool simTiles = false;
  int simMaxZ = 12;
  std::map<std::string, std::vector<uint16_t>> simTileStore;
  const uint16_t *simTile(int z, int x, int y) {
    if (!simTiles || z > simMaxZ) return nullptr;
    std::string k = std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y);
    auto &v = simTileStore[k];
    if (v.empty()) {
      v.resize(256 * 256);
      uint16_t base = (uint16_t)((((x * 53) & 31) << 11) | (((y * 37) & 63) << 5) | 12);
      for (int i = 0; i < 256 * 256; i++) {
        int px = i % 256, py = i / 256;
        v[i] = (px % 32 == 0 || py % 32 == 0) ? 0xFFFF : base;
      }
    }
    return v.data();
  }
#ifdef MESHTOP_DESKTOP
  const uint16_t *tile(int z, int x, int y, const std::string &st) override { return dtTile ? dtTile(z, x, y, st, true) : nullptr; }
  const uint16_t *tilePeek(int z, int x, int y, const std::string &st) override { return dtTile ? dtTile(z, x, y, st, false) : nullptr; }
  bool tileFailed(int z, int x, int y, const std::string &st) override { return dtFailed && dtFailed(z, x, y, st); }
#else
  const uint16_t *tile(int z, int x, int y, const std::string &) override { return simTile(z, x, y); }
  const uint16_t *tilePeek(int z, int x, int y, const std::string &) override { return simTile(z, x, y); }
  bool tileFailed(int z, int, int, const std::string &) override { return simTiles && z > simMaxZ; }
#endif
  void beep(int) override {}
  void setBacklight(int pct) override { backlight = pct; }
  void requestChannels() override {
    if (model.cfg.radioOnly()) return;
    demo.channels(model, nowMs());
    ui::channelsChanged();
  }
  void requestHistory(const std::string &ch, const std::string &) override {
    demo.history(model, ch, nowMs());
    ui::historyChanged(ch);
  }
  void requestMyNodes() override {
    model.resolveMyNodes();
    if (model.cfg.radioOnly()) st::radioOnlyCards(model);
    else demo.myCards(model, nowMs());
    ui::myNodesChanged();
  }
  // the same steps as the panel's applySettings for a data source switch
  void setDimMode(const std::string &mode) override { model.cfg.dimMode = mode; }
  void setDataSource(const std::string &mode) override {
    bool was = model.cfg.radioOnly();
    model.cfg.useCoreScope = mode != "radio";
    model.cfg.radioOn = radioOn = mode != "corescope";
    if (model.cfg.radioOnly() != was) {
      st::clearForSourceSwitch(model);
      if (model.cfg.radioOnly()) {
        st::bridgeContacts(model, ::radio, true);
        if (::radio.state == st::R_READY) model.markDirectoryReady();
      } else {
        demo.seed(model, nowMs());
        model.markDirectoryReady();
      }
    }
    ui::configChanged();
  }
  void requestSearch(const std::string &) override {}
  void saveLists() override {}
  st::ConnState conn() override { return st::CONN_LIVE; }
  std::string connMsg() override { return model.cfg.radioOnly() ? "Radio only: CoreScope is off" : "Connected to CoreScope live feed"; }
  std::string settingsUrl() override { return "http://scopething.local"; }
  std::string netInfo() override { return "Wi-Fi HomeNet  -58 dBm  192.168.1.40"; }
  std::string cardInfo() override { return "29.1 GB free  |  map pack chicago (zoom 6-14)  |  48 tiles saved since start"; }
  std::string version() override { return "v3.1.4"; }
  st::Companion *radio() override { return &::radio; }
  bool radioEnabled() override { return radioOn; }
  void radioEnable(bool on) override { radioOn = on; }
  void wifiScan() override {
    ui::wifiScanResult({{"HomeNet", -52, true}, {"HomeNet-5G", -58, true}, {"NETGEAR42", -71, true}, {"xfinitywifi", -78, false}, {"Neighbor Guest", -84, true}});
  }
  void wifiConnect(const std::string &ssid, const std::string &) override { ui::wifiStatus("Connecting to " + ssid + "...", false, true); }
  bool radioOn = true;
  bool setupDone = true;
  bool radioSetupDone() override { return setupDone; }
  void setRadioSetupDone(bool d) override { setupDone = d; }
  int backlight = 100;
};
static SimPlatform plat;
static int64_t simNow() { return plat.nowMs(); }

static void run(uint32_t ms) {
  static int64_t nextPkt = 0;
  for (uint32_t t = 0; t < ms; t += 5) {
    vticks += 5;
    if ((int64_t)vticks >= nextPkt) nextPkt = vticks + (model.cfg.radioOnly() ? 500 : demo.tick(model, plat.nowMs()));  // CoreScope-style traffic only while it's on
    if (vticks % 20 == 0 && !toPanel.empty()) {  // BLE notifications arrive a little later
      auto f = toPanel.front();
      toPanel.pop_front();
      radio.frame(f.data(), f.size());
    }
    if (vticks % 1000 == 0) radio.tick();
    lv_timer_handler();
  }
}
static void tap(int x, int y) {
  tapX = x;
  tapY = y;
  tapLeft = 6;
  run(200);
}
static void shot(const char *name) {
  lv_obj_invalidate(lv_screen_active());
  lv_refr_now(nullptr);
  std::string path = std::string("sim/out/") + name + ".rgb565";
  FILE *f = fopen(path.c_str(), "wb");
  fwrite(fb, 2, 800 * 480, f);
  fclose(f);
  printf("shot %s (backlight %d)\n", name, plat.backlight);
}


// ---- test helper: build a MeshCore group-text packet the way the firmware does (AES-128-ECB + HMAC)
static void aesEncBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  using namespace st::aesd;
  const uint8_t *S = sbox();
  uint8_t rk[176];
  memcpy(rk, key, 16);
  uint8_t rcon = 1;
  for (int i = 16; i < 176; i += 4) {
    uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
    if (i % 16 == 0) {
      uint8_t u = t[0];
      t[0] = (uint8_t)(S[t[1]] ^ rcon); t[1] = S[t[2]]; t[2] = S[t[3]]; t[3] = S[u];
      rcon = xt(rcon);
    }
    for (int k = 0; k < 4; k++) rk[i + k] = rk[i - 16 + k] ^ t[k];
  }
  uint8_t s[16];
  for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
  for (int round = 1; round <= 10; round++) {
    uint8_t t[16];
    for (int i = 0; i < 16; i++) t[i] = S[s[i]];
    for (int c = 0; c < 4; c++)
      for (int r = 0; r < 4; r++) s[c * 4 + r] = t[((c + r) % 4) * 4 + r];
    if (round < 10)
      for (int c = 0; c < 4; c++) {
        uint8_t a0 = s[c * 4], a1 = s[c * 4 + 1], a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
        s[c * 4] = mul(a0, 2) ^ mul(a1, 3) ^ a2 ^ a3;
        s[c * 4 + 1] = a0 ^ mul(a1, 2) ^ mul(a2, 3) ^ a3;
        s[c * 4 + 2] = a0 ^ a1 ^ mul(a2, 2) ^ mul(a3, 3);
        s[c * 4 + 3] = mul(a0, 3) ^ a1 ^ a2 ^ mul(a3, 2);
      }
    for (int i = 0; i < 16; i++) s[i] ^= rk[round * 16 + i];
  }
  memcpy(out, s, 16);
}
static std::vector<uint8_t> rxLogFrame(const uint8_t key[16], uint32_t ts, const std::string &text, std::vector<uint8_t> path, int8_t snr4, int8_t rssi) {
  std::vector<uint8_t> plain(5 + text.size() + 1, 0);
  memcpy(plain.data(), &ts, 4);
  memcpy(&plain[5], text.data(), text.size());
  while (plain.size() % 16) plain.push_back(0);
  std::vector<uint8_t> ct(plain.size());
  for (size_t o = 0; o < plain.size(); o += 16) aesEncBlock(key, &plain[o], &ct[o]);
  uint8_t h[32], k32[32] = {0};
  memcpy(k32, key, 16);
  st::hmacSha256(k32, 32, ct.data(), ct.size(), h);
  uint8_t ch[32];
  st::sha256(key, 16, ch);
  std::vector<uint8_t> f = {0x88, (uint8_t)snr4, (uint8_t)rssi, (uint8_t)((0x05 << 2) | 0x01), (uint8_t)path.size()};
  f.insert(f.end(), path.begin(), path.end());
  f.push_back(ch[0]); f.push_back(h[0]); f.push_back(h[1]);
  f.insert(f.end(), ct.begin(), ct.end());
  return f;
}

int main(int argc, char **argv) {
  int fails = 0;
  auto check = [&](bool ok, const char *what) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); fails += !ok; };
  std::string only = argc > 1 ? argv[1] : "";
#ifdef MESHTOP_DESKTOP
  EPOCH0 = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  desktopConfig(argc, argv);
#endif
  lv_init();
  lv_tick_set_cb(simTick);
  lv_display_t *disp = lv_display_create(800, 480);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(disp, fb, nullptr, sizeof fb, LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(disp, flush);
  lv_indev_t *in = lv_indev_create();
  lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(in, readTouch);

  model.cfg.demo = true;
  model.cfg.mentionNames = "";
  model.nowMs = [] { return plat.nowMs(); };
  model.onRoute = [](const st::Route &r) { ui::onRoute(r); };
  model.onAlert = [](const st::Alert &a) { ui::onAlert(a); };
  model.onChatMsg = [](const std::string &c, const st::ChatMsg &m) { ui::onChatMsg(c, m); };
  demo.seed(model, plat.nowMs());
  // a few My Nodes + a watched name
  model.setMyNode(model.nodes[0].key, model.nodes[0].name, true);
  model.setMyNode(model.nodes[3].key, model.nodes[3].name, true);
  model.setMyNode(model.nodes[7].key, model.nodes[7].name, true);
  model.setMyNode(model.nodes[14].key, model.nodes[14].name, true);
  model.setMention(model.nodes[0].key, model.nodes[0].name, true);
  demo.channels(model, plat.nowMs());
  demo.myCards(model, plat.nowMs());

  radio.nowMs = [] { return plat.nowMs(); };
  radio.clockValid = [] { return true; };
  radio.write = [](const std::vector<uint8_t> &f) { fakeRadioRx(f); };
  radio.onChange = [] {
    if (model.cfg.radioOnly() && st::bridgeContacts(model, radio)) model.resolveMyNodes();
    ui::radioChanged();
  };
  radio.onHeard = [](const st::HeardPkt &h) {
    if (model.cfg.radioOnly()) st::bridgeHeard(model, radio, h);
  };
  radio.onMessage = [](const st::Conv &c, const st::RMsg &m) { ui::onRadioMessage(c, m); };
  radio.onResult = [](bool ok, const std::string &t) { ui::radioResult(ok, t); };
  fakeInitSlots();
  radio.devName = "MeshCore-DudeXIAO";
  queueChannelMsg(0, "KD9XYZ Mobile: Anyone on the mesh tonight?", 26, 2);
  queueChannelMsg(0, "N0CALL Base: Loud and clear from Naperville", 18, 3);
  queueChannelMsg(1, "Ridge-RPT: Repeater maintenance Saturday 9am", 30, 1);
  ui::begin(&model, &plat);
  radio.setState(st::R_CONNECTING, "Connecting to MeshCore-DudeXIAO...");
  radio.connected();
#ifdef MESHTOP_DESKTOP
  return desktopMain(argc, argv);
#endif
  run(9000);
  shot("01-map");
  ui::showScreenByName("feed");
  run(600);
  shot("02-feed");
  ui::showScreenByName("chan");
  run(800);
  shot("03-channels");
  ui::showScreenByName("mine");
  run(600);
  shot("04-mynodes");
  ui::showScreenByName("status");
  run(1200);
  shot("05-status");
  ui::openFind("ri");
  run(800);
  shot("06-find");
  ui::showScreenByName("map");
  run(2500);
  // an @mention alert
  {
    st::PacketIn p;
    p.hash = "mention1";
    p.typeName = "GRP_TXT";
    p.channel = "Public";
    p.sender = "Demo-9";
    p.text = "@[" + model.nodes[0].name + "] are you hearing the new Tower repeater?";
    p.observerId = model.observers[0].id;
    p.observerName = model.observers[0].name;
    p.pubKey = model.nodes[20].key;
    p.hops = {model.nodes[2].key.substr(0, 4)};
    p.resolved = {model.nodes[2].key};
    model.handlePacket(p);
  }
  run(700);
  shot("07-alert");
  // tap a node on the map for the node card
  run(9500);
  // the live traffic panel folds down to one line (and stays folded across restarts), then back
  ui::toggleTrafficPanel();
  run(1200);
  check(ui::trafficPanelFolded() && model.cfg.feedFolded, "traffic panel folds to one line");
  shot("08-map-traffic-folded");
  ui::toggleTrafficPanel();
  run(400);
  check(!ui::trafficPanelFolded(), "traffic panel unfolds");
  shot("08b-map-traffic-open");
  // untouched for the idle time: back to the map, dimmed ("always" here; the default dims at night)
  {
    std::string dimWas = model.cfg.dimMode;
    model.cfg.dimMode = "always";
    ui::showScreenByName("feed");
    run(1500);
    ui::goIdleNow();
    run(600);
    bool dimmed = ui::isIdle() && plat.backlight < 100;
    tap(560, 300);
    run(600);
    check(dimmed && !ui::isIdle() && plat.backlight == 100, "idle goes back to the map and dims; a touch brings it back");
    model.cfg.dimMode = dimWas;
    run(300);
  }
  ui::openAlerts();
  run(400);
  shot("09-alerts");
  ui::showSetup(false, nullptr);
  ui::showScreenByName("mesh");
  run(500);
  radio.sendText("c:0", "ScopeThing panel checking in, 73!");
  queueDm(0x22, "Nice, the panel works! Hearing you 2 hops out");
  radio.syncNow();
  run(10500);  // let the alert banner time out
  shot("15-mesh-chats");
  ui::showRadioSection("people");
  run(400);
  radio.requestStatus(radio.contacts[1].key);
  // select the repeater via the list: tap its row (second row)
  tap(96 + 150, TOP + 50 + 58 + 30);
  run(1500);
  shot("16-mesh-people");
  ui::showRadioSection("channels");
  run(500);
  shot("17-mesh-channels");
  tap(96 + 150, TOP + 40 + 62 * 2 + 31);
  run(500);
  shot("18-mesh-private-channel");
  ui::showRadioSection("radio");
  run(500);
  shot("19-mesh-myradio");
  radio.addHashtagChannel("#emcomm");
  run(600);
  shot("20-mesh-toast");
  ui::showRadioSection("wizard");
  run(400);
  shot("21-wizard-start");
  tap(800 - 30 - 90, 480 - 16 - 26);
  tap(800 - 30 - 90, 480 - 16 - 26);
  run(400);
  shot("22-wizard-region");
  ui::showRadioSection("chats");
  ui::showScreenByName("map");
  run(10);
  ui::showScreenByName("map");
  model.cfg.theme = "day";
  ui::themeChanged();
  run(6000);
  shot("11-day-map");
  ui::showScreenByName("mine");
  run(600);
  shot("12-day-mynodes");
  ui::showScreenByName("chan");
  run(800);
  shot("13-day-channels");
  ui::showScreenByName("feed");
  run(600);
  shot("14-day-feed");
  model.cfg.theme = "dark";
  ui::themeChanged();
  ui::showSetup(true, "ScopeThing-Setup");
  ui::wifiStatus("Couldn't connect to HomeNet: wrong password, or the router refused the panel. Pick it below and retype the password.", false, false);
  run(300);
  shot("10-wifi-setup");
  // ---- protocol self-check against the fake radio
  check(radio.selfName == "DudeXIAO" && std::abs(radio.freqMHz - 910.525) < 1e-6 && radio.sf == 7, "self info parsed");
  check(radio.contacts.size() == 6 && radio.contacts[0].fav(), "contacts + favourite flag parsed");
  st::RChannel *c0 = radio.channelByIdx(0), *c1 = radio.channelByIdx(1), *c2 = radio.channelByIdx(2), *c3 = radio.channelByIdx(3);
  check(c0 && c0->kind == st::CK_PUBLIC, "Public channel recognised by its key");
  check(c1 && c1->kind == st::CK_HASHTAG, "#chicago recognised as hashtag");
  check(c2 && c2->kind == st::CK_PRIVATE, "Desk Crew recognised as private");
  uint8_t k[16]; st::hashtagKey("#emcomm", k);
  check(c3 && c3->name == "#emcomm" && !memcmp(slots[3].sec, k, 16), "join #emcomm wrote the hashtag key to slot 3");
  st::Conv *pub = radio.conv("c:0");
  check(pub && pub->msgs.size() >= 3 && pub->msgs[0].who == "KD9XYZ Mobile", "channel message sender split from text");
  bool delivered = false;
  for (auto &cv : radio.convs) for (auto &m : cv.msgs) delivered |= m.mine && m.status == st::MS_DELIVERED;
  bool chSent = false;
  for (auto &m : pub->msgs) chSent |= m.mine && m.status == st::MS_SENT;
  check(chSent, "channel message acknowledged by radio");
  {  // "heard by": repeats of our own channel message, decrypted from the raw-receive log
    st::RMsg *mine = nullptr;
    for (auto &m : pub->msgs) if (m.mine && m.text == "ScopeThing panel checking in, 73!") mine = &m;
    const st::RChannel *pc = radio.channelByIdx(0);
    bool ok = mine && pc;
    if (ok) {
      std::string full = radio.selfName + ": " + mine->text;
      auto f1 = rxLogFrame(pc->secret, mine->sentTs, full, {0xAB, 0xCD}, 26, -71);
      radio.frame(f1.data(), f1.size());
      radio.frame(f1.data(), f1.size());  // same repeater heard twice: counted once
      auto f2 = rxLogFrame(pc->secret, mine->sentTs, full, {0xAB, 0xEF, 0x12}, -8, -101);
      radio.frame(f2.data(), f2.size());
      auto f3 = rxLogFrame(pc->secret, mine->sentTs + 5, full, {0x77}, 10, -60);  // different message: ignored
      radio.frame(f3.data(), f3.size());
      uint8_t wrong[16] = {1, 2, 3};
      auto f4 = rxLogFrame(wrong, mine->sentTs, full, {0x55}, 10, -60);  // another channel's key: ignored
      radio.frame(f4.data(), f4.size());
      ok = mine->heard.size() == 2 && mine->heard[0].hop == "cd" && mine->heard[1].hop == "12" &&
           std::abs(mine->heard[0].snr - 6.5f) < 0.01f && mine->heard[1].hops == 3;
      printf("  heard by: %d repeaters", (int)mine->heard.size());
      for (auto &r : mine->heard) printf(" [%s %.1f dB %d dBm, %d hops]", r.hop.c_str(), r.snr, r.rssi, r.hops);
      printf("\n");
    }
    check(ok, "heard-by repeats decoded, de-duplicated, other messages and channels ignored");
    if (ok) {
      ui::showScreenByName("mesh");
      size_t mi = 0;
      for (size_t k = 0; k < pub->msgs.size(); k++) if (&pub->msgs[k] == mine) mi = k;
      ui::showHeardFor(pub->id, mi);
      run(600);
      shot("23-heard-by");
    }
  }
  {  // "Leave messages for my phone": the inbox is left alone; channel messages come from what the radio hears.
     // Saved chats: every message is handed over for saving, the line format round-trips, and restore merges.
    const st::RChannel *pc = radio.channelByIdx(0);
    std::vector<st::ChatLine> saved;
    radio.onStore = [&](const st::Conv &c, const st::RMsg &m) {
      st::ChatLine l;
      l.isChannel = c.isChannel; l.chanName = c.isChannel ? c.title : ""; l.dmKey = c.isChannel ? "" : c.key; l.title = c.title;
      l.ts = m.ts; l.mine = m.mine; l.who = m.who; l.hops = m.hops; l.hasSnr = m.hasSnr; l.snr = m.snr; l.text = m.text;
      st::ChatLine back;
      if (st::chatLineDecode(st::chatLineEncode(l), back) && back.text == l.text && back.who == l.who && back.ts == l.ts) saved.push_back(back);
    };
    radio.leaveForPhone = true;
    uint32_t ts = (uint32_t)(simNow() / 1000) - 8;
    queueChannelMsgTs(0, "Ann Mobile: left for the phone", 20, 1, ts);  // waiting in the radio's inbox
    radio.syncNow();
    run(1500);
    auto has = [&](const std::string &t) {
      for (auto &m : pub->msgs) if (m.text == t) return true;
      return false;
    };
    bool inboxLeft = !has("left for the phone") && !pendingMsgs.empty();
    auto f = rxLogFrame(pc->secret, ts + 2, "Bob Base: heard over the air\twith a tab", {0x44}, 18, -90);
    radio.frame(f.data(), f.size());
    radio.frame(f.data(), f.size());  // a second copy: not shown twice
    int copies = 0;
    for (auto &m : pub->msgs) copies += m.text == "heard over the air\twith a tab";
    check(inboxLeft, "phone mode: messages waiting in the radio are left for the phone app");
    check(copies == 1, "phone mode: a channel message heard over the air is shown once");
    bool savedIt = false;
    for (auto &l : saved) savedIt |= l.text == "heard over the air\twith a tab" && l.who == "Bob Base" && l.chanName == pc->name;
    check(savedIt, "saved chats: a received message is handed over for saving and its line reads back the same");
    st::RMsg old;
    old.who = "Cara HT";
    old.text = "from yesterday";
    old.ts = (int64_t)(ts - 86400) * 1000;
    int unreadBefore = pub->unread;
    radio.restoreMsg(true, pc->name, "", old);
    radio.restoreMsg(true, pc->name, "", old);  // twice: kept once
    int n = 0;
    for (auto &m : pub->msgs) n += m.text == "from yesterday";
    check(n == 1 && pub->unread == unreadBefore && pub->msgs.front().ts <= old.ts, "saved chats: put back once, in time order, not counted as unread");
    queueChannelMsgTs(0, "Bob Base: heard over the air\twith a tab", 18, 1, ts + 2);  // the same one, still in the inbox
    radio.onStore = nullptr;
    radio.leaveForPhone = false;
    radio.syncNow();  // back to normal: the waiting message is collected
    run(1500);
    copies = 0;
    for (auto &m : pub->msgs) copies += m.text == "heard over the air\twith a tab";
    check(has("left for the phone") && copies == 1, "phone mode off: waiting messages are collected, ones already shown aren't doubled");
  }
  {  // routes of someone else's channel message: copies logged before and after the radio hands it over
    const st::RChannel *pc = radio.channelByIdx(0);
    uint32_t ts = (uint32_t)(simNow() / 1000) - 20;
    std::string full = "Sarah HT: Anyone hear the new Tower repeater?";
    auto a = rxLogFrame(pc->secret, ts, full, {0x44, 0x11}, 22, -88);  // via Tower-RPT then Ridge-RPT
    radio.frame(a.data(), a.size());
    auto b = rxLogFrame(pc->secret, ts, full, {}, -26, -112);  // heard straight from the sender
    radio.frame(b.data(), b.size());
    queueChannelMsgTs(0, full, 22, 2, ts);
    radio.syncNow();
    run(1500);
    auto c2 = rxLogFrame(pc->secret, ts, full, {0x44, 0x33, 0x11}, 4, -97);  // a later copy, the long way round
    radio.frame(c2.data(), c2.size());
    radio.frame(a.data(), a.size());  // the same route again: not listed twice
    auto other = rxLogFrame(pc->secret, ts + 1, "Sarah HT: something else", {0x55}, 4, -97);  // another message: not attached
    radio.frame(other.data(), other.size());
    st::RMsg *m = nullptr;
    size_t mi = 0;
    for (size_t k = 0; k < pub->msgs.size(); k++)
      if (!pub->msgs[k].mine && pub->msgs[k].text == "Anyone hear the new Tower repeater?") { m = &pub->msgs[k]; mi = k; }
    bool ok = m && m->paths.size() == 3 && m->paths[0].hops.size() == 2 && m->paths[0].hops[0] == "44" && m->paths[0].hops[1] == "11" &&
              m->paths[1].hops.empty() && m->paths[2].hops.size() == 3 && !m->paths[0].direct;
    if (m) {
      printf("  channel routes: %d copies", (int)m->paths.size());
      for (auto &p : m->paths) { printf(" ["); for (auto &h : p.hops) printf("%s ", h.c_str()); printf("%.1f dB]", p.snr); }
      printf("\n");
    }
    check(ok, "routes of someone else's channel message collected from copies heard before and after it arrived");
    if (ok) {
      ui::showRouteFor(pub->id, mi);
      run(600);
      shot("25-route-channel");
      tap(400, 10);  // tap outside the card: closes
      run(200);
    }
  }
  {  // a direct message: copies matched by the sender's hash, then by the packet itself
    std::vector<uint8_t> pay = {0xD0, 0x22, 0x5A, 0xA5};  // to us (our key starts d0), from KD9XYZ (22...), MAC
    for (int i = 0; i < 32; i++) pay.push_back((uint8_t)(i * 37 + 11));
    auto dmFrame = [&](const std::vector<uint8_t> &pl, uint8_t route, std::vector<uint8_t> path, int8_t snr4, int8_t rssi) {
      std::vector<uint8_t> f = {0x88, (uint8_t)snr4, (uint8_t)rssi, (uint8_t)((0x02 << 2) | route), (uint8_t)path.size()};
      f.insert(f.end(), path.begin(), path.end());
      f.insert(f.end(), pl.begin(), pl.end());
      return f;
    };
    auto d1 = dmFrame(pay, 1, {0x44}, 30, -80);
    radio.frame(d1.data(), d1.size());
    queueDm(0x22, "Heading out, catch you on the mesh later");
    radio.syncNow();
    run(1500);
    auto d2 = dmFrame(pay, 1, {0x55, 0x44}, 8, -95);  // a later copy of the same packet
    radio.frame(d2.data(), d2.size());
    std::vector<uint8_t> pay2 = pay;
    pay2[5] ^= 0xFF;  // a second message, sent along a set route: arrives with an empty path
    auto d3 = dmFrame(pay2, 2, {}, 18, -84);
    radio.frame(d3.data(), d3.size());
    queueDm(0x22, "Got your reply, thanks!");
    radio.syncNow();
    run(1500);
    st::Conv *dm = radio.conv("d:222324252627");
    st::RMsg *m1 = nullptr, *m2 = nullptr;
    size_t i2 = 0;
    if (dm)
      for (size_t k = 0; k < dm->msgs.size(); k++) {
        if (dm->msgs[k].text == "Heading out, catch you on the mesh later") m1 = &dm->msgs[k];
        if (dm->msgs[k].text == "Got your reply, thanks!") { m2 = &dm->msgs[k]; i2 = k; }
      }
    bool ok = m1 && m2 && m1->paths.size() == 2 && m1->paths[0].hops.size() == 1 && m1->paths[1].hops.size() == 2 && m1->pkt != 0 &&
              m2->paths.size() == 1 && m2->paths[0].direct && m2->pkt != m1->pkt;
    printf("  dm routes: first %d copies, second %d (direct %d)\n", m1 ? (int)m1->paths.size() : -1, m2 ? (int)m2->paths.size() : -1,
           m2 && !m2->paths.empty() ? (int)m2->paths[0].direct : -1);
    check(ok, "direct-message copies matched to the right messages, set-route copy recognised");
    if (ok && dm) {
      size_t i1 = 0;
      for (size_t k = 0; k < dm->msgs.size(); k++) if (&dm->msgs[k] == m1) i1 = k;
      ui::showRouteFor(dm->id, i1);
      run(600);
      shot("26-route-dm");
      ui::showRouteFor(dm->id, i2);
      run(600);
      shot("27-route-dm-direct");
      tap(400, 10);
      run(200);
    }
  }
  radio.sendText("d:" + radio.contacts[0].key.substr(0, 12), "test dm");
  radio.openDm(radio.contacts[0]);
  radio.sendText("d:" + radio.contacts[0].key.substr(0, 12), "test dm");
  run(1500);
  for (auto &cv : radio.convs) for (auto &m : cv.msgs) delivered |= m.mine && m.status == st::MS_DELIVERED;
  check(delivered, "DM delivery receipt matched");
  {  // trace route to the repeater along its stored route, mirrored back
    std::string tk;
    for (auto &ct : radio.contacts) if (ct.name.str().rfind("Tower-RPT", 0) == 0) tk = ct.key.str();  // reached via one repeater
    radio.forgetFirstHand();  // an earlier test message came straight from code 44; here it's only known by its saved route
    bool started = !tk.empty() && radio.traceRoute(tk);
    ui::showScreenByName("mesh");
    run(1500);
    auto &t = radio.trace;
    std::string route;
    for (auto &h : t.route) route += h + " ";
    printf("  trace route: %s| snrs:", route.c_str());
    for (float v : t.snrs) printf(" %.1f", v);
    printf(" | back to you %.1f\n", t.finalSnr);
    bool ok = started && t.done && !t.failed && t.snrs.size() == t.route.size() && t.route.size() >= 3 &&
              t.route.front() == t.route.back() && std::abs(t.snrs[0] - 9.5f) < 0.01f && std::abs(t.finalSnr - 6.5f) < 0.01f;
    ok = ok && t.route.size() == 3 && t.route[0] == "a1" && t.route[1] == "44";
    check(ok, "trace route sent along the stored route and back, per-hop SNR decoded");
    if (ok) {
      ui::showTraceFor(tk);
      run(600);
      shot("24-trace-route");
    }
  }
  {  // muting a channel: no alerts from it, on the CoreScope chatter or the radio
    auto mention = [&](const std::string &ch, const std::string &id) {
      st::PacketIn p;
      p.hash = id;
      p.typeName = "GRP_TXT";
      p.channel = ch;
      p.sender = "Bot-7";
      p.text = "@[" + model.nodes[0].name + "] ack " + id;
      p.observerId = model.observers[0].id;
      p.observerName = model.observers[0].name;
      p.pubKey = model.nodes[21].key;
      model.handlePacket(p);
    };
    size_t a0 = model.alerts.size();
    model.setMuted("#test", true);
    mention("#test", "mute1");
    size_t a1 = model.alerts.size();
    mention("#TEST", "mute2");  // names match whatever the case
    size_t a2 = model.alerts.size();
    model.setMuted("#test", false);
    mention("#test", "mute3");
    size_t a3 = model.alerts.size();
    check(a1 == a0 && a2 == a0 && a3 == a0 + 1 && model.cfg.mutedChannels.empty(), "muted chatter channel raises no alerts; unmuting restores them");
    // radio: an @mention in a muted radio channel raises no alert and doesn't count as unread on the tab
    model.setMuted("#chicago", true);
    size_t b0 = model.alerts.size();
    queueChannelMsg(1, "Ridge-RPT: @[" + radio.selfName + "] can you hear me?", 30, 1);
    radio.syncNow();
    ui::showScreenByName("map");
    run(1500);
    size_t b1 = model.alerts.size();
    st::Conv *chi = radio.conv("c:1");
    model.setMuted("#chicago", false);
    queueChannelMsg(1, "Ridge-RPT: @[" + radio.selfName + "] and now?", 30, 1);
    radio.syncNow();
    run(1500);
    size_t b2 = model.alerts.size();
    model.setMuted("#chicago", true);
    check(b1 == b0 && b2 == b0 + 1 && chi && chi->unread > 0 && model.isMuted(chi->title), "muted radio channel: message kept, no alert (unmuted: alert)");
    model.setMuted("Public", true);  // also mute the CoreScope Public channel for the screenshot
    ui::showSetup(false, nullptr);   // clear what earlier steps left open: Wi-Fi setup, the trace pop-up, the banner
    ui::showScreenByName("mesh");
    tap(583, 334);
    run(10500);
    ui::showScreenByName("chan");
    run(800);
    shot("28-chatter-muted");
    ui::showScreenByName("mesh");
    ui::showRadioSection("chats");
    ui::radioOpenConvFor("c:1");
    run(800);
    shot("29-mesh-muted");
    model.setMuted("Public", false);
    model.setMuted("#chicago", false);
  }
  {  // batch 1: radio health, "Who can hear me?", routes on the map
    ui::showScreenByName("mesh");
    ui::showRadioSection("radio");
    run(1500);
    const st::RadioStats &h = radio.stats;
    check(h.core && h.radio && h.packets && h.noiseFloor == -112 && std::abs(h.lastSnr - 6.5f) < 0.01f && h.txAirSecs == 1234 &&
              h.recvErrors == 40 && h.upSecs == 3 * 86400 + 4 * 3600 + 17 * 60,
          "radio health: core, radio and packet stats decoded");
    shot("30-radio-health");
    ui::showDiscovery();
    run(1500);
    auto &dv = radio.discovery;
    printf("  discovery: running %d done %d failed %d found %d tag %08x note '%s' state %d fw %d\n", dv.running, dv.done, dv.failed, (int)dv.found.size(),
           (unsigned)dv.tag, dv.note.c_str(), (int)radio.state, radio.fwCode);
    for (auto &f : dv.found) printf("    %s type %d they %.1f we %.1f rssi %d\n", f.key.c_str(), f.type, f.theyHear, f.weHear, f.rssi);
    bool okd = dv.running && dv.found.size() == 3;
    for (auto &f : dv.found) okd = okd && f.type == 2 && f.key.size() == 16;
    if (okd) okd = std::abs(dv.found[0].theyHear - 8.5f) < 0.01f && std::abs(dv.found[0].weHear - 6.5f) < 0.01f && dv.found[0].rssi == -71;
    check(okd, "who can hear me: answers matched by tag, both directions' SNR decoded");
    run(600);
    shot("31-who-hears-me");
    ui::discoveryToMap();
    run(2500);
    shot("32-who-hears-me-map");
    st::Conv *pubc = radio.conv("c:0");
    size_t ri = 0;
    if (pubc)
      for (size_t k = 0; k < pubc->msgs.size(); k++)
        if (pubc->msgs[k].text == "Anyone hear the new Tower repeater?") ri = k;
    ui::showRouteFor("c:0", ri);
    run(400);
    ui::routeToMap(1);  // the copy that came through two repeaters
    run(2500);
    shot("33-route-on-map");
    run(21000);  // the discovery window closes
    check(!dv.running && dv.done && dv.found.size() == 3, "discovery window closes after 20 s");
    // trace to a repeater that just answered "Who can hear me?": straight there and back
    std::string tk;
    for (auto &ct : radio.contacts) if (ct.name.str().rfind("Tower-RPT", 0) == 0) tk = ct.key.str();
    radio.traceRoute(tk);
    run(1500);
    auto &t = radio.trace;
    check(t.done && !t.failed && !t.viaSaved && t.route.size() == 1 && t.route[0] == "44", "trace goes straight to a repeater heard in direct range");
    check(t.fromRxLog, "trace finished from what the radio heard on air (it logs the packet before its own report)");
    fakeTraceNoAir = true;  // the radio's own trace report alone still works
    radio.traceRoute(tk);
    run(1500);
    fakeTraceNoAir = false;
    check(t.done && !t.failed && !t.fromRxLog && t.snrs.size() == 1, "trace finished from the radio's own report");
    fakeTraceNoReport = true;  // ...and so does what it heard alone
    radio.traceRoute(tk);
    run(1500);
    fakeTraceNoReport = false;
    check(t.done && !t.failed && t.fromRxLog && t.snrs.size() == 1 && std::abs(t.finalSnr - 6.5f) < 0.01f, "trace finished from the air when the report never comes");
    // a repeater not heard directly, whose saved route is stale: the saved route fails, then straight works
    radio.discovery = st::DiscoverRun();
    radio.forgetFirstHand();
    fakeDropLongTraces = true;
    radio.traceRoute(tk);
    run(1500);
    bool firstWay = t.viaSaved && t.waiting && t.route.size() == 3;
    bool progress = t.heardHops == 1 && ui::traceTextNow().find("pass it on (1 of 3)") != std::string::npos;
    run(25000);
    fakeDropLongTraces = false;
    check(firstWay && t.done && !t.failed && t.retriedDirect && t.route.size() == 1, "stale saved route: trace retried straight to the repeater");
    check(progress && t.secondTry && t.first.heardHops == 1, "trace shows how far it got (the first repeater passed it on)");

    // the real flow: Trace route button on the MESH screen, pop-up open, watched over time
    radio.discovery = st::DiscoverRun();
    radio.forgetFirstHand();
    fakeTraceSilent = true;
    ui::showScreenByName("mesh");
    ui::startTraceFor(tk);
    run(1200);
    std::string p1 = ui::popupTextNow();
    run(2200);
    std::string p2 = ui::popupTextNow();
    check(p1.find("come back...  1 s") != std::string::npos && p2.find("come back...  3 s") != std::string::npos,
          "trace pop-up keeps counting while it's open");
    fakeTraceSilent = false;
    tap(400, 10);  // close it
    run(300);
    ui::startTraceFor(tk);
    run(4000);
    std::string p3 = ui::popupTextNow();
    check(p3.find("Round trip") != std::string::npos, "trace pop-up shows the answer when it comes back");
    shot("36-trace-came-back");
    tap(400, 10);
    run(300);

    // two repeaters share the short code 44: the one you picked is named, the other is pointed out
    auto jp = contactFrame(0x8A, 0x44, "Jeff Park-RPT", 2, 3, 41.97, -87.76, 0x90);
    radio.frame(jp.data(), jp.size());
    run(300);
    radio.forgetFirstHand();
    fakeTraceSilent = true;
    radio.traceRoute(tk);
    run(1500);
    std::string tt = ui::traceTextNow();
    bool named = tt.find("Tower-RPT Naperville (44)") != std::string::npos && tt.find("Jeff Park-RPT (44)") == std::string::npos;
    bool shared = tt.find("also used by Jeff Park-RPT") != std::string::npos;
    ui::showTraceFor(tk);
    run(600);
    shot("34-trace-shared-code");
    run(40000);  // saved route, then straight: both silent
    std::string ft = ui::traceTextNow();
    ui::showTraceFor(tk);
    run(600);
    shot("35-trace-no-reply");
    tap(400, 10);
    run(300);
    fakeTraceSilent = false;
    check(named && shared, "trace names the repeater you picked, not another with the same short code");
    check(t.failed && t.secondTry && ft.find("Straight to it: your radio didn't hear any repeater pass it on") != std::string::npos &&
              ft.find("Along your saved route: it got as far as") == std::string::npos,
          "failed trace says what was heard on each try");
    // packets reach your radio straight from a repeater with code 44: try it straight first
    {
      uint8_t k[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
      auto f = rxLogFrame(k, 1234, "x: hi", {0xA1, 0x44}, 20, -90);
      radio.frame(f.data(), f.size());
    }
    check(radio.heardDirectly(tk), "a repeater that just passed a packet straight to you counts as in range");
    fakeTraceSilentDirect = true;  // ...but it doesn't answer straight, so the saved route is tried next
    radio.traceRoute(tk);
    run(1500);
    bool directFirst = !t.viaSaved && t.route.size() == 1 && t.waiting;
    run(25000);
    fakeTraceSilentDirect = false;
    check(directFirst && t.done && !t.failed && t.secondTry && t.viaSaved && t.route.size() == 3 && !t.retriedDirect,
          "trace tries straight first when it's heard first-hand, then the saved route");
    for (size_t i = 0; i < radio.contacts.size(); i++)
      if (radio.contacts[i].name.str() == "Jeff Park-RPT") radio.contacts.erase(radio.contacts.begin() + i);
  }
  {  // ---- tapping your own message: Send again / Copy
    ui::tidyForScreenshot();
    st::Conv *dm = nullptr;
    for (auto &c : radio.convs) if (!c.isChannel) { dm = &c; break; }
    std::string dmId = dm ? dm->id : "";
    if (dm) {
      radio.sendText(dmId, "Did my message get through?");
      run(1200);
      dm = radio.conv(dmId);
      size_t mi = dm->msgs.size() - 1;
      dm->msgs[mi].status = st::MS_FAILED;  // no delivery receipt came back
      ui::showHeardFor(dmId, mi);
      run(300);
      std::string btns = ui::popupButtons(), txt = ui::popupTextNow();
      shot("40-not-delivered");
      ui::popupPress(1);  // Copy
      run(300);
      std::string copied = ui::composeTextNow();
      ui::tidyForScreenshot();
      check(btns == "Close|Copy|Send again" && txt.find("No delivery receipt") != std::string::npos && copied == "Did my message get through?",
            "a DM that wasn't delivered offers Send again and Copy (Copy fills the message box)");
      size_t before = radio.conv(dmId)->msgs.size();
      ui::showHeardFor(dmId, mi);
      run(200);
      ui::popupPress(2);  // Send again
      run(1200);
      check(radio.conv(dmId)->msgs.size() == before + 1 && radio.conv(dmId)->msgs.back().text == "Did my message get through?",
            "Send again sends it once more");
    }
    // a channel message no repeater was heard passing on
    radio.sendText("c:1", "Anyone hear this one?");
    run(1500);
    st::Conv *ch = radio.conv("c:1");
    size_t ci = ch ? ch->msgs.size() - 1 : 0;
    if (ch) {
      ch->msgs[ci].heard.clear();
      ui::showHeardFor("c:1", ci);
      run(300);
      std::string b2 = ui::popupButtons(), t2 = ui::popupTextNow();
      ui::popupPress(1);
      run(300);
      std::string c2 = ui::composeTextNow();
      ui::tidyForScreenshot();
      check(b2 == "Close|Copy|Send again" && t2.find("hasn't heard any repeater") != std::string::npos && c2 == "Anyone hear this one?",
            "a channel message nobody repeated offers Send again and Copy");
    }
    run(300);
  }
  {  // ---- repeater neighbours, remote admin, and finding a fresh route
    ui::tidyForScreenshot();
    ui::showScreenByName("mesh");
    ui::showRadioSection("people");
    run(400);
    st::RContact *rpt = radio.contactByPrefix("111213");
    st::RContact *kd = radio.contactByPrefix("222324");
    if (rpt && kd) {
      std::string rk = rpt->key, kk = kd->key;
      st::RemoteStatus &rs = radio.remote[rk.substr(0, 12)];
      rs.loggedIn = rs.isAdmin = true;
      ui::selectPersonForTest(rk);
      radio.requestNeighbours(rk);
      run(1500);
      ui::focusResultForTest(1);
      run(600);
      shot("43-repeater-neighbours");
      radio.sendCli(rk, "ver");
      ui::focusResultForTest(2);
      run(1500);
      shot("43b-repeater-admin");
      st::RemoteStatus &rs2 = radio.remote[rk.substr(0, 12)];
      bool towerNamed = false;
      for (auto &nb : rs2.neigh) towerNamed |= nb.prefix == "444546474849";
      check(rs2.hasNeigh && rs2.neigh.size() == 3 && rs2.neighTotal == 5 && towerNamed && std::fabs(rs2.neigh[0].snr - 9.5f) < 0.01f,
            "a repeater's neighbours are listed (3 of 5, strongest first)");
      bool verBack = false;
      for (auto &l : rs2.cli) verBack |= l.find("v1.18.0") != std::string::npos;
      bool noChat = radio.conv("d:" + rk.substr(0, 12)) == nullptr || radio.conv("d:" + rk.substr(0, 12))->msgs.empty();
      check(verBack && rs2.cli.front() == "> ver" && noChat, "an admin command's answer shows in the console, not as a chat message");
      ui::selectPersonForTest(kk);
      radio.discoverPath(kk);
      ui::focusResultForTest(3);
      run(1500);
      shot("44-fresh-route");
      check(radio.pathDisc.done && !radio.pathDisc.failed && radio.pathDisc.out == std::vector<std::string>{"11", "44"} &&
                radio.pathDisc.back == std::vector<std::string>{"11", "44"},
            "a fresh route comes back both ways");
    } else check(false, "test contacts for neighbours / route");
  }
  {  // ---- stretched tiles: past the top zoom on hand, the lower zoom is enlarged instead of blank
    ui::tidyForScreenshot();
    plat.simTiles = true;
    plat.simMaxZ = 12;
    ui::showScreenByName("map");
    ui::mapSetView(model.center.lat, model.center.lon, 14);
    run(1500);
    shot("42-stretched");
    // the pattern's 1-pixel white lines come out 4 pixels wide at zoom 14 (enlarged 4x from zoom 12)
    int run4 = 0, runOther = 0;
    for (int y = 100; y < 380; y += 7) {
      int len = 0;
      for (int x = 300; x < 720; x++) {  // clear of the traffic panel and the buttons
        uint16_t c = fb[y * 800 + x];
        if (((c >> 11) & 31) > 20 && ((c >> 5) & 63) > 40 && (c & 31) > 20) len++;  // the white lines (maybe toned down)
        else {
          if (len == 4) run4++;
          else if (len > 0 && len < 4) runOther++;
          len = 0;
        }
      }
    }
    printf("stretched: %d 4-px runs, %d thinner\n", run4, runOther);
    check(run4 > 20 && runOther < run4 / 4, "past the top zoom, the zoom-12 tiles are drawn enlarged");
    plat.simTiles = false;
    ui::mapRecenter();
    run(500);
  }
  {  // ---- screen dimming chooser (STATUS > Dimming)
    ui::tidyForScreenshot();
    ui::showScreenByName("status");
    run(300);
    ui::showDimChoice();
    run(300);
    shot("41-dimming");
    plat.setDimMode("off");
    ui::closeChooser();
    ui::tidyForScreenshot();
    check(model.cfg.dimMode == "off", "the screen can be set never to dim");
    plat.setDimMode("auto");
  }
  {  // ---- data source: my radio only
    ui::tidyForScreenshot();
    model.cfg.demo = false;  // the simulator normally runs CoreScope's side as demo traffic
    plat.setDataSource("radio");
    run(1500);
    std::string tk;
    for (auto &ct : radio.contacts) if (ct.name.str().rfind("Tower-RPT", 0) == 0) tk = ct.key.str();
    st::Node *tn = model.findKey(tk);
    check(model.cfg.radioOnly() && model.cfg.radioOn && !ui::tabShown(1) && !ui::tabShown(2) && ui::tabShown(4),
          "radio only: FEED and CHATTER tabs hidden");
    check(tn && tn->role == st::ROLE_REPEATER && tn->hasLoc && model.findKey(radio.selfKey) && model.feed.empty() && model.observers.empty(),
          "radio only: the node list is your radio's contacts (CoreScope's is gone)");
    // an advert your radio hears, from a repeater it doesn't know yet
    {
      std::vector<uint8_t> f = {0x88, (uint8_t)(int8_t)24, (uint8_t)(int8_t)-88, (uint8_t)((0x04 << 2) | 0x01), 1, 0x44};
      for (int i = 0; i < 32; i++) f.push_back((uint8_t)(0x7A + i));
      p32(f, (uint32_t)(simNow() / 1000));
      for (int i = 0; i < 64; i++) f.push_back(0);
      f.push_back(0x80 | 0x10 | 0x02);
      p32(f, (uint32_t)(int32_t)(41.80 * 1e6)); p32(f, (uint32_t)(int32_t)(-87.95 * 1e6));
      for (char ch : std::string("Hilltop-RPT")) f.push_back((uint8_t)ch);
      radio.frame(f.data(), f.size());
    }
    run(300);
    st::Node *hn = model.findByName("Hilltop-RPT");
    check(!model.feed.empty() && model.feed[0].label == "ADVERT" && model.feed[0].who == "Hilltop-RPT" && hn && hn->hasLoc &&
              hn->role == st::ROLE_REPEATER,
          "radio only: an advert your radio hears shows in live traffic and adds the node");
    // a channel message your radio can read: live traffic only (MESH has the chat and its alerts)
    {
      size_t msgsBefore = model.messages.size();
      std::string kwWas = model.cfg.keywords;
      model.cfg.keywords = "radio only test";
      uint8_t pub[16];
      memcpy(pub, st::PUBLIC_CHANNEL_KEY, 16);
      auto f = rxLogFrame(pub, (uint32_t)(simNow() / 1000), "KD9XYZ Mobile: radio only test, anyone copy?", {0xA1, 0x44}, 20, -90);
      radio.frame(f.data(), f.size());
      run(300);
      bool kw = !model.alerts.empty() && model.alerts[0].kind == st::A_KEYWORD && model.alerts[0].body.find("anyone copy") != std::string::npos;
      model.cfg.keywords = kwWas;
      check(model.feed[0].label == "CHANNEL" && model.feed[0].who == "KD9XYZ Mobile" && model.feed[0].txt.find("anyone copy") != std::string::npos &&
                model.messages.size() == msgsBefore,
            "radio only: a channel message your radio hears shows in live traffic, not in CHATTER");
      check(kw, "radio only: keyword alerts still work on what your radio hears");
    }
    model.cfg.myNodesText = "Tower-RPT Naperville";
    plat.requestMyNodes();
    const st::MyNodeCard *tc = nullptr;
    for (auto &c : model.myCards) {
      printf("  my node card: %s  last %lld s ago  status %d\n", c.name.c_str(), (long long)(c.last ? (simNow() - c.last) / 1000 : -1), c.status);
      if (c.name == "Tower-RPT Naperville") tc = &c;
    }
    check(tc && tc->last > 0 && tc->status == 0 && tc->today < 0, "radio only: My Nodes cards come from your radio's contacts");
    check(!model.search("tower", 5).empty(), "radio only: search finds your radio's contacts");
    ui::showScreenByName("map");
    run(2500);
    shot("37-radio-only-map");
    ui::showScreenByName("status");
    run(1200);
    shot("38-radio-only-status");
    ui::showDataSource();
    run(400);
    shot("39-data-source-choices");
    tap(400, 10);
    run(200);
    ui::showScreenByName("map");
    run(200);
    // and back: CoreScope's side returns, the radio's nodes go
    plat.setDataSource("both");
    model.cfg.demo = true;
    model.cfg.myNodesText = "";
    run(800);
    check(ui::tabShown(1) && ui::tabShown(2) && !model.findByName("Hilltop-RPT") && !model.cfg.radioOnly(),
          "back to CoreScope + radio: tabs return, the radio's nodes are gone");
  }
  auto &rs = radio.remote[radio.contacts[1].key.substr(0, 12)];
  check(rs.hasStats && rs.battMv == 4105 && rs.upSecs == 3 * 86400 + 5 * 3600 + 12 * 60 && std::abs(rs.lastSnr - 6.5f) < 0.01f, "repeater status decoded");
  radio.removeChannel(3);
  run(500);
  check(!radio.channelByIdx(3) && slots[3].name.empty(), "remove channel clears the slot");
  radio.setName("DudeTested Desk");
  run(500);
  check(radio.selfName == "DudeTested Desk", "rename");
  printf("%s (%d failed)\n", fails ? "SELF-CHECK FAILED" : "SELF-CHECK OK", fails);
  return fails ? 1 : 0;
}

#ifdef MESHTOP_DESKTOP
#include "desktop.inc"
#endif
