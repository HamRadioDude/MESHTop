// MESHtop (formerly the ScopeThing panel) for the Elecrow CrowPanel Advance 7" (ESP32-S3)
// Internal names (SCOPETHING_*, the "scopething" settings namespace, /scopething on the SD card) are
// kept on purpose: renaming them would lose saved settings and cached map tiles on update.
// A live CoreScope MeshCore map for your desk: streaks, feed, channels, My Nodes, alerts.
// MIT License. By DudeTested (HamRadioDude).
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <Preferences.h>
#include <esp_random.h>
#include <lvgl.h>
#include <sys/time.h>

#include <unordered_map>

#include "app/demo.h"
#include "app/platform.h"
#include "app/radio_bridge.h"
#include "core/model.h"
#include "core/companion.h"
#include "core/chatlog.h"
#include "device/ble_radio.h"
#include "device/board.h"
#include "device/net.h"
#include "device/store.h"
#include "ui/ui.h"

#ifndef SCOPETHING_VERSION
#define SCOPETHING_VERSION "2.1.2"
#endif

static st::Model model;
static st::Demo demo;
static st::Companion radio;
static store::DeviceCfg dcfg;
static store::Lists lists;

// ---------------------------------------------------------------- tile cache (UI thread only)
struct TileSlot {
  uint64_t key;
  uint16_t *pix;
  uint32_t used;
};
static std::vector<TileSlot> tileCache;
static std::unordered_map<uint64_t, uint32_t> tilePending;  // key -> retry-after (millis)
static std::unordered_map<uint64_t, uint32_t> tileMissed;   // key -> when a try last found nothing
static constexpr size_t TILE_SLOTS = 16;  // 128 KB each in PSRAM

static uint64_t tileKey(const std::string &style, int z, int x, int y) {
  uint64_t s = style == "light" ? 1 : 0;
  return (s << 60) | ((uint64_t)z << 52) | ((uint64_t)x << 26) | (uint64_t)y;
}

static void applySettings(const store::DeviceCfg &c, bool first);
static void radioOnlySync(bool force = false);
static uint32_t lastNodesChange = 0;

