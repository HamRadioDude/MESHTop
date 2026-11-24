// What the UI needs from the hardware / network side. The panel firmware and the desktop
// simulator each implement this.
#pragma once
#include <cstdint>
#include <string>

namespace st {

class Companion;

enum ConnState : uint8_t { CONN_WAIT = 0, CONN_LIVE, CONN_ERR, CONN_DEMO };

class Platform {
 public:
  virtual ~Platform() {}
  virtual int64_t nowMs() = 0;  // epoch ms (0-based fallback before NTP)
  virtual bool timeValid() = 0;
  virtual void localClock(int64_t ms, int &h, int &m, int &s) = 0;

  // Map tile cache. Returns 256x256 RGB565 pixels when ready, otherwise nullptr
  // (the request is queued and ui::mapChanged() is called once it arrives).
  virtual const uint16_t *tile(int z, int x, int y, const std::string &style) = 0;
  // a tile already in memory (no download asked for), and whether the last try at a tile found nothing
  virtual const uint16_t *tilePeek(int, int, int, const std::string &) { return nullptr; }
  virtual bool tileFailed(int, int, int, const std::string &) { return false; }
  virtual void dropTileRequests() {}

  virtual void beep(int ms) = 0;
  virtual void setBacklight(int pct) = 0;  // 0..100

  virtual void requestChannels() = 0;
  virtual void requestHistory(const std::string &channel, const std::string &hash) = 0;
  virtual void requestMyNodes() = 0;
  virtual void requestSearch(const std::string &q) = 0;

  virtual void saveLists() = 0;
  virtual void saveFilter() {}
  virtual void saveSettings() {}  // model.cfg changed on the panel (e.g. theme toggle)
  // where live data comes from: "both" (CoreScope and your radio), "corescope" (radio off) or "radio"
  // (CoreScope never contacted). Applies at once and saves.
  virtual void setDataSource(const std::string &) {}
  virtual void setDimMode(const std::string &) {}  // "auto" (at night) | "always" | "off"

  virtual ConnState conn() = 0;
  virtual std::string connMsg() = 0;
  virtual std::string settingsUrl() = 0;
  virtual std::string netInfo() = 0;
  virtual std::string cardInfo() { return ""; }  // STATUS "Map card" line
  virtual std::string version() = 0;

  // MeshThing (companion radio over Bluetooth)
  virtual Companion *radio() { return nullptr; }
  virtual bool radioEnabled() { return false; }
  virtual void radioEnable(bool) {}
  virtual std::string radioPin() { return "123456"; }
  virtual void setRadioPin(uint32_t) {}
  virtual std::string radioName() { return ""; }
  virtual void wifiScan() {}
  virtual void wifiConnect(const std::string &, const std::string &) {}
  virtual void radioForget() {}                       // forget the paired radio, scan again
  virtual bool radioSetupDone() { return true; }      // first-connect wizard shown already?
  virtual void setRadioSetupDone(bool) {}
};

}  // namespace st
