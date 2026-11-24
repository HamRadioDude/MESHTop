#include "store.h"

#include <Preferences.h>

namespace store {

static std::mutex mtx;
static DeviceCfg shared;
static Lists sharedLists;
static std::string sharedStatus;

static std::string getS(Preferences &p, const char *k, const std::string &def) {
  if (!p.isKey(k)) return def;
  return std::string(p.getString(k, def.c_str()).c_str());
}
static std::string packList(const std::vector<std::pair<std::string, std::string>> &v) {
  std::string out;
  for (auto &e : v) out += e.first + "|" + e.second + "\n";
  return out;
}
static std::vector<std::pair<std::string, std::string>> unpackList(const std::string &s) {
  std::vector<std::pair<std::string, std::string>> v;
  size_t a = 0;
  while (a < s.size()) {
    size_t nl = s.find('\n', a);
    if (nl == std::string::npos) nl = s.size();
    std::string line = s.substr(a, nl - a);
    size_t bar = line.find('|');
    if (bar != std::string::npos && bar > 0) v.push_back({line.substr(0, bar), line.substr(bar + 1)});
    a = nl + 1;
  }
  return v;
}

void load(DeviceCfg &c, Lists &l) {
  Preferences p;
  p.begin("scopething", true);
  st::Settings &s = c.s;
  s.server = getS(p, "server", s.server);
  s.area = getS(p, "area", s.area);
  s.radiusMi = p.getInt("radius", s.radiusMi);
  s.watchRepeater = getS(p, "rpt", s.watchRepeater);
  s.myNodesText = getS(p, "mynodes", s.myNodesText);
  s.keywords = getS(p, "keywords", s.keywords);
  s.mentionNames = getS(p, "mentions", s.mentionNames);
  s.mapStyle = getS(p, "style", s.mapStyle);
  s.showControl = p.getBool("ctrl", s.showControl);
  s.alertMyNodes = p.getBool("almine", s.alertMyNodes);
  s.quietMin = p.getInt("quiet", s.quietMin);
  s.silentHours = p.getInt("silent", s.silentHours);
  s.alertNewNodes = p.getBool("alnew", s.alertNewNodes);
  s.mentionMyNodes = p.getBool("menmine", s.mentionMyNodes);
  s.idleMin = p.getInt("ambient", s.idleMin);  // key kept from the old screensaver setting
  s.feedFolded = p.getBool("feedfold", s.feedFolded);
  s.dimMode = getS(p, "dim", s.dimMode);
  s.theme = getS(p, "theme", s.theme);
  s.radioOn = p.getBool("radio", s.radioOn);
  s.useCoreScope = p.getBool("cs", s.useCoreScope);
  s.radioPin = p.getInt("rpin", s.radioPin);
  s.radioName = getS(p, "rname", s.radioName);
  s.chatsToSd = p.getBool("chatsd", s.chatsToSd);
  s.phoneMode = p.getBool("phone", s.phoneMode);
  s.alertSound = p.getBool("sound", s.alertSound);
  s.demo = p.getBool("demo", s.demo);
  s.mutedChannels = getS(p, "muted", s.mutedChannels);
  c.tz = getS(p, "tz", c.tz);
  c.cartoKey = getS(p, "carto", c.cartoKey);
  c.brightness = p.getInt("bright", c.brightness);
  l.myNodes = unpackList(getS(p, "l_mine", ""));
  l.mentions = unpackList(getS(p, "l_ment", ""));
  l.groupMask = p.getUChar("groups", l.groupMask);
  p.end();
}

void saveCfg(const DeviceCfg &c) {
  Preferences p;
  p.begin("scopething", false);
  const st::Settings &s = c.s;
  p.putString("server", s.server.c_str());
  p.putString("area", s.area.c_str());
  p.putInt("radius", s.radiusMi);
  p.putString("rpt", s.watchRepeater.c_str());
  p.putString("mynodes", s.myNodesText.c_str());
  p.putString("keywords", s.keywords.c_str());
  p.putString("mentions", s.mentionNames.c_str());
  p.putString("style", s.mapStyle.c_str());
  p.putBool("ctrl", s.showControl);
  p.putBool("almine", s.alertMyNodes);
  p.putInt("quiet", s.quietMin);
  p.putInt("silent", s.silentHours);
  p.putBool("alnew", s.alertNewNodes);
  p.putBool("menmine", s.mentionMyNodes);
  p.putInt("ambient", s.idleMin);
  p.putBool("feedfold", s.feedFolded);
  p.putString("dim", s.dimMode.c_str());
  p.putString("theme", s.theme.c_str());
  p.putBool("radio", s.radioOn);
  p.putBool("cs", s.useCoreScope);
  p.putInt("rpin", s.radioPin);
  p.putString("rname", s.radioName.c_str());
  p.putBool("chatsd", s.chatsToSd);
  p.putBool("phone", s.phoneMode);
  p.putBool("sound", s.alertSound);
  p.putBool("demo", s.demo);
  p.putString("muted", s.mutedChannels.c_str());
  p.putString("tz", c.tz.c_str());
  p.putString("carto", c.cartoKey.c_str());
  p.putInt("bright", c.brightness);
  p.end();
}

void saveLists(const Lists &l) {
  Preferences p;
  p.begin("scopething", false);
  p.putString("l_mine", packList(l.myNodes).c_str());
  p.putString("l_ment", packList(l.mentions).c_str());
  p.putUChar("groups", l.groupMask);
  p.end();
}

void wipeAll() {
  Preferences p;
  p.begin("scopething", false);
  p.clear();
  p.end();
}

void publish(const DeviceCfg &c, const Lists &l, const std::string &status) {
  std::lock_guard<std::mutex> g(mtx);
  shared = c;
  sharedLists = l;
  sharedStatus = status;
}
DeviceCfg snapshot() {
  std::lock_guard<std::mutex> g(mtx);
  return shared;
}
Lists listsSnapshot() {
  std::lock_guard<std::mutex> g(mtx);
  return sharedLists;
}
std::string statusSnapshot() {
  std::lock_guard<std::mutex> g(mtx);
  return sharedStatus;
}

}  // namespace store