class DevicePlatform : public st::Platform {
 public:
  int64_t nowMs() override {
    if (!net::timeValid()) return (int64_t)millis();
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
  }
  bool timeValid() override { return net::timeValid(); }
  void localClock(int64_t ms, int &h, int &m, int &s) override {
    time_t t = (time_t)(ms / 1000);
    struct tm lt;
    localtime_r(&t, &lt);
    h = lt.tm_hour;
    m = lt.tm_min;
    s = lt.tm_sec;
  }
  const uint16_t *tile(int z, int x, int y, const std::string &style) override {
    if (style == "none") return nullptr;
    uint64_t k = tileKey(style, z, x, y);
    for (auto &t : tileCache)
      if (t.key == k) {
        t.used = millis();
        return t.pix;
      }
    auto it = tilePending.find(k);
    if (it != tilePending.end() && (int32_t)(millis() - it->second) < 0) return nullptr;
    tilePending[k] = millis() + 90000;  // node-list loading can hold the download queue for half a minute
    net::Req *r = new net::Req();
    r->type = net::RQ_TILE;
    r->z = z;
    r->x = x;
    r->y = y;
    r->style = style;
    r->gen = net::currentTileGen();
    net::request(r);
    return nullptr;
  }
  const uint16_t *tilePeek(int z, int x, int y, const std::string &style) override {
    uint64_t k = tileKey(style, z, x, y);
    for (auto &t : tileCache)
      if (t.key == k) {
        t.used = millis();
        return t.pix;
      }
    return nullptr;
  }
  bool tileFailed(int z, int x, int y, const std::string &style) override { return tileMissed.count(tileKey(style, z, x, y)) > 0; }
  void dropTileRequests() override {
    net::dropTiles();
    tilePending.clear();
  }
  void beep(int ms) override { board::beep(ms); }
  void setBacklight(int pct) override { board::backlight(pct * dcfg.brightness / 100); }
  void requestChannels() override {
    if (model.cfg.radioOnly() && !model.cfg.demo) return;  // CoreScope is off
    if (model.cfg.demo) {
      demo.channels(model, nowMs());
      ui::channelsChanged();
      return;
    }
    net::Req *r = new net::Req();
    r->type = net::RQ_CHANNELS;
    net::request(r);
  }
  void requestHistory(const std::string &ch, const std::string &hash) override {
    if (model.cfg.radioOnly() && !model.cfg.demo) return;
    if (model.cfg.demo) {
      demo.history(model, ch, nowMs());
      ui::historyChanged(ch);
      return;
    }
    net::Req *r = new net::Req();
    r->type = net::RQ_HISTORY;
    r->a = ch;
    r->b = hash;
    net::request(r);
  }
  void requestMyNodes() override {
    model.resolveMyNodes();
    if (model.cfg.demo) {
      demo.myCards(model, nowMs());
      ui::myNodesChanged();
      return;
    }
    if (model.cfg.radioOnly()) {  // from what your radio knows; nothing goes to CoreScope
      st::radioOnlyCards(model);
      ui::myNodesChanged();
      return;
    }
    net::Req *r = new net::Req();
    r->type = net::RQ_HEALTH;
    r->keys = model.myKeys;
    net::request(r);
  }
  void requestSearch(const std::string &q) override {
    if (model.cfg.demo || model.cfg.radioOnly()) return;  // radio only: the panel searches your contacts itself
    net::Req *r = new net::Req();
    r->type = net::RQ_SEARCH;
    r->a = q;
    net::request(r);
  }
  void saveLists() override { persistLists(); }
  void saveFilter() override { persistLists(); }
  void saveSettings() override {
    dcfg.s = model.cfg;
    store::saveCfg(dcfg);
    store::publish(dcfg, lists, net::connMsg());
  }
  st::ConnState conn() override { return model.cfg.demo ? st::CONN_DEMO : net::conn(); }
  std::string connMsg() override { return model.cfg.demo ? "Demo mode (fake traffic)" : net::connMsg(); }
  std::string settingsUrl() override { return net::settingsUrl(); }
  std::string netInfo() override { return net::netInfo(); }
  std::string cardInfo() override { return net::cardInfo(); }
  std::string version() override { return "v" SCOPETHING_VERSION; }
  st::Companion *radio() override { return &::radio; }
  bool radioEnabled() override { return model.cfg.radioOn; }
  void setDataSource(const std::string &mode) override {
    store::DeviceCfg c = dcfg;
    c.s = model.cfg;
    c.s.useCoreScope = mode != "radio";
    c.s.radioOn = mode != "corescope";
    applySettings(c, false);
  }
  void setDimMode(const std::string &mode) override {
    model.cfg.dimMode = mode;
    saveSettings();
  }
  void radioEnable(bool on) override {
    if (!on && model.cfg.radioOnly()) {  // no radio and no CoreScope would leave nothing to show
      setDataSource("corescope");
      ui::radioResult(true, "Radio off. Showing CoreScope only.");
      return;
    }
    model.cfg.radioOn = on;
    saveSettings();
    bleradio::configure(on, model.cfg.radioPin, model.cfg.radioName);
    if (!on) ::radio.setState(st::R_OFF, "Radio off");
    else ::radio.setState(st::R_SCANNING, "Starting Bluetooth...");
  }
  std::string radioPin() override { return std::to_string(model.cfg.radioPin); }
  void setRadioPin(uint32_t pin) override {
    model.cfg.radioPin = pin;
    saveSettings();
    bleradio::configure(model.cfg.radioOn, pin, model.cfg.radioName);
    if (model.cfg.radioOn) ::radio.setState(st::R_SCANNING, "Trying the new PIN...");
  }
  std::string radioName() override { return model.cfg.radioName; }
  void wifiScan() override {
    net::Req *r = new net::Req();
    r->type = net::RQ_WIFI_SCAN;
    net::request(r);
  }
  void wifiConnect(const std::string &ssid, const std::string &pass) override {
    net::Req *r = new net::Req();
    r->type = net::RQ_WIFI_CONNECT;
    r->a = ssid;
    r->b = pass;
    net::request(r);
  }
  void radioForget() override {
    bleradio::forget();
    ::radio.setState(st::R_SCANNING, "Looking for a radio...");
  }
  bool radioSetupDone() override {
    Preferences p;
    p.begin("scopething", true);
    bool d = p.getBool("rwiz", false);
    p.end();
    return d;
  }
  void setRadioSetupDone(bool d) override {
    Preferences p;
    p.begin("scopething", false);
    p.putBool("rwiz", d);
    p.end();
  }

