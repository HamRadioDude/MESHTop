// Network task: Wi-Fi, time, CoreScope (REST + live WebSocket), geocoding, map tiles, settings web page.
// Runs on core 0. It never touches the model or LVGL: it posts Events for the UI thread to apply.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "app/platform.h"
#include "core/model.h"
#include "store.h"

namespace net {

enum EvType : uint8_t {
  EV_PACKET,
  EV_NODES,
  EV_OBSERVERS,
  EV_DIR_DONE,
  EV_CENTER,
  EV_CHANNELS,
  EV_HISTORY,
  EV_HEALTH,
  EV_SEARCH,
  EV_TILE,
  EV_SETTINGS,     // saved from the web page
  EV_LIST_REMOVE,  // web page removed a My Node (ok=false) or @mention (ok=true)
  EV_PORTAL,       // Wi-Fi setup portal on (ok=true) / off
  EV_TIME,         // clock is valid now
  EV_WIFI_SCAN,    // nearby networks (wifi)
  EV_WIFI_STATUS,  // text = what happened, ok = connected, x = 1 while trying
  EV_FILE,         // RQ_READ answered: text = path, blob = the file's last part (ok = found)
};

struct RawNode {
  std::string key, name, role;
  bool hasLoc = false;
  double lat = 0, lon = 0;
  int64_t last = 0;
};
struct Health {
  std::string key, name, role;
  int64_t last = 0;
  bool hasSnr = false;
  float snr = 0;
  int today = -1, observers = -1;
  bool hasLoc = false;
  double lat = 0, lon = 0;
};

struct WifiNet {
  std::string ssid;
  int rssi = 0;
  bool secure = true;
};

struct Event {
  EvType type;
  st::PacketIn pkt;
  std::vector<RawNode> nodes;
  std::vector<st::Observer> observers;
  std::vector<st::ChannelInfo> channels;
  std::vector<st::ChatMsg> msgs;
  std::vector<Health> health;
  std::string text;
  double lat = 0, lon = 0;
  bool ok = false;
  int z = 0, x = 0, y = 0;
  uint32_t gen = 0;
  std::string style;
  uint16_t *pix = nullptr;  // EV_TILE: 256x256 RGB565 (heap, owned by the receiver)
  store::DeviceCfg cfg;
  std::vector<WifiNet> wifi;
  std::string blob;
};

enum ReqType : uint8_t {
  RQ_CONFIG, RQ_CHANNELS, RQ_HISTORY, RQ_HEALTH, RQ_SEARCH, RQ_TILE, RQ_WIFI_SCAN, RQ_WIFI_CONNECT,
  RQ_APPEND,  // SD card: add b (a line) to the file a
  RQ_READ,    // SD card: read the end of file a -> EV_FILE (text = path, blob = contents)
};
struct Req {
  ReqType type;
  std::string a, b;
  std::vector<std::string> keys;
  int z = 0, x = 0, y = 0;
  uint32_t gen = 0;
  std::string style;
  bool sdTried = false;  // the card worker already looked for this tile: download it, don't look again
  store::DeviceCfg cfg;
};

void start(const store::DeviceCfg &cfg);
bool poll(Event *&e);  // UI thread
void request(Req *r);  // UI thread
void post(Event *e);   // network task
void dropTiles();      // forget queued tile requests (view changed)
uint32_t currentTileGen();
// SD card helpers for the settings web page (network task)
std::vector<std::string> sdListFiles(const std::string &dir);
bool sdReadTail(const std::string &path, size_t maxBytes, std::string &out);
void resetWifi();      // network task only (web page)

st::ConnState conn();
std::string connMsg();
std::string netInfo();
std::string cardInfo();  // STATUS "Map card": free space, map packs, tiles saved
std::string settingsUrl();
bool timeValid();
// true while Wi-Fi is scanning or joining a network (Bluetooth backs off so they don't fight over the antenna)
bool wifiBusy();

}  // namespace net
