// Persistent settings (NVS) + the copy of the settings shared with the network task / web page.
#pragma once
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/model.h"

namespace store {

struct DeviceCfg {
  st::Settings s;
  std::string tz = "CST6CDT,M3.2.0,M11.1.0";  // POSIX TZ (US Central)
  std::string cartoKey;
  int brightness = 100;
};

struct Lists {
  std::vector<std::pair<std::string, std::string>> myNodes, mentions;  // key, name
  uint8_t groupMask = 0x1F;  // bit per st::Group, CONTROL off
};

void load(DeviceCfg &c, Lists &l);
void saveCfg(const DeviceCfg &c);
void saveLists(const Lists &l);
void wipeAll();

// Shared snapshot (UI thread writes, network task / web page read)
void publish(const DeviceCfg &c, const Lists &l, const std::string &status);
DeviceCfg snapshot();
Lists listsSnapshot();
std::string statusSnapshot();

}  // namespace store