  static void persistLists() {
    lists.myNodes = model.savedMyNodes;
    lists.mentions = model.savedMentions;
    lists.groupMask = 0;
    for (int i = 0; i < st::G_COUNT; i++)
      if (model.groupOn[i]) lists.groupMask |= 1 << i;
    store::saveLists(lists);
    store::publish(dcfg, lists, net::connMsg());
  }
};
static DevicePlatform plat;

// ---------------------------------------------------------------- settings
static void resolveRepeater() {
  const std::string &want = model.cfg.watchRepeater;
  if (want.empty()) {
    model.repeaterKey.clear();
    model.repeaterName.clear();
    return;
  }
  st::Node *n = model.findByName(want);
  if (n) {
    model.repeaterKey = n->key;
    model.repeaterName = n->name;
    if (n->hasLoc) model.center = {n->lat, n->lon};
  } else {
    model.repeaterKey.clear();
    model.repeaterName = want + " (not found yet)";
    plat.requestSearch(want);
  }
}

static void sendConfig() {
  net::Req *r = new net::Req();
  r->type = net::RQ_CONFIG;
  r->cfg = dcfg;
  net::request(r);
}

static void applySettings(const store::DeviceCfg &c0, bool first) {
  store::DeviceCfg c = c0;
  if (c.s.radioOnly()) c.s.radioOn = true;  // radio only needs the radio
  bool wasDemo = model.cfg.demo;
  bool wasRadioOnly = model.cfg.radioOnly();
  bool areaChanged = c.s.area != model.cfg.area;
  dcfg = c;
  model.cfg = c.s;
  model.groupOn[st::G_CONTROL] = c.s.showControl || (lists.groupMask & (1 << st::G_CONTROL));
  if (!first) model.groupOn[st::G_CONTROL] = c.s.showControl;
  setenv("TZ", c.tz.c_str(), 1);
  tzset();
  if (areaChanged) model.areaLabel = c.s.area;
  if (c.s.demo && (!wasDemo || first)) {
    model.feed.clear();
    demo.seed(model, plat.nowMs());
    demo.channels(model, plat.nowMs());
  } else if (!c.s.demo && wasDemo && !first) {
    model.nodes.clear();
    model.nodeIndex.clear();
    model.feed.clear();
    model.channels.clear();
    model.messages.clear();
    model.directoryReady = false;
  }
  // CoreScope switched off or on: nothing from one source stays beside the other
  if (!first && !c.s.demo && c.s.radioOnly() != wasRadioOnly) {
    st::clearForSourceSwitch(model);
    if (c.s.radioOnly()) radioOnlySync(true);
  }
  model.resolveMyNodes();
  resolveRepeater();
  if (!first) {
    store::saveCfg(dcfg);
    sendConfig();
    plat.requestMyNodes();
    plat.setBacklight(100);
    plat.dropTileRequests();
    ui::themeChanged();
    ui::configChanged();
  }
  bleradio::configure(c.s.radioOn, c.s.radioPin, c.s.radioName);
  radio.leaveForPhone = c.s.phoneMode;
  if (!c.s.radioOn) radio.setState(st::R_OFF, "Radio off");
  store::publish(dcfg, lists, net::connMsg());
}

