// Settings page served by the panel itself: http://meshtop.local (or the IP shown on STATUS)
#include "web.h"

#include <Arduino.h>
#include <WebServer.h>

#include "ble_radio.h"
#include "net.h"
#include "store.h"
#include "core/chatlog.h"

#include <algorithm>
#include <map>
#include <time.h>

#ifndef SCOPETHING_VERSION
#define SCOPETHING_VERSION "2.1.2"
#endif

namespace web {

static WebServer server(80);
static bool started = false;

static String esc(const std::string &s) {
  String o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}
static String text(const char *name, const char *label, const std::string &v, const char *hint = "") {
  return String("<label>") + label + "<input name='" + name + "' value=\"" + esc(v) + "\"></label>" +
         (*hint ? String("<p class=h>") + hint + "</p>" : String());
}
static String numF(const char *name, const char *label, int v, int mn, int mx, const char *hint = "") {
  return String("<label>") + label + "<input type=number name='" + name + "' value='" + v + "' min='" + mn + "' max='" + mx + "'></label>" +
         (*hint ? String("<p class=h>") + hint + "</p>" : String());
}
static String check(const char *name, const char *label, bool v) {
  return String("<label class=c><input type=checkbox name='") + name + "' value=1" + (v ? " checked" : "") + "> " + label + "</label>";
}
static String select(const char *name, const char *label, const std::string &v, const std::vector<std::pair<const char *, const char *>> &opts) {
  String s = String("<label>") + label + "<select name='" + name + "'>";
  bool found = false;
  for (auto &o : opts) {
    bool sel = v == o.first;
    found |= sel;
    s += String("<option value=\"") + o.first + "\"" + (sel ? " selected" : "") + ">" + o.second + "</option>";
  }
  if (!found && !v.empty()) s += "<option value=\"" + esc(v) + "\" selected>" + esc(v) + "</option>";
  return s + "</select></label>";
}

static const char *CSS =
    "<style>body{margin:0;background:#0A0E13;color:#E6EDF3;font:16px system-ui,sans-serif}main{max-width:640px;margin:auto;padding:16px}"
    "h1{font-size:24px;margin:8px 0 2px}h1 span{color:#39D0FF}h2{font-size:14px;letter-spacing:.08em;color:#9FB0C2;margin:26px 0 8px;border-top:1px solid #1C2633;padding-top:16px}"
    "label{display:block;margin:10px 0 0;color:#C3CFDB;font-size:14px}input,select{display:block;width:100%;box-sizing:border-box;margin-top:4px;padding:10px;border-radius:8px;"
    "border:1px solid #263445;background:#0F151D;color:#E6EDF3;font-size:16px}label.c{display:flex;align-items:center;gap:8px}label.c input{width:auto;margin:0}"
    ".h{color:#7D8A9A;font-size:13px;margin:4px 0}button{margin-top:18px;padding:12px 18px;border:0;border-radius:10px;background:#39D0FF;color:#06202B;font-weight:700;font-size:16px}"
    "button.g{background:#16202C;color:#E6EDF3;border:1px solid #263445}.ok{background:#10251C;color:#3DDC97;padding:10px;border-radius:8px}"
    ".row{display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-bottom:1px solid #1C2633}.row button{margin:0;padding:6px 10px}"
    "a{color:#39D0FF}footer{color:#7D8A9A;font-size:13px;margin:30px 0}</style>";

// Your radio's chats as saved on the SD card, newest conversation first (read-only).
static void handleMessages() {
  String h = "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'><meta http-equiv=refresh content=30>"
             "<title>MESHtop messages</title>";
  h += CSS;
  h += "<style>.m{padding:8px 10px;margin:6px 0;border-radius:10px;background:#111922}.m.me{background:#0F2A36;margin-left:40px}"
       ".m .w{color:#39D0FF;font-size:13px}.m .t{color:#7D8A9A;font-size:12px;float:right}.m .x{white-space:pre-wrap;word-wrap:break-word;margin-top:2px}</style>";
  h += "<main><h1><span>MESH</span>top messages</h1><p class=h>Saved on the panel's memory card. Refreshes every 30 seconds. <a href=/>Settings</a></p>";
  struct Conv {
    std::string title;
    bool isChannel = true;
    std::vector<st::ChatLine> msgs;
    long long last = 0;
  };
  std::map<std::string, Conv> convs;
  for (auto &name : net::sdListFiles("/scopething/chats")) {
    std::string path = "/scopething/chats/" + std::string(name.c_str() + (name.rfind('/') == std::string::npos ? 0 : name.rfind('/') + 1));
    std::string data;
    if (!net::sdReadTail(path, 60000, data)) continue;
    for (auto &l : st::chatParseAll(data)) {
      std::string k = (l.isChannel ? "c:" + l.chanName : "d:" + l.dmKey.substr(0, 12));
      Conv &c = convs[k];
      c.title = l.title.empty() ? (l.isChannel ? l.chanName : l.dmKey.substr(0, 8)) : l.title;
      c.isChannel = l.isChannel;
      c.last = std::max(c.last, l.ts);
      c.msgs.push_back(l);
    }
  }
  if (convs.empty()) h += "<p>No saved messages yet. They're saved while <b>Keep my chats on the SD card</b> is on, a memory card is in the panel, and your radio is connected.</p>";
  std::vector<Conv *> order;
  for (auto &kv : convs) order.push_back(&kv.second);
  std::sort(order.begin(), order.end(), [](Conv *a, Conv *b) { return a->last > b->last; });
  for (Conv *c : order) {
    std::stable_sort(c->msgs.begin(), c->msgs.end(), [](const st::ChatLine &a, const st::ChatLine &b) { return a.ts < b.ts; });
    h += "<h2>" + esc((c->isChannel ? "" : "@ ") + c->title) + "</h2>";
    size_t from = c->msgs.size() > 60 ? c->msgs.size() - 60 : 0;
    for (size_t i = from; i < c->msgs.size(); i++) {
      auto &m = c->msgs[i];
      time_t t = (time_t)(m.ts / 1000);
      struct tm lt;
      localtime_r(&t, &lt);
      char when[32];
      strftime(when, sizeof when, "%b %d, %I:%M %p", &lt);
      h += String("<div class='m") + (m.mine ? " me" : "") + "'><span class=t>" + when + "</span><div class=w>" + esc(m.mine ? "You" : m.who) + "</div><div class=x>" +
           esc(m.text) + "</div></div>";
    }
  }
  h += "<footer>MESHtop " SCOPETHING_VERSION "</footer></main>";
  server.send(200, "text/html", h);
}

static void handleRoot() {
  store::DeviceCfg c = store::snapshot();
  store::Lists l = store::listsSnapshot();
  const st::Settings &s = c.s;
  String h;
  h.reserve(14000);
  h += "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>MESHtop</title>";
  h += CSS;
  h += "</head><body><main><h1>MESH<span>top</span></h1><div class=h>CrowPanel 7\" | v" SCOPETHING_VERSION " | ";
  h += esc(store::statusSnapshot());
  h += "</div>";
  if (server.hasArg("saved")) h += "<p class=ok>Saved. The panel is applying your settings.</p>";
  h += "<form method=post action=/save>";
  h += "<h2>DATA SOURCE</h2>";
  h += select("data", "Show live data from", s.radioOnly() ? "radio" : s.radioOn ? "both" : "corescope",
              {{"both", "CoreScope and my radio"}, {"corescope", "CoreScope only (no radio)"}, {"radio", "My radio only (CoreScope off)"}});
  h += "<p class=h>My radio only never contacts CoreScope. The map shows your radio's contacts and the packets it hears; FEED and CHATTER are hidden.</p>";
  h += "<h2>WHERE TO WATCH</h2>";
  h += text("server", "CoreScope server", s.server, "The CoreScope instance for your region, e.g. https://corescope.chicagolandmesh.org");
  h += text("area", "Area", s.area, "City, zip code, or \"lat, lon\". Example: Naperville, IL  or  41.88, -87.63");
  h += numF("radius", "Radius (miles)", s.radiusMi, 5, 300);
  h += text("rpt", "Watch one repeater (optional)", s.watchRepeater, "Repeater name. When set, only traffic through that repeater is shown.");
  h += check("demo", "Demo mode (fake traffic, no network needed)", s.demo);
  h += "<h2>MAP + THEME</h2>";
  h += select("theme", "Theme", s.theme, {{"auto", "Auto: day mode from sunrise to sunset"}, {"dark", "Always dark"}, {"day", "Always day"}});
  h += select("style", "Night map style", s.mapStyle, {{"dark", "Dark"}, {"light", "Light (dimmed)"}, {"none", "No map (grid only)"}});
  h += text("carto", "CARTO API key (optional)", c.cartoKey, "Leave empty to use OpenStreetMap tiles, darkened on the panel.");
  h += check("ctrl", "Show CONTROL packets (ACKs etc.)", s.showControl);
  h += "<h2>MY NODES + ALERTS</h2>";
  h += text("mynodes", "My Nodes (names or key prefixes, comma separated)", s.myNodesText, "You can also add nodes on the panel: tap a node on the map, or use Find node.");
  h += check("almine", "Alert when My Nodes come back on the air or go silent", s.alertMyNodes);
  h += numF("quiet", "Back-on-air after quiet for (minutes)", s.quietMin, 5, 1440);
  h += numF("silent", "Gone silent after (hours)", s.silentHours, 1, 72);
  h += text("mentions", "Watch @mentions of these names", s.mentionNames, "Comma separated. A pop-up shows when someone @'s them in a public channel.");
  h += check("menmine", "Also watch @mentions of My Nodes", s.mentionMyNodes);
  h += text("keywords", "Keyword alerts", s.keywords, "Comma separated words to watch for in channel messages.");
  h += check("alnew", "Alert on new nodes inside the radius", s.alertNewNodes);
  h += check("sound", "Beep on alerts", s.alertSound);
  h += "<h2>YOUR RADIO</h2><p class=h>Send and receive on the mesh through your own MeshCore companion radio (e.g. XIAO nRF52840 + Wio-SX1262 with Companion Radio BLE firmware). Turn it on or off with the data source above. Disconnect the phone app from it first.</p>";
  h += text("rname", "Radio name (optional)", s.radioName, "Part of the Bluetooth name, e.g. MeshCore-DudeXIAO. Empty = nearest MeshCore radio.");
  h += numF("rpin", "Radio Bluetooth PIN", s.radioPin, 0, 999999, "MeshCore default is 123456.");
  h += check("chatsd", "Keep my chats on the SD card", s.chatsToSd);
  h += "<p class=h>Messages you send and receive on MESHtop are saved on the memory card, so they're still there after a restart. Read them on the <a href=/messages>Messages page</a>.</p>";
  h += check("phone", "Leave messages for my phone", s.phoneMode);
  h += "<p class=h>Messages stay waiting on your radio, so the MeshCore phone app still gets them. MESHtop shows channel messages from what your radio hears; direct messages only reach the phone app. Messages you send from MESHtop never appear in the phone app.</p>";
  h += "<h2>DISPLAY</h2>";
  h += numF("bright", "Brightness (%)", c.brightness, 10, 100);
  h += numF("ambient", "Go back to the map when untouched for (minutes, 0 = never)", s.idleMin, 0, 240);
  h += select("dim", "Dim the screen when untouched", s.dimMode, {{"auto", "At night (sunset to sunrise)"}, {"always", "Always"}, {"off", "Never"}});
  h += select("tz", "Time zone", c.tz,
              {{"EST5EDT,M3.2.0,M11.1.0", "US Eastern"},
               {"CST6CDT,M3.2.0,M11.1.0", "US Central"},
               {"MST7MDT,M3.2.0,M11.1.0", "US Mountain"},
               {"MST7", "US Arizona"},
               {"PST8PDT,M3.2.0,M11.1.0", "US Pacific"},
               {"AKST9AKDT,M3.2.0,M11.1.0", "Alaska"},
               {"HST10", "Hawaii"},
               {"GMT0BST,M3.5.0/1,M10.5.0", "UK"},
               {"CET-1CEST,M3.5.0,M10.5.0/3", "Central Europe"},
               {"AEST-10AEDT,M10.1.0,M4.1.0/3", "Australia East"},
               {"UTC0", "UTC"}});
  h += "<button>Save settings</button></form>";

  h += "<h2>SAVED ON THE PANEL</h2>";
  if (l.myNodes.empty() && l.mentions.empty()) h += "<p class=h>Nothing yet. Nodes you add on the touchscreen show up here.</p>";
  for (auto &m : l.myNodes)
    h += "<form method=post action=/rm class=row><span>" + esc(m.second) + " <span class=h>My Node</span></span><input type=hidden name=k value=\"" + esc(m.first) +
         "\"><input type=hidden name=t value=n><button class=g>Remove</button></form>";
  for (auto &m : l.mentions)
    h += "<form method=post action=/rm class=row><span>@" + esc(m.second) + " <span class=h>mention watch</span></span><input type=hidden name=k value=\"" + esc(m.first) +
         "\"><input type=hidden name=t value=m><button class=g>Remove</button></form>";

  h += "<form method=post action=/radioforget class=row><span class=h>Paired with a different radio? Forget the saved one.</span><button class=g>Forget radio</button></form>";
  h += "<h2>WI-FI</h2><p class=h>Forget this Wi-Fi network and start the setup hotspot (MESHtop-Setup) again.</p>";
  h += "<form method=post action=/wifireset onsubmit=\"return confirm('Forget Wi-Fi and restart?')\"><button class=g>Reset Wi-Fi</button></form>";
  h += "<footer>Data: CoreScope | Map &copy; OpenStreetMap contributors | Enjoying MESHtop? <a href='https://buymeacoffee.com/hamradiodude'>Buy DudeTested a coffee</a></footer>";
  h += "</main></body></html>";
  server.send(200, "text/html; charset=utf-8", h);
}

static std::string arg(const char *n) { return std::string(server.arg(n).c_str()); }
static int argInt(const char *n, int def, int mn, int mx) {
  if (!server.hasArg(n)) return def;
  int v = server.arg(n).toInt();
  return v < mn ? mn : v > mx ? mx : v;
}

static void handleSave() {
  store::DeviceCfg c = store::snapshot();
  st::Settings &s = c.s;
  s.server = arg("server");
  if (s.server.empty()) s.server = st::Settings().server;
  s.area = arg("area");
  if (s.area.empty()) s.area = st::Settings().area;
  s.radiusMi = argInt("radius", s.radiusMi, 5, 300);
  s.watchRepeater = arg("rpt");
  s.demo = server.hasArg("demo");
  s.mapStyle = arg("style");
  if (server.hasArg("theme")) s.theme = arg("theme");
  c.cartoKey = arg("carto");
  s.showControl = server.hasArg("ctrl");
  s.myNodesText = arg("mynodes");
  s.alertMyNodes = server.hasArg("almine");
  s.quietMin = argInt("quiet", s.quietMin, 5, 1440);
  s.silentHours = argInt("silent", s.silentHours, 1, 72);
  s.mentionNames = arg("mentions");
  s.mentionMyNodes = server.hasArg("menmine");
  s.keywords = arg("keywords");
  s.alertNewNodes = server.hasArg("alnew");
  s.alertSound = server.hasArg("sound");
  c.brightness = argInt("bright", c.brightness, 10, 100);
  s.idleMin = argInt("ambient", s.idleMin, 0, 240);
  s.dimMode = arg("dim");
  if (server.hasArg("tz")) c.tz = arg("tz");
  std::string data = arg("data");
  if (data == "radio") s.useCoreScope = false, s.radioOn = true;
  else if (data == "corescope") s.useCoreScope = true, s.radioOn = false;
  else if (data == "both") s.useCoreScope = true, s.radioOn = true;
  s.radioName = arg("rname");
  s.radioPin = argInt("rpin", s.radioPin, 0, 999999);
  s.chatsToSd = server.hasArg("chatsd");
  s.phoneMode = server.hasArg("phone");
  net::Event *e = new net::Event();
  e->type = net::EV_SETTINGS;
  e->cfg = c;
  net::post(e);
  server.sendHeader("Location", "/?saved=1");
  server.send(303);
}

static void handleRemove() {
  net::Event *e = new net::Event();
  e->type = net::EV_LIST_REMOVE;
  e->text = arg("k");
  e->ok = arg("t") == "m";
  net::post(e);
  server.sendHeader("Location", "/?saved=1");
  server.send(303);
}

static void handleWifiReset() {
  server.send(200, "text/html", "<meta name=viewport content='width=device-width'><body style='background:#0A0E13;color:#E6EDF3;font:18px sans-serif;padding:20px'>"
                                "Wi-Fi forgotten. The panel is restarting. Join the <b>MESHtop-Setup</b> hotspot to connect it again.");
  delay(500);
  net::resetWifi();
  delay(300);
  ESP.restart();
}

void begin() {
  if (started) return;
  started = true;
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/messages", HTTP_GET, handleMessages);
  server.on("/rm", HTTP_POST, handleRemove);
  server.on("/wifireset", HTTP_POST, handleWifiReset);
  server.on("/radioforget", HTTP_POST, [] {
    bleradio::forget();
    server.sendHeader("Location", "/?saved=1");
    server.send(303);
  });
  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void loop() {
  if (started) server.handleClient();
}

}  // namespace web