// Radio only: your radio's contacts are the panel's node list (map, My Nodes, search)
static void radioOnlySync(bool force) {
  if (!model.cfg.radioOnly() || model.cfg.demo) return;
  if (st::bridgeContacts(model, radio, force)) {
    lastNodesChange = millis();
    model.resolveMyNodes();
  }
  if (!model.directoryReady && radio.state == st::R_READY) {
    model.markDirectoryReady();
    plat.requestMyNodes();
    ui::mapChanged();
  }
}

static void handleRadio(bleradio::Event *e) {
  switch (e->type) {
    case bleradio::BE_FRAME:
      radio.frame(e->data.data(), e->data.size());
      break;
    case bleradio::BE_CONNECTED:
      radio.devName = e->text;
      radio.connected();
      break;
    case bleradio::BE_DISCONNECTED:
      // NimBLE reasons: 520 = link timed out (radio out of range or starved), 531 = the radio hung up,
      // 534 = we hung up, 574 = never connected
      Serial.printf("[radio] link lost (%s)  internal free %u (lowest %u)\n", e->text.c_str(),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
      radio.disconnected();
      break;
    case bleradio::BE_STATUS:
      if (radio.state != st::R_READY && radio.state != st::R_SYNCING) radio.setState(e->state, e->text);
      break;
  }
  delete e;
}

// ---------------------------------------------------------------- saved chats (SD card)
static std::string chatFile() {
  return radio.selfKey.size() >= 12 ? "/scopething/chats/" + radio.selfKey.substr(0, 12) + ".txt" : "";
}
static void storeChat(const st::Conv &c, const st::RMsg &m) {
  std::string path = chatFile();
  if (!model.cfg.chatsToSd || model.cfg.demo || path.empty()) return;
  st::ChatLine l;
  l.isChannel = c.isChannel;
  l.chanName = c.isChannel ? c.title : "";
  l.dmKey = c.isChannel ? "" : c.key;
  l.title = c.title;
  l.ts = m.ts;
  l.mine = m.mine;
  l.who = m.who;
  l.hops = m.hops;
  l.hasSnr = m.hasSnr;
  l.snr = m.snr;
  l.text = m.text;
  net::Req *r = new net::Req();
  r->type = net::RQ_APPEND;
  r->a = path;
  r->b = st::chatLineEncode(l);
  net::request(r);
}
static std::string chatsRestored;  // the radio whose saved chats have been put back
static uint32_t radioReadySince = 0;
static void maybeRestoreChats() {  // once per radio, a few seconds after it's ready (channels loaded)
  if (radio.state != st::R_READY || !model.cfg.chatsToSd || model.cfg.demo || chatFile().empty()) {
    radioReadySince = 0;
    return;
  }
  if (chatsRestored == radio.selfKey) return;
  if (!radioReadySince) radioReadySince = millis();
  if (millis() - radioReadySince < 5000) return;
  chatsRestored = radio.selfKey;
  net::Req *r = new net::Req();
  r->type = net::RQ_READ;
  r->a = chatFile();
  net::request(r);
}
static void restoreChats(const std::string &data) {
  int n = 0;
  for (auto &l : st::chatParseAll(data)) {
    st::RMsg m;
    m.who = l.who;
    m.text = l.text;
    m.ts = l.ts;
    m.mine = l.mine;
    m.hops = l.hops;
    m.hasSnr = l.hasSnr;
    m.snr = l.snr;
    radio.restoreMsg(l.isChannel, l.chanName, l.dmKey, m);
    n++;
  }
  Serial.printf("[chats] %d saved messages put back from %s\n", n, chatFile().c_str());
  if (n) ui::radioChanged();
}

// ---------------------------------------------------------------- network events -> model / UI
static void handle(net::Event *e) {
  using namespace net;
  switch (e->type) {
    case EV_FILE:
      if (e->ok && e->text == chatFile()) restoreChats(e->blob);
      break;
    case EV_PACKET:
      if (!model.cfg.demo) model.handlePacket(e->pkt);
      break;
    case EV_NODES:
    case EV_SEARCH:
      if (model.cfg.demo) break;
      for (auto &n : e->nodes) model.upsertNode(n.key, n.name, n.role, n.hasLoc, n.lat, n.lon, n.last);
      lastNodesChange = millis();
      if (e->type == EV_SEARCH) {
        if (!model.cfg.watchRepeater.empty() && model.repeaterKey.empty()) {
          resolveRepeater();
          if (!model.repeaterKey.empty()) ui::configChanged();
        }
        ui::searchChanged();
        ui::mapChanged();
      }
      break;
    case EV_OBSERVERS:
      if (!model.cfg.demo) model.setObservers(std::move(e->observers));
      ui::observersChanged();
      break;
    case EV_DIR_DONE: {
      if (model.cfg.demo) break;
      bool firstLoad = !model.directoryReady;
      model.markDirectoryReady();
      std::string before = model.repeaterKey;
      resolveRepeater();
      if (model.repeaterKey != before) ui::configChanged();
      if (firstLoad) {
        plat.requestMyNodes();
        plat.requestChannels();
      }
      ui::mapChanged();
      break;
    }
    case EV_CENTER:
      if (e->ok) {
        if (model.repeaterKey.empty()) model.center = {e->lat, e->lon};
        model.areaLabel = e->text;
        if (model.cfg.demo) {
          demo.seed(model, plat.nowMs());
          model.resolveMyNodes();
        }
        ui::configChanged();
      } else {
        model.areaLabel = e->text + " (not found)";
      }
      break;
    case EV_CHANNELS:
      if (!model.cfg.demo) {
        model.setChannels(std::move(e->channels));
        ui::channelsChanged();
      }
      break;
    case EV_HISTORY:
      if (!model.cfg.demo) {
        model.setHistory(e->text, std::move(e->msgs));
        ui::historyChanged(e->text);
      }
      break;
    case EV_HEALTH: {
      std::vector<st::MyNodeCard> cards;
      for (auto &h : e->health) {
        if (!model.isMine(h.key)) continue;  // removed meanwhile
        st::Node *n = model.findKey(h.key);
        st::MyNodeCard c;
        c.key = h.key;
        c.name = !h.name.empty() ? h.name : n ? n->name.str() : h.key.substr(0, 8);
        c.role = !h.role.empty() ? st::parseRole(h.role) : n ? n->role : st::ROLE_UNKNOWN;
        c.last = std::max(h.last, n ? n->last : (int64_t)0);
        c.hasSnr = h.hasSnr;
        c.avgSnr = h.snr;
        c.today = h.today;
        c.observers = h.observers;
        if (h.hasLoc) {
          c.hasLoc = true;
          c.lat = h.lat;
          c.lon = h.lon;
        } else if (n && n->hasLoc) {
          c.hasLoc = true;
          c.lat = n->lat;
          c.lon = n->lon;
        }
        cards.push_back(c);
      }
      // keep cards for My Nodes the server did not answer for
      for (auto &k : model.myKeys)
        if (std::none_of(cards.begin(), cards.end(), [&](auto &c) { return c.key == k; })) {
          st::Node *n = model.findKey(k);
          st::MyNodeCard c;
          c.key = k;
          c.name = n ? n->name.str() : k.substr(0, 8);
          c.role = n ? n->role : st::ROLE_UNKNOWN;
          c.last = n ? n->last : 0;
          if (n && n->hasLoc) {
            c.hasLoc = true;
            c.lat = n->lat;
            c.lon = n->lon;
          }
          cards.push_back(c);
        }
      model.setMyCards(std::move(cards));
      ui::myNodesChanged();
      break;
    }
    case EV_TILE: {
      uint64_t k = tileKey(e->style, e->z, e->x, e->y);
      if (e->ok && e->pix) {
        tilePending.erase(k);
        tileMissed.erase(k);
        bool have = false;  // a second copy of a tile we already hold (asked twice): keep the first
        for (auto &t : tileCache)
          if (t.key == k) {
            have = true;
            t.used = millis();
          }
        if (have) break;  // handle() frees e->pix
        if (tileCache.size() >= TILE_SLOTS) {
          auto lru = std::min_element(tileCache.begin(), tileCache.end(), [](auto &a, auto &b) { return a.used < b.used; });
          heap_caps_free(lru->pix);
          tileCache.erase(lru);
        }
        tileCache.push_back({k, e->pix, millis()});
        e->pix = nullptr;
        ui::mapChanged();
      } else {
        tilePending[k] = millis() + 60000;  // failed: retry in a minute
        if (tileMissed.size() > 2000) tileMissed.clear();
        if (tileMissed.emplace(k, millis()).second) ui::mapChanged();  // draw a stretched lower-zoom tile there
      }
      break;
    }
    case EV_SETTINGS:
      applySettings(e->cfg, false);
      break;
    case EV_LIST_REMOVE:
      if (e->ok) {
        for (auto &m : model.savedMentions)
          if (m.first == e->text) {
            model.setMention(m.first, m.second, false);
            break;
          }
      } else {
        model.setMyNode(e->text, "", false);
        plat.requestMyNodes();
      }
      ui::myNodesChanged();
      break;
    case EV_PORTAL:
      ui::showSetup(e->ok, e->text.c_str());
      break;
    case EV_TIME:
      break;
    case EV_WIFI_SCAN: {
      std::vector<ui::WifiItem> items;
      for (auto &w : e->wifi) items.push_back({w.ssid, w.rssi, w.secure});
      ui::wifiScanResult(items);
      break;
    }
    case EV_WIFI_STATUS:
      ui::wifiStatus(e->text, e->ok, e->x != 0);
      break;
  }
  if (e->pix) heap_caps_free(e->pix);
  delete e;
}

// ---------------------------------------------------------------- Arduino
// TLS (CoreScope API, live feed, map tiles) allocates ~35 KB per connection. This build keeps those
// buffers in internal RAM, which Wi-Fi, Bluetooth and the display also need, so a few connections
// at once could starve the handshake. Big TLS buffers go to PSRAM (what Espressif's
// MBEDTLS_EXTERNAL_MEM_ALLOC option does); small ones stay internal.
extern "C" int mbedtls_platform_set_calloc_free(void *(*)(size_t, size_t), void (*)(void *));
static void *tlsCalloc(size_t n, size_t size) {
  size_t total = n * size;
  if (size && total / size != n) return nullptr;
  // Everything TLS allocates goes to PSRAM first (as Espressif's MBEDTLS_EXTERNAL_MEM_ALLOC does):
  // with the live feed, CoreScope and a tile server connected at once, even the small pieces added up.
  // None of it is DMA memory; the AES/SHA hardware drivers allocate their own internal buffers.
  void *p = heap_caps_calloc(1, total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_calloc(1, total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_calloc(1, total, MALLOC_CAP_8BIT);
  return p;
}
static void tlsFree(void *p) { heap_caps_free(p); }

void setup() {
  mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
  Serial.begin(115200);
  // Note: no global PSRAM malloc threshold. Wi-Fi/lwIP/WPA must stay in internal RAM on this build;
  // big app data is put in PSRAM explicitly (PStr, ExtAlloc, LVGL hooks, JSON allocator).
  Serial.println("\nMESHtop v" SCOPETHING_VERSION " for CrowPanel 7\"");
  if (!board::begin()) Serial.println("[main] display init failed");

  // Hold a finger on the screen during power-up for ~4 s to forget Wi-Fi
  if (board::touchHeld()) {
    uint32_t t0 = millis();
    while (board::touchHeld() && millis() - t0 < 4000) delay(20);
    if (millis() - t0 >= 4000) {
      Serial.println("[main] touch held at boot: forgetting Wi-Fi");
      WiFi.mode(WIFI_STA);
      WiFi.disconnect(true, true);
      board::beep(300);
      delay(400);
      board::loop();
    }
  }

  store::load(dcfg, lists);
  model.nowMs = [] { return plat.nowMs(); };
  model.onRoute = [](const st::Route &r) { ui::onRoute(r); };
  model.onAlert = [](const st::Alert &a) { ui::onAlert(a); };
  model.onChatMsg = [](const std::string &c, const st::ChatMsg &m) { ui::onChatMsg(c, m); };
  model.onSaveLists = [] { DevicePlatform::persistLists(); };
  model.savedMyNodes = lists.myNodes;
  model.savedMentions = lists.mentions;
  for (int i = 0; i < st::G_COUNT; i++) model.groupOn[i] = (lists.groupMask >> i) & 1;
  if (!(lists.groupMask & 0x3F)) model.groupOn[st::G_ADVERT] = model.groupOn[st::G_CHANNEL] = true;
  model.areaLabel = dcfg.s.area;
  model.cfg.demo = false;
  applySettings(dcfg, true);
  if (model.cfg.demo) demo.myCards(model, plat.nowMs());
  else
    for (auto &m : lists.myNodes) {  // placeholder cards until the server answers
      st::MyNodeCard c;
      c.key = m.first;
      c.name = m.second;
      model.myCards.push_back(c);
    }

  radio.nowMs = [] { return plat.nowMs(); };
  radio.clockValid = [] { return net::timeValid(); };
  radio.write = [](const std::vector<uint8_t> &f) { bleradio::write(f); };
  radio.onChange = [] {
    radioOnlySync();
    ui::radioChanged();
  };
  radio.onHeard = [](const st::HeardPkt &h) {
    if (!model.cfg.radioOnly() || model.cfg.demo) return;  // only while CoreScope is off
    st::bridgeHeard(model, radio, h);
    if (h.type == 4) lastNodesChange = millis();  // an advert may have added a node
  };
  radio.onMessage = [](const st::Conv &c, const st::RMsg &m) { ui::onRadioMessage(c, m); };
  radio.onStore = storeChat;
  radio.leaveForPhone = model.cfg.phoneMode;
  radio.onResult = [](bool ok, const std::string &t) { ui::radioResult(ok, t); };
  radio.randomBytes = [](uint8_t *b, size_t n) { esp_fill_random(b, n); };  // hardware RNG for private channel keys
  radio.log = [](const std::string &m) { Serial.println(m.c_str()); };
  bleradio::start();
  bleradio::configure(model.cfg.radioOn, model.cfg.radioPin, model.cfg.radioName);

  net::start(dcfg);  // before the UI: the map asks for tiles while it is being built
  sendConfig();
  ui::begin(&model, &plat);
  plat.setBacklight(100);
}

void loop() {
  static uint32_t nextDemo = 0, lastPublish = 0;
  net::Event *e;
  int n = 0;
  while (n++ < 24 && net::poll(e)) handle(e);
  bleradio::Event *be;
  // the radio's data: as much as fits in ~10 ms, so a burst (a sync, busy traffic) clears quickly
  // without starving the screen
  uint32_t radioStart = millis();
  n = 0;
  while (n++ < 128 && bleradio::poll(be)) {
    handleRadio(be);
    if (millis() - radioStart > 10) break;
  }
  static uint32_t lastRadioTick = 0;
  if (millis() - lastRadioTick > 1000) {
    lastRadioTick = millis();
    radio.tick();
    maybeRestoreChats();
  }
  if (lastNodesChange && millis() - lastNodesChange > 800) {
    lastNodesChange = 0;
    ui::mapChanged();
  }
  if (model.cfg.demo && (int32_t)(millis() - nextDemo) >= 0) nextDemo = millis() + demo.tick(model, plat.nowMs());
  static uint32_t lastMem = 0;
  if (millis() - lastMem > 30000) {
    lastMem = millis();
    Serial.printf("[mem] internal free %u (largest %u, lowest %u)  psram free %u  nodes %u  radio frames lost %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)model.nodes.size(), (unsigned)bleradio::droppedFrames());
  }
  if (millis() - lastPublish > 5000) {
    lastPublish = millis();
    store::publish(dcfg, lists, plat.connMsg());
  }
  board::loop();
  uint32_t wait = lv_timer_handler();
  delay(wait > 5 ? 5 : (wait ? wait : 1));
}
