#include "net.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#if __has_include(<mdns.h>)
#include <ESPmDNS.h>
#define ST_HAS_MDNS 1
#endif
#include <FS.h>
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#include <NetworkClientSecure.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <nvs.h>
#include <esp_vfs_fat.h>
#include <esp_phy_init.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>

#include "web.h"
#include "core/tiles.h"

extern const uint8_t ca_bundle_start[] asm("_binary_data_x509_crt_bundle_start");
extern const uint8_t ca_bundle_end[] asm("_binary_data_x509_crt_bundle_end");

#ifndef SCOPETHING_VERSION
#define SCOPETHING_VERSION "2.1.2"
#endif

namespace net {

static const char *UA = "MESHtop/" SCOPETHING_VERSION " (CrowPanel; +https://github.com/HamRadioDude/ScopeThing)";
static const char *PORTAL_SSID = "MESHtop-Setup";

static QueueHandle_t evQ, rqQ, sdQ;
static std::atomic<uint8_t> connState{st::CONN_WAIT};
static std::atomic<bool> clockOk{false};
static std::atomic<uint32_t> tileGen{1};
static std::mutex msgMtx;
static std::string msg = "Starting";

static store::DeviceCfg cfg;
static WiFiManager wm;
static WebSocketsClient ws;
static NetworkClientSecure tlsApi, tlsTile;
static NetworkClient plainApi, plainTile;

static void setStatus(st::ConnState s, const std::string &m) {
  connState = s;
  std::lock_guard<std::mutex> g(msgMtx);
  msg = m;
}
st::ConnState conn() { return (st::ConnState)connState.load(); }
std::string connMsg() {
  std::lock_guard<std::mutex> g(msgMtx);
  return msg;
}
bool timeValid() { return clockOk; }
static std::atomic<int> wifiBusyN{0};
struct WifiBusy {
  WifiBusy() { wifiBusyN++; }
  ~WifiBusy() { wifiBusyN--; }
};
bool wifiBusy() { return wifiBusyN.load() > 0; }
static std::atomic<bool> sdOkPub{false};
static std::mutex sdInfoMtx;
// STATUS "Map card" line (the net task writes it, the UI reads it)
static std::string sdInfo = "Looking for a card...";
std::string netInfo() {
  if (WiFi.status() != WL_CONNECTED) return "Wi-Fi not connected";
  char b[96];
  snprintf(b, sizeof b, "%s  %d dBm  %s", WiFi.SSID().c_str(), WiFi.RSSI(), WiFi.localIP().toString().c_str());
  return b;
}
std::string cardInfo() {
  std::lock_guard<std::mutex> g(sdInfoMtx);
  return sdInfo;
}
std::string settingsUrl() {
  if (WiFi.status() != WL_CONNECTED) return "http://192.168.4.1";
  return std::string("http://") + WiFi.localIP().toString().c_str();
}

// Queues are created on first use, so callers never depend on start() having run first
// (the UI asks for map tiles while it is still being built).
static void ensureQueues() {
  static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  if (evQ && rqQ && sdQ) return;
  QueueHandle_t e = xQueueCreate(96, sizeof(Event *)), r = xQueueCreate(96, sizeof(Req *)), t = xQueueCreate(96, sizeof(Req *));
  portENTER_CRITICAL(&mux);
  if (!evQ) evQ = e;
  else vQueueDelete(e);
  if (!rqQ) rqQ = r;
  else vQueueDelete(r);
  if (!sdQ) sdQ = t;
  else vQueueDelete(t);
  portEXIT_CRITICAL(&mux);
}

void post(Event *e) {
  ensureQueues();
  if (xQueueSend(evQ, &e, e->type == EV_PACKET ? 0 : pdMS_TO_TICKS(200)) != pdTRUE) {
    if (e->pix) heap_caps_free(e->pix);
    delete e;
  }
}
bool poll(Event *&e) {
  ensureQueues();
  return xQueueReceive(evQ, &e, 0) == pdTRUE;
}
void request(Req *r) {
  ensureQueues();
  // map tiles go to the card worker first: reading the card must never wait behind a slow download
  bool card = (r->type == RQ_TILE && !r->sdTried) || r->type == RQ_APPEND || r->type == RQ_READ;
  QueueHandle_t q = card ? sdQ : rqQ;
  if (xQueueSend(q, &r, 0) != pdTRUE) delete r;
}
void dropTiles() { tileGen++; }
uint32_t currentTileGen() { return tileGen; }

// ================================================================ helpers
// JSON documents (node pages hold thousands of small strings) go to external PSRAM
struct PsramJsonAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
  }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override {
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, n);
  }
};
static PsramJsonAllocator psramJson;
struct PDoc : JsonDocument {
  PDoc() : JsonDocument(&psramJson) {}
};

static double num(JsonVariantConst v, double def = NAN) {
  if (v.is<double>() || v.is<long>() || v.is<int>()) return v.as<double>();
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    char *end = nullptr;
    double d = strtod(s, &end);
    if (end != s) return d;
  }
  return def;
}
static std::string str(JsonVariantConst v) {
  if (v.is<const char *>()) return v.as<const char *>();
  if (v.is<long>() || v.is<double>()) {
    char b[32];
    snprintf(b, sizeof b, "%g", v.as<double>());
    return b;
  }
  return "";
}
static bool validLL(double lat, double lon) {
  return !std::isnan(lat) && !std::isnan(lon) && std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && !(lat == 0 && lon == 0);
}
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}
static int64_t parseTime(JsonVariantConst v) {
  if (v.is<double>() || v.is<long>()) {
    double d = v.as<double>();
    return (int64_t)(d > 1e12 ? d : d * 1000);
  }
  if (!v.is<const char *>()) return 0;
  const char *s = v.as<const char *>();
  int Y, M, D, h = 0, mi = 0;
  double sec = 0;
  if (sscanf(s, "%d-%d-%d%*c%d:%d:%lf", &Y, &M, &D, &h, &mi, &sec) < 3) return 0;
  int64_t t = (daysFromCivil(Y, M, D) * 86400 + h * 3600 + mi * 60) * 1000 + (int64_t)(sec * 1000);
  const char *tz = strpbrk(s + 10, "Z+-");
  if (tz && (*tz == '+' || *tz == '-')) {
    int th = 0, tm = 0;
    if (sscanf(tz + 1, "%d:%d", &th, &tm) >= 1) t -= (*tz == '+' ? 1 : -1) * (int64_t)(th * 60 + tm) * 60000;
  }
  return t;
}
static std::string urlEnc(const std::string &s) {
  std::string o;
  char b[4];
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else {
      snprintf(b, sizeof b, "%%%02X", c);
      o += b;
    }
  }
  return o;
}
// Radio only: CoreScope is never contacted. Enforced at the two places every CoreScope request goes
// through (getJson for the REST API, startWs for the live feed); tools/privacy_check.py checks both.
static bool coreScopeOff() { return cfg.s.radioOnly(); }

static std::string base() {
  std::string s = cfg.s.server;
  while (!s.empty() && s.back() == '/') s.pop_back();
  if (s.rfind("http", 0) != 0) s = "https://" + s;
  return s;
}

// Response bodies are downloaded whole into PSRAM by HTTPClient (which handles Content-Length,
// chunked and close-delimited replies), then parsed from memory.
class PsramSink : public Stream {
 public:
  std::vector<char, st::ExtAlloc<char>> buf;
  size_t cap = 6 * 1024 * 1024;
  bool overflow = false;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *d, size_t n) override {
    if (buf.size() + n > cap) {
      overflow = true;
      return 0;
    }
    buf.insert(buf.end(), (const char *)d, (const char *)d + n);
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
};

// Read the response body ourselves instead of HTTPClient::writeToStream(): that loop spins on the TLS
// socket without ever yielding, and on a slow transfer it starved the idle task long enough for the
// task watchdog to reboot the panel (v2.1.29). We ask for HTTP/1.0 so the body is never chunked: it
// ends at Content-Length, or when the server closes the connection.
template <class Sink>
static int readBody(HTTPClient &http, Sink &sink, int len, int timeoutMs) {
  NetworkClient *s = http.getStreamPtr();
  if (!s) return -1;
  static uint8_t *tmp = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!tmp) return -1;
  uint32_t last = millis();
  size_t sinceYield = 0;
  while (true) {
    int a = s->available();
    if (a > 0) {
      int r = s->read(tmp, std::min(a, 4096));
      if (r > 0) {
        if (sink.write(tmp, r) != (size_t)r) return -3;  // too big
        last = millis();
        sinceYield += r;
        if (len > 0 && (int)sink.buf.size() >= len) break;
        if (sinceYield >= 16 * 1024) {  // let other tasks (and the idle task) run on long downloads
          sinceYield = 0;
          vTaskDelay(1);
        }
        continue;
      }
    }
    if (!s->connected()) break;
    if (millis() - last > (uint32_t)timeoutMs) return -11;  // stalled
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return (int)sink.buf.size();
}

static int getJson(const std::string &url, JsonDocument &doc, const JsonDocument *filter, int timeoutMs = 15000) {
  if (coreScopeOff() && url.rfind(base(), 0) == 0) return -100;  // radio only: CoreScope stays untouched
  HTTPClient http;
  bool tls = url.rfind("https", 0) == 0;
  if (tls) tlsApi.setCACertBundle(ca_bundle_start, ca_bundle_end - ca_bundle_start);
  http.setReuse(false);
  if (!http.begin(tls ? (NetworkClient &)tlsApi : plainApi, url.c_str())) return -1;
  http.useHTTP10(true);  // no chunked bodies; readBody() reads to Content-Length or close
  http.setUserAgent(UA);
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  http.setTimeout(timeoutMs);
  http.setConnectTimeout(8000);
  uint32_t t0 = millis();
  int code = http.GET();
  if (code == 200) {
    PsramSink sink;
    int len = http.getSize();
    sink.buf.reserve(len > 0 && len < (int)sink.cap ? len + 1 : 64 * 1024);
    int got = readBody(http, sink, len, timeoutMs);
    DeserializationError err = DeserializationError::EmptyInput;
    if (!sink.buf.empty())
      err = filter ? deserializeJson(doc, (const char *)sink.buf.data(), sink.buf.size(), DeserializationOption::Filter(*filter))
                   : deserializeJson(doc, (const char *)sink.buf.data(), sink.buf.size());
    Serial.printf("[http] %s: %u bytes (expected %d, write %d) in %u ms, JSON %s\n", url.c_str(), (unsigned)sink.buf.size(), len, got,
                  (unsigned)(millis() - t0), err.c_str());
    if (err) {
      if (!sink.buf.empty()) {
        char head[81];
        size_t n = std::min<size_t>(80, sink.buf.size());
        memcpy(head, sink.buf.data(), n);
        head[n] = 0;
        for (size_t i = 0; i < n; i++)
          if ((unsigned char)head[i] < 32) head[i] = '.';
        Serial.printf("[http] starts: %s\n", head);
      }
      code = sink.overflow ? -3 : -2;
    }
  }
  http.end();
  if (code != 200) {
    char tlsErr[100] = "";
    if (tls && code < 0) tlsApi.lastError(tlsErr, sizeof tlsErr);
    Serial.printf("[http] %s -> %d%s%s  internal free %u (largest %u, lowest %u)\n", url.c_str(), code, tlsErr[0] ? "  tls: " : "", tlsErr,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  }
  return code;
}

// Connection problems (a TLS handshake that didn't finish, a dropped or stalled socket, a busy server)
// are usually momentary: try twice more, a little later, before reporting a failure.
static int getJsonRetry(const std::string &url, JsonDocument &doc, const JsonDocument *filter, int timeoutMs = 15000) {
  int code = 0;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt) {
      Serial.printf("[http] retrying (%d/2) in %d s\n", attempt, attempt * 2);
      vTaskDelay(pdMS_TO_TICKS(2000 * attempt));
    }
    code = getJson(url, doc, filter, timeoutMs);
    bool transient = code == -1 || code == -2 || code == -4 || code == -5 || code == -7 || code == -8 || code == -11 || code >= 500;
    if (!transient) break;
  }
  return code;
}

// ================================================================ parsing
static RawNode rawNode(JsonObjectConst o) {
  RawNode n;
  n.key = str(o["public_key"]);
  if (n.key.empty()) n.key = str(o["pubKey"]);
  for (auto &c : n.key) c = (char)tolower(c);
  n.name = str(o["name"]);
  n.role = str(o["role"]);
  double lat = num(o["lat"]), lon = num(o["lon"]);
  if (validLL(lat, lon)) {
    n.hasLoc = true;
    n.lat = lat;
    n.lon = lon;
  }
  n.last = std::max(parseTime(o["last_heard"]), parseTime(o["last_seen"]));
  return n;
}
static void nodeFilter(JsonDocument &f, const char *arr) {
  JsonObject n = f[arr][0].to<JsonObject>();
  for (const char *k : {"public_key", "name", "role", "lat", "lon", "last_seen", "last_heard"}) n[k] = true;
}
static void strList(JsonVariantConst v, std::vector<std::string> &out) {
  if (v.is<JsonArrayConst>()) {
    for (JsonVariantConst e : v.as<JsonArrayConst>()) out.push_back(str(e));
  } else if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    if (s[0] == '[') {
      PDoc d;
      if (!deserializeJson(d, s))
        for (JsonVariantConst e : d.as<JsonArrayConst>()) out.push_back(str(e));
    }
  }
}

static void onWsText(const uint8_t *payload, size_t len) {
  if (len == 20 && !memcmp(payload, "{\"type\":\"heartbeat\"}", 20)) return;
  static PDoc filter;
  if (filter.isNull()) {
    filter["type"] = true;
    JsonObject d = filter["data"].to<JsonObject>();
    for (const char *k : {"hash", "observer_id", "observer_name", "snr", "path_json", "resolved_path", "decoded_json"}) d[k] = true;
    d["decoded"]["header"]["payloadTypeName"] = true;
    d["decoded"]["path"]["hops"] = true;
    JsonObject pl = d["decoded"]["payload"].to<JsonObject>();
    for (const char *k : {"pubKey", "name", "lat", "lon", "channel", "sender", "text"}) pl[k] = true;
  }
  PDoc doc;
  if (deserializeJson(doc, payload, len, DeserializationOption::Filter(filter))) return;
  if (strcmp(doc["type"] | "", "packet") != 0) return;
  JsonObjectConst d = doc["data"];
  Event *e = new Event();
  e->type = EV_PACKET;
  st::PacketIn &p = e->pkt;
  p.hash = str(d["hash"]);
  p.observerId = str(d["observer_id"]);
  p.observerName = str(d["observer_name"]);
  double snr = num(d["snr"]);
  if (!std::isnan(snr)) {
    p.hasSnr = true;
    p.snr = (float)snr;
  }
  p.typeName = d["decoded"]["header"]["payloadTypeName"] | "UNKNOWN";
  PDoc pj;
  JsonObjectConst pl = d["decoded"]["payload"];
  if (pl.isNull() && d["decoded_json"].is<const char *>()) {
    if (!deserializeJson(pj, d["decoded_json"].as<const char *>())) pl = pj.as<JsonObjectConst>();
  }
  p.pubKey = str(pl["pubKey"]);
  p.name = str(pl["name"]);
  p.channel = str(pl["channel"]);
  p.sender = str(pl["sender"]);
  p.text = str(pl["text"]);
  double lat = num(pl["lat"]), lon = num(pl["lon"]);
  if (validLL(lat, lon)) {
    p.hasLoc = true;
    p.lat = lat;
    p.lon = lon;
  }
  strList(d["path_json"], p.hops);
  if (p.hops.empty()) strList(d["decoded"]["path"]["hops"], p.hops);
  strList(d["resolved_path"], p.resolved);
  if (p.resolved.size() != p.hops.size()) p.resolved.clear();
  post(e);
}

// ================================================================ CoreScope REST
static bool loadDirectory() {
  setStatus(st::CONN_WAIT, "Loading observers...");
  {
    PDoc f, doc;
    JsonObject o = f["observers"][0].to<JsonObject>();
    for (const char *k : {"id", "name", "iata", "lat", "lon", "last_seen", "last_packet_at", "packetsLastHour"}) o[k] = true;
    int code = getJsonRetry(base() + "/api/observers", doc, &f);
    if (code != 200) {
      char tlsErr[100] = "";
      tlsApi.lastError(tlsErr, sizeof tlsErr);
      Serial.printf("[corescope] %s/api/observers -> %d  tls: %s  internal free %u (largest %u)\n", base().c_str(), code, tlsErr,
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      std::string why = code == 404 ? "server says not found - check the address" : code > 0 ? "server error " + std::to_string(code)
                        : code == -2 ? "reply couldn't be read" : code == -3 ? "reply too big" : "no connection (secure connection failed)";
      if (code < 0 && tlsErr[0]) why += std::string(" - ") + tlsErr;
      setStatus(st::CONN_ERR, "Server: " + why);
      return false;
    }
    Event *e = new Event();
    e->type = EV_OBSERVERS;
    for (JsonObjectConst ob : doc["observers"].as<JsonArrayConst>()) {
      st::Observer o2;
      o2.id = str(ob["id"]);
      o2.name = str(ob["name"]);
      o2.iata = str(ob["iata"]);
      double la = num(ob["lat"]), lo = num(ob["lon"]);
      if (validLL(la, lo)) {
        o2.hasLoc = true;
        o2.lat = la;
        o2.lon = lo;
      }
      o2.last = std::max(parseTime(ob["last_packet_at"]), parseTime(ob["last_seen"]));
      o2.pkh = (int)num(ob["packetsLastHour"], 0);
      e->observers.push_back(o2);
    }
    post(e);
  }
  PDoc f;
  nodeFilter(f, "nodes");
  f["hasMore"] = true;
  f["has_more"] = true;  // CoreScope's actual field name
  f["total"] = true;
  int total = 0;
  for (int offset = 0, page = 0; page < 40; page++) {
    setStatus(st::CONN_WAIT, "Loading nodes... " + std::to_string(total));
    PDoc doc;
    int code = getJsonRetry(base() + "/api/nodes?limit=500&offset=" + std::to_string(offset), doc, &f, 25000);
    if (code != 200) {
      if (total == 0) {
        setStatus(st::CONN_ERR, "Node list failed (HTTP " + std::to_string(code) + ")");
        return false;
      }
      break;
    }
    Event *e = new Event();
    e->type = EV_NODES;
    for (JsonObjectConst o : doc["nodes"].as<JsonArrayConst>()) {
      RawNode n = rawNode(o);
      if (!n.key.empty()) e->nodes.push_back(std::move(n));
      if ((e->nodes.size() & 63) == 0) vTaskDelay(1);  // let the UI run between chunks
    }
    int got = (int)e->nodes.size();
    total += got;
    post(e);
    offset += got;
    bool more = (doc["has_more"] | false) || (doc["hasMore"] | false);
    Serial.printf("[nodes] page %d: %d nodes (total %d, more=%d)  internal free %u (lowest %u)\n", page, got, total, more,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    if (!more || got == 0) break;
    ws.loop();
  }
  Event *e = new Event();
  e->type = EV_DIR_DONE;
  post(e);
  return true;
}

static void geocode() {
  Event *e = new Event();
  e->type = EV_CENTER;
  e->text = cfg.s.area;
  double a, b;
  char tail;
  if (sscanf(cfg.s.area.c_str(), "%lf , %lf %c", &a, &b, &tail) == 2 && validLL(a, b)) {
    e->ok = true;
    e->lat = a;
    e->lon = b;
  } else {
    // A bare postcode ("10115", "10115-1234", "K1A 0B1") is ambiguous worldwide (many countries use
    // 5-digit codes). Search it as a postcode in the country the panel's time zone suggests first.
    std::string q = cfg.s.area;
    while (!q.empty() && isspace((unsigned char)q.back())) q.pop_back();
    while (!q.empty() && isspace((unsigned char)q.front())) q.erase(0, 1);
    bool digits = !q.empty(), postcodeLike = q.size() >= 3 && q.size() <= 10;
    for (char c : q) {
      if (!isdigit((unsigned char)c) && c != '-' && c != ' ') digits = false;
      if (!isalnum((unsigned char)c) && c != '-' && c != ' ') postcodeLike = false;
    }
    postcodeLike = postcodeLike && (digits || std::any_of(q.begin(), q.end(), ::isdigit));
    static const char *naZones[] = {"EST5", "CST6", "MST7", "PST8", "AKST9", "HST10", "AST4", "NST3"};
    bool northAmerica = false;
    for (const char *z : naZones) northAmerica |= cfg.tz.rfind(z, 0) == 0;
    std::vector<std::string> tries;
    const std::string api = "https://nominatim.openstreetmap.org/search?format=json&limit=1";
    if (postcodeLike && northAmerica) tries.push_back(api + "&countrycodes=us,ca&postalcode=" + urlEnc(q));
    tries.push_back(api + "&q=" + urlEnc(q));
    for (auto &url : tries) {
      PDoc f, doc;
      f[0]["lat"] = true;
      f[0]["lon"] = true;
      f[0]["display_name"] = true;
      int code = getJson(url, doc, &f, 10000);
      if (code == 200 && doc.size() > 0) {
        double la = num(doc[0]["lat"]), lo = num(doc[0]["lon"]);
        if (validLL(la, lo)) {
          Serial.printf("[area] \"%s\" -> %s (%.4f, %.4f)\n", q.c_str(), str(doc[0]["display_name"]).c_str(), la, lo);
          e->ok = true;
          e->lat = la;
          e->lon = lo;
          break;
        }
      }
    }
  }
  post(e);
}

static void doChannels() {
  PDoc f, doc;
  JsonObject o = f["channels"][0].to<JsonObject>();
  for (const char *k : {"hash", "name", "lastMessage", "lastSender", "messageCount", "lastActivity"}) o[k] = true;
  if (getJson(base() + "/api/channels", doc, &f) != 200) return;
  Event *e = new Event();
  e->type = EV_CHANNELS;
  for (JsonObjectConst c : doc["channels"].as<JsonArrayConst>()) {
    st::ChannelInfo ci;
    ci.hash = str(c["hash"]);
    ci.name = str(c["name"]);
    if (ci.hash.empty()) ci.hash = ci.name;
    if (ci.name.empty()) ci.name = ci.hash;
    if (ci.hash.empty()) continue;
    ci.last = str(c["lastMessage"]).substr(0, 80);
    ci.who = str(c["lastSender"]).substr(0, 30);
    ci.count = (int)num(c["messageCount"], 0);
    ci.ts = parseTime(c["lastActivity"]);
    e->channels.push_back(ci);
  }
  post(e);
}

static void doHistory(const std::string &name, const std::string &hash) {
  PDoc f, doc;
  JsonObject o = f["messages"][0].to<JsonObject>();
  for (const char *k : {"sender", "text", "timestamp", "first_seen", "repeats", "observers", "hops", "snr", "packetHash"}) o[k] = true;
  if (getJson(base() + "/api/channels/" + urlEnc(hash.empty() ? name : hash) + "/messages?limit=60", doc, &f) != 200) return;
  Event *e = new Event();
  e->type = EV_HISTORY;
  e->text = name;
  for (JsonObjectConst m : doc["messages"].as<JsonArrayConst>()) {
    st::ChatMsg c;
    c.id = str(m["packetHash"]);
    c.who = str(m["sender"]).substr(0, 30);
    if (c.who.empty()) c.who = "unknown";
    c.text = str(m["text"]).substr(0, 300);
    c.ts = parseTime(m["timestamp"]);
    if (!c.ts) c.ts = parseTime(m["first_seen"]);
    if (c.id.empty()) c.id = std::to_string(c.ts);
    c.rep = (int)num(m["repeats"], 1);
    double h = num(m["hops"]);
    c.hops = std::isnan(h) ? -1 : (int)h;
    double s = num(m["snr"]);
    if (!std::isnan(s)) {
      c.hasSnr = true;
      c.snr = (float)s;
    }
    for (JsonVariantConst ob : m["observers"].as<JsonArrayConst>())
      if (c.obs.size() < 12) c.obs.push_back(str(ob).substr(0, 30));
    e->msgs.push_back(c);
  }
  std::sort(e->msgs.begin(), e->msgs.end(), [](const st::ChatMsg &a, const st::ChatMsg &b) { return a.ts < b.ts; });
  post(e);
}

// My Nodes asks for every card whenever the list changes; an answer less than a minute old is reused
// instead of downloading the same ~30 KB again.
static std::map<std::string, std::pair<Health, uint32_t>> healthCache;
static void doHealth(const std::vector<std::string> &keys) {
  Event *e = new Event();
  e->type = EV_HEALTH;
  if (healthCache.size() > 40) healthCache.clear();
  PDoc f;
  for (const char *k : {"name", "role", "lat", "lon", "last_seen"}) f["node"][k] = true;
  for (const char *k : {"lastHeard", "avgSnr", "packetsToday"}) f["stats"][k] = true;
  f["observers"][0]["id"] = true;
  for (auto &k : keys) {
    auto hit = healthCache.find(k);
    if (hit != healthCache.end() && millis() - hit->second.second < 60000) {
      e->health.push_back(hit->second.first);
      continue;
    }
    PDoc doc;
    Health h;
    h.key = k;
    if (getJson(base() + "/api/nodes/" + urlEnc(k) + "/health", doc, &f, 10000) == 200) {
      h.name = str(doc["node"]["name"]);
      h.role = str(doc["node"]["role"]);
      h.last = std::max(parseTime(doc["stats"]["lastHeard"]), parseTime(doc["node"]["last_seen"]));
      double s = num(doc["stats"]["avgSnr"]);
      if (!std::isnan(s)) {
        h.hasSnr = true;
        h.snr = (float)s;
      }
      double t = num(doc["stats"]["packetsToday"]);
      h.today = std::isnan(t) ? -1 : (int)t;
      if (doc["observers"].is<JsonArrayConst>()) h.observers = (int)doc["observers"].size();
      double la = num(doc["node"]["lat"]), lo = num(doc["node"]["lon"]);
      if (validLL(la, lo)) {
        h.hasLoc = true;
        h.lat = la;
        h.lon = lo;
      }
      healthCache[k] = {h, millis()};
    }
    e->health.push_back(h);
    ws.loop();
  }
  post(e);
}

static void doSearch(const std::string &q) {
  PDoc f, doc;
  nodeFilter(f, "nodes");
  if (getJson(base() + "/api/nodes/search?q=" + urlEnc(q), doc, &f, 8000) != 200) return;
  Event *e = new Event();
  e->type = EV_SEARCH;
  e->text = q;
  for (JsonObjectConst o : doc["nodes"].as<JsonArrayConst>()) {
    RawNode n = rawNode(o);
    if (!n.key.empty()) e->nodes.push_back(n);
  }
  post(e);
}

// ================================================================ SD card tile cache (offline maps)
static SPIClass sdSpi(FSPI);
static std::atomic<bool> sdOk{false};
static uint32_t sdTiles = 0;
// Where tiles live on the card, the same places WadaMesh reads (so one card serves both):
//   /maps/<name>/<name>.pmtiles   one-file packs (PNG or JPEG tiles)
//   /maps/osm/z/x/y.png           the shared folder library; downloaded tiles are saved here too
//   /tiles/z/x/y.jpg (or .png)    WadaMesh's own folder
//   /scopething/tiles/osm/...     this panel's cache before v3.0.6 (still read)
static std::vector<st::PmTiles *> packs;
static std::atomic<bool> hasMapsOsm{false};
static bool hasTilesDir = false, hasOldCache = false;
static std::atomic<bool> sdChecked{false};  // the card has been looked for (mounted or not)
static uint64_t sdFreeBytes = 0;
static void sdSummary() {
  char b[200];
  int n = snprintf(b, sizeof b, "%.1f GB free", sdFreeBytes / 1e9);
  for (size_t i = 0; i < packs.size() && n < (int)sizeof b - 48; i++)
    n += snprintf(b + n, sizeof b - n, "  |  map pack %s (zoom %d-%d)", packs[i]->name.c_str(), packs[i]->minZoom, packs[i]->maxZoom);
  if (packs.empty() && !hasMapsOsm && !hasTilesDir) n += snprintf(b + n, sizeof b - n, "  |  no map packs; maps you look at are kept");
  else if (hasMapsOsm || hasTilesDir) n += snprintf(b + n, sizeof b - n, "  |  map folders: %s%s%s", hasMapsOsm ? "maps/osm" : "",
                                                    hasMapsOsm && hasTilesDir ? ", " : "", hasTilesDir ? "tiles" : "");
  if (sdTiles) snprintf(b + n, sizeof b - n, "  |  %u tiles saved since start", (unsigned)sdTiles);
  std::lock_guard<std::mutex> g(sdInfoMtx);
  sdInfo = b;
}
static void sdFree() {  // through ESP-IDF, which knows FAT32 and exFAT alike
  uint64_t total = 0, freeB = 0;
  if (esp_vfs_fat_info("/sd", &total, &freeB) == ESP_OK) sdFreeBytes = freeB;
}
static void sdFindPacks() {
  File maps = SD.open("/maps");
  if (!maps || !maps.isDirectory()) return;
  for (File d = maps.openNextFile(); d && packs.size() < 3; d = maps.openNextFile()) {
    if (!d.isDirectory()) continue;
    std::string name = d.name();
    size_t slash = name.rfind('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    std::string path = "/maps/" + name + "/" + name + ".pmtiles";
    if (!SD.exists(path.c_str())) continue;
    File *f = new File(SD.open(path.c_str(), FILE_READ));
    if (!*f) {
      delete f;
      continue;
    }
    auto *pm = new st::PmTiles();
    pm->name = name;
    pm->read = [f](uint64_t off, uint32_t len, uint8_t *dst) {  // the first 2 GB of a file (ESP-IDF's file offsets are 32-bit signed)
      if (off + len > 0x7FFFFFFFull || !f->seek((uint32_t)off)) return false;
      return f->read(dst, len) == len;
    };
    if (pm->open()) {
      packs.push_back(pm);
      Serial.printf("[sd] pack %s: z%d-%d, %llu tiles, %s, %llu MB%s\n", name.c_str(), pm->minZoom, pm->maxZoom, (unsigned long long)pm->tiles,
                    pm->tileType == 3 ? "JPEG" : "PNG", (unsigned long long)(pm->bytes >> 20),
                    pm->bytes > 0x7FFFFFFFull ? " - over 2 GB: tiles past 2 GB can't be read, split it into smaller packs" : "");
    } else {
      Serial.printf("[sd] %s: can't use it (%s)\n", path.c_str(), pm->error.c_str());
      f->close();
      delete f;
      delete pm;
    }
  }
}
static void sdBegin() {
  // CrowPanel Advance 7" microSD: SCK 5, MISO 4, MOSI 6, CS 0 (Elecrow lesson 4). On V1.3 and later the
  // card only works with the switches on the back set to TF card (S1 = 1, S0 = 1).
  if (!sdSpi.begin(5, 4, 6, 0)) return;
  if (!SD.begin(0, sdSpi, 40000000, "/sd", 8)) {  // up to 8 open files: packs stay open
    sdSpi.end();
    Serial.println("[sd] no card, or it isn't FAT32 / exFAT, or the switches on the back aren't set to TF card");
    std::lock_guard<std::mutex> g(sdInfoMtx);
    sdInfo = "No card found. It needs to be FAT32 or exFAT, with the switches on the back set to TF card.";
    return;
  }
  sdOk = true;
  sdOkPub = true;
  hasMapsOsm = SD.exists("/maps/osm");
  hasTilesDir = SD.exists("/tiles");
  hasOldCache = SD.exists("/scopething/tiles/osm");
  sdFindPacks();
  sdFree();
  sdSummary();
  Serial.printf("[sd] card mounted (%s), %llu MB, %llu MB free; tile folders:%s%s%s\n", SD.cardType() == CARD_SDHC ? "SDHC/SDXC" : "SD",
                SD.cardSize() / (1024 * 1024), sdFreeBytes / (1024 * 1024), hasMapsOsm ? " maps/osm" : "", hasTilesDir ? " tiles" : "",
                hasOldCache ? " scopething (old cache)" : "");
}
static std::string tilePath(const std::string &style, int z, int x, int y) {
  char b[96];
  snprintf(b, sizeof b, "/scopething/tiles/%s/%d/%d/%d.png", style.c_str(), z, x, y);
  return b;
}
static bool sdRead(const std::string &path, std::vector<uint8_t> &out) {
  if (!sdOk) return false;
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) return false;
  size_t n = f.size();
  if (n == 0 || n > 400000) {
    f.close();
    return false;
  }
  out.resize(n);
  size_t got = f.read(out.data(), n);
  f.close();
  return got == n;
}
static void sdWrite(const std::string &path, const std::vector<uint8_t> &data) {
  if (!sdOk || data.empty()) return;
  // create the folders one level at a time
  for (size_t i = 1; i < path.size(); i++)
    if (path[i] == '/') {
      std::string d = path.substr(0, i);
      if (!SD.exists(d.c_str())) SD.mkdir(d.c_str());
    }
  File f = SD.open(path.c_str(), FILE_WRITE);
  if (!f) return;
  f.write(data.data(), data.size());
  f.close();
  sdTiles++;
  sdFreeBytes = sdFreeBytes > data.size() ? sdFreeBytes - data.size() : 0;
  if (sdTiles % 25 == 1) sdSummary();
}
bool sdReady() { return sdOk; }

bool sdReadTail(const std::string &path, size_t maxBytes, std::string &out) {
  out.clear();
  if (!sdOk) return false;
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) return false;
  size_t n = f.size(), from = n > maxBytes ? n - maxBytes : 0;
  f.seek(from);
  out.resize(n - from);
  size_t got = f.read((uint8_t *)out.data(), out.size());
  f.close();
  out.resize(got);
  if (from) {  // start at a whole line
    size_t nl = out.find('\n');
    out.erase(0, nl == std::string::npos ? out.size() : nl + 1);
  }
  return true;
}
std::vector<std::string> sdListFiles(const std::string &dir) {
  std::vector<std::string> out;
  if (!sdOk) return out;
  File d = SD.open(dir.c_str());
  if (!d || !d.isDirectory()) return out;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (!f.isDirectory()) out.push_back(f.name());
    f.close();
  }
  return out;
}
static void sdAppend(const std::string &path, const std::string &line) {
  if (!sdOk || line.empty()) return;
  for (size_t i = 1; i < path.size(); i++)
    if (path[i] == '/') {
      std::string d = path.substr(0, i);
      if (!SD.exists(d.c_str())) SD.mkdir(d.c_str());
    }
  File f = SD.open(path.c_str(), FILE_APPEND);
  if (!f) return;
  f.write((const uint8_t *)line.data(), line.size());
  f.close();
}

// ================================================================ tiles
class MemStream : public Stream {
 public:
  std::vector<uint8_t, std::allocator<uint8_t>> buf;
  size_t write(uint8_t c) override {
    if (buf.size() < 400000) buf.push_back(c);
    return 1;
  }
  size_t write(const uint8_t *b, size_t n) override {
    if (buf.size() + n < 400000) buf.insert(buf.end(), b, b + n);
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

// PNG or JPEG -> RGB565 for the map (st::decodeTileRGB in core/tiles.cpp), darkened for the dark theme
static uint16_t *decodeTile(const std::vector<uint8_t> &data, bool invert) {
  // one buffer per call: the card worker and the network task both decode
  struct Buf {
    uint8_t *p = (uint8_t *)heap_caps_malloc(256 * 256 * 3, MALLOC_CAP_SPIRAM);
    ~Buf() { heap_caps_free(p); }
  } b;
  uint8_t *rgb = b.p;
  if (!rgb || data.empty() || !st::decodeTileRGB(data.data(), data.size(), rgb)) return nullptr;
  if (invert && st::tileBrightness(rgb) < 100) invert = false;  // drawn dark already (a CARTO-dark pack, say)
  uint16_t *pix = (uint16_t *)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
  if (!pix) return nullptr;
  for (int i = 0; i < 256 * 256; i++) {
    int R = rgb[i * 3], G = rgb[i * 3 + 1], B = rgb[i * 3 + 2];
    if (invert) {
      // flip lightness, keep hue, then darken: OSM becomes a dark map
      int mx = std::max(R, std::max(G, B)), mn = std::min(R, std::min(G, B));
      int d = 255 - mx - mn;
      R = std::min(255, ((R + d) * 150 >> 8) + 4);
      G = std::min(255, ((G + d) * 155 >> 8) + 6);
      B = std::min(255, ((B + d) * 170 >> 8) + 10);
    }
    pix[i] = (uint16_t)(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3));
  }
  return pix;
}

// A tile from the card: packs, then the shared folders (WadaMesh's order: JPEG before PNG in /tiles)
static bool sdTile(int z, int x, int y, std::vector<uint8_t> &out, const char *&from) {
  if (!sdOk) return false;
  for (auto *pm : packs)
    if (pm->get(z, (uint32_t)x, (uint32_t)y, out)) {
      from = "pack";
      return true;
    }
  char p[64];
  if (hasMapsOsm) {
    snprintf(p, sizeof p, "/maps/osm/%d/%d/%d.png", z, x, y);
    if (sdRead(p, out)) return (from = "maps/osm", true);
  }
  if (hasTilesDir) {
    snprintf(p, sizeof p, "/tiles/%d/%d/%d.jpg", z, x, y);
    if (sdRead(p, out)) return (from = "tiles", true);
    snprintf(p, sizeof p, "/tiles/%d/%d/%d.png", z, x, y);
    if (sdRead(p, out)) return (from = "tiles", true);
  }
  if (hasOldCache && sdRead(tilePath("osm", z, x, y), out)) return (from = "old cache", true);
  return false;
}

static std::atomic<bool> hasCarto{false};  // a CARTO key is set (cfg belongs to the network task)

// A tile from the card for this request: CARTO's own saved dark tiles first (when a key is set), then the
// packs and folders. invert: the tile is a normal light map that must be darkened for night.
static bool cardTile(const Req &r, std::vector<uint8_t> &png, const char *&from, bool &invert) {
  bool dark = r.style == "dark";
  if (dark && hasCarto && sdRead(tilePath("carto", r.z, r.x, r.y), png)) {
    from = "SD (CARTO)";
    invert = false;
    return true;
  }
  invert = dark;
  return sdTile(r.z, r.x, r.y, png, from);
}

// Decode, hand to the map, log, and keep a downloaded tile on the card.
static void finishTile(const Req &r, std::vector<uint8_t> &png, bool fromSd, const char *from, bool invert, int expectLen,
                       bool carto) {
  Event *e = new Event();
  e->type = EV_TILE;
  e->z = r.z;
  e->x = r.x;
  e->y = r.y;
  e->gen = r.gen;
  e->style = r.style;
  e->pix = decodeTile(png, invert);
  e->ok = e->pix != nullptr;
  {
    static std::mutex logMtx;
    std::lock_guard<std::mutex> g(logMtx);
    static int logged = 0;
    static std::string loggedStyle;
    if (r.style != loggedStyle) {  // day <-> night: log the new style's first tiles too
      loggedStyle = r.style;
      logged = 0;
    }
    if (logged < 30) {
      logged++;
      uint32_t sum = 0;
      if (e->pix)
        for (int i = 0; i < 256 * 256; i += 97) sum = sum * 31 + e->pix[i];
      char cut[48] = "";
      if (!fromSd && expectLen > 0 && (int)png.size() < expectLen) snprintf(cut, sizeof cut, " (cut short: %d expected)", expectLen);
      Serial.printf("[tile] %s %d/%d/%d from %s: %u bytes%s, decode %s, sum %08x, starts %02x%02x%02x%02x\n", r.style.c_str(), r.z, r.x, r.y,
                    from, (unsigned)png.size(), cut, e->ok ? "ok" : "FAILED", (unsigned)sum, png.size() > 3 ? png[0] : 0,
                    png.size() > 3 ? png[1] : 0, png.size() > 3 ? png[2] : 0, png.size() > 3 ? png[3] : 0);
    }
  }
  bool keep = e->ok && !fromSd;
  post(e);  // on screen first; saving to the card can wait a moment
  if (keep) {  // OSM tiles into the shared library (WadaMesh reads it too), CARTO's apart
    if (carto) sdWrite(tilePath("carto", r.z, r.x, r.y), png);
    else {
      char p[64];
      snprintf(p, sizeof p, "/maps/osm/%d/%d/%d.png", r.z, r.x, r.y);
      sdWrite(p, png);
      hasMapsOsm = true;
    }
  }
}

// The network task's part: download what the card didn't have.
static void doTile(const Req &r, bool online) {
  if (r.gen != tileGen) return;  // the view moved on
  bool carto = r.style == "dark" && !cfg.cartoKey.empty();
  std::vector<uint8_t> png;
  const char *from = online ? "net" : "nowhere (offline)";
  bool invert = r.style == "dark" && !carto;
  bool fromSd = !r.sdTried && cardTile(r, png, from, invert);  // normally the card worker has looked already
  int expectLen = -1;  // Content-Length of a download, to tell a cut-off download from a bad picture
  if (!fromSd && online) {
    char b[200];
    if (carto) snprintf(b, sizeof b, "https://basemaps.cartocdn.com/rastertiles/dark_all/%d/%d/%d.png?key=%s", r.z, r.x, r.y, cfg.cartoKey.c_str());
    else snprintf(b, sizeof b, "https://tile.openstreetmap.org/%d/%d/%d.png", r.z, r.x, r.y);
    HTTPClient http;
    tlsTile.setCACertBundle(ca_bundle_start, ca_bundle_end - ca_bundle_start);
    http.setReuse(true);
    if (http.begin(tlsTile, b)) {
      http.setUserAgent(UA);
      http.setTimeout(10000);
      if (http.GET() == 200) {
        MemStream ms;
        ms.buf.reserve(40000);
        int len = http.getSize();
        expectLen = len;
        if (len > 0) readBody(http, ms, len, 10000);  // yields while waiting (see readBody)
        else http.writeToStream(&ms);                  // chunked reply: let HTTPClient decode it
        png.swap(ms.buf);
      }
      http.end();
    }
  }
  finishTile(r, png, fromSd, from, invert, expectLen, carto);
}

// The card worker: map tiles from the SD card, on their own task, so they never wait behind a slow
// CoreScope download or a Wi-Fi reconnect (the map used to show only a grid while the panel said
// CONNECTING). Tiles the card doesn't have go on to the network task to download.
static void cardTask(void *) {
  while (!sdChecked) vTaskDelay(pdMS_TO_TICKS(50));
  for (;;) {
    Req *r;
    if (xQueueReceive(sdQ, &r, portMAX_DELAY) != pdTRUE) continue;
    if (r->type == RQ_APPEND) {
      sdAppend(r->a, r->b);
      delete r;
      continue;
    }
    if (r->type == RQ_READ) {
      Event *e = new Event();
      e->type = EV_FILE;
      e->text = r->a;
      e->ok = sdReadTail(r->a, 120000, e->blob);
      post(e);
      delete r;
      continue;
    }
    if (r->gen != tileGen) {  // the view moved on
      delete r;
      continue;
    }
    std::vector<uint8_t> png;
    const char *from = "SD";
    bool invert = false;
    if (sdOk && cardTile(*r, png, from, invert)) {
      finishTile(*r, png, true, from, invert, -1, false);
      delete r;
    } else {
      r->sdTried = true;
      request(r);  // -> the network task
    }
  }
}

// ================================================================ WebSocket
static uint32_t wsLastMsg = 0;
static bool wsStarted = false;
static void wsEvent(WStype_t type, uint8_t *payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      wsLastMsg = millis();
      setStatus(st::CONN_LIVE, "Live from " + cfg.s.server.substr(cfg.s.server.find("//") == std::string::npos ? 0 : cfg.s.server.find("//") + 2));
      break;
    case WStype_DISCONNECTED:
      if (wsStarted) setStatus(st::CONN_WAIT, "Live feed disconnected, retrying...");
      break;
    case WStype_TEXT:
      wsLastMsg = millis();
      onWsText(payload, len);
      break;
    default:
      break;
  }
}
static void startWs() {
  if (coreScopeOff()) return;  // radio only: no live feed from CoreScope
  std::string b = base();
  bool tls = b.rfind("https", 0) == 0;
  std::string rest = b.substr(tls ? 8 : 7);
  size_t slash = rest.find('/');
  std::string hostPort = rest.substr(0, slash), path = slash == std::string::npos ? "" : rest.substr(slash);
  int port = tls ? 443 : 80;
  size_t colon = hostPort.find(':');
  std::string host = hostPort.substr(0, colon);
  if (colon != std::string::npos) port = atoi(hostPort.c_str() + colon + 1);
  static std::string sHost, sUrl;
  sHost = host;
  sUrl = path + "/ws";
  ws.disconnect();
  if (tls) ws.beginSslWithBundle(sHost.c_str(), port, sUrl.c_str(), ca_bundle_start, ca_bundle_end - ca_bundle_start, "");
  else ws.begin(sHost.c_str(), port, sUrl.c_str(), "");
  ws.setExtraHeaders((std::string("User-Agent: ") + UA).c_str());
  ws.onEvent(wsEvent);
  ws.setReconnectInterval(5000);
  ws.enableHeartbeat(30000, 10000, 2);
  wsStarted = true;
  wsLastMsg = millis();
  setStatus(st::CONN_WAIT, "Connecting to live feed...");
}

// ================================================================ Wi-Fi setup helpers
static std::atomic<int> lastReason{0};
// What the router-side reason codes mean in practice (ESP-IDF wifi_err_reason_t). Codes 2 and 4 with a
// strong signal usually mean the router itself was restarting or busy, not that the password is wrong.
static const char *reasonText(int r) {
  switch (r) {
    case 201: case 210: case 211: case 212: return "network not found (out of range, or 5 GHz only?)";
    case 202: case 204: case 205: return "the router didn't accept the login (password, or a security-mode mismatch)";
    case 15: return "the login handshake timed out (wrong password, or the router was busy)";
    case 2: return "the router didn't answer the login (restarting, or busy?)";
    case 3: return "the router ended the connection";
    case 4: return "the router dropped the connection (did it restart?)";
    case 8: return "disconnected";
    case 203: return "the router refused the connection";
    case 200: return "signal lost (router off, or out of range?)";
    case 0: return "no answer from the router";
    default: return "connection failed";
  }
}
static void onWifiEvent(arduino_event_id_t ev, arduino_event_info_t info) {
  if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    int r = info.wifi_sta_disconnected.reason;
    lastReason = r;
    Serial.printf("[wifi] disconnected, reason %d: %s (rssi %d)\n", r, reasonText(r), info.wifi_sta_disconnected.rssi);
  } else if (ev == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    Serial.println("[wifi] associated, waiting for an IP address...");
  } else if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf("[wifi] got IP %s\n", WiFi.localIP().toString().c_str());
    lastReason = 0;
  }
}
static void wifiStatus(const std::string &t, bool ok, bool trying) {
  Serial.printf("[wifi] %s\n", t.c_str());
  Event *e = new Event();
  e->type = EV_WIFI_STATUS;
  e->text = t;
  e->ok = ok;
  e->x = trying ? 1 : 0;
  post(e);
}
RTC_NOINIT_ATTR static uint32_t phyHealFlag;  // see healRadioIfDeaf()
static constexpr uint32_t PHY_HEALED = 0x5C0FE11u;

static void doWifiScan() {
  Serial.println("[wifi] scanning...");
  WifiBusy busy;
  int n = WiFi.scanNetworks(false, false);
  Event *e = new Event();
  e->type = EV_WIFI_SCAN;
  for (int i = 0; i < n; i++) {
    std::string ssid = WiFi.SSID(i).c_str();
    if (ssid.empty()) continue;
    bool dup = false;
    for (auto &w : e->wifi) dup |= w.ssid == ssid;
    if (dup) continue;
    WifiNet w;
    w.ssid = ssid;
    w.rssi = WiFi.RSSI(i);
    w.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    e->wifi.push_back(w);
  }
  WiFi.scanDelete();
  std::sort(e->wifi.begin(), e->wifi.end(), [](const WifiNet &a, const WifiNet &b) { return a.rssi > b.rssi; });
  if (e->wifi.size() > 20) e->wifi.resize(20);
  Serial.printf("[wifi] %d networks\n", (int)e->wifi.size());
  if (n > 0) phyHealFlag = 0;
  post(e);
}
// Connect with the given credentials (saved by the Wi-Fi driver for next boot). Blocks up to ~20 s.
// ---- Wi-Fi receiver self-heal
// The ESP32 saves its Wi-Fi radio calibration in flash and reuses it on every start. If that data is
// corrupted the receiver hears nothing at all (every scan: 0 networks, RSSI -128), even after a power
// cycle. If a scan finds nothing, wipe the calibration once and restart: the chip then recalibrates
// from scratch. The flag lives in RTC memory, so a genuinely Wi-Fi-less room can't cause a loop.
static int quickScanCount() {
  int n = WiFi.scanNetworks(false, true);
  WiFi.scanDelete();
  return n < 0 ? 0 : n;
}
static void healRadioIfDeaf(const char *why) {
  if (quickScanCount() > 0) {
    phyHealFlag = 0;  // receiver is fine
    return;
  }
  if (phyHealFlag == PHY_HEALED) {
    Serial.printf("[wifi] still no networks after recalibrating (%s); leaving it\n", why);
    return;
  }
  Serial.printf("[wifi] %s and a scan hears no networks at all: wiping the radio calibration and restarting\n", why);
  phyHealFlag = PHY_HEALED;
  esp_phy_erase_cal_data_in_nvs();
  delay(300);
  ESP.restart();
}

// WiFiManager's stopConfigPortal() dereferences its web server, which is gone once the portal has
// been stopped: calling it twice crashed the panel (v2.1.18-v2.1.25) whenever the saved network was
// slow to appear at boot. Only stop a portal that is actually running.
static void stopPortalSafe() {
  if (wm.getConfigPortalActive()) wm.stopConfigPortal();
}

static bool isHandshakeFail(int r) { return r == 202 || r == 15 || r == 204 || r == 2 || r == 205 || r == 0; }

// Some ESP32-S3 boards can't finish the Wi-Fi handshake at full transmit power (the signal overloads
// nearby routers / the board's own antenna path). Once a lower power is known to work we remember it.
static bool lowTxSaved() {
  // read with the NVS API directly: Preferences prints an alarming "nvs_open failed: NOT_FOUND" when the
  // note was never needed (full power has always worked), which is the usual case
  nvs_handle_t h;
  if (nvs_open("wifi_x", NVS_READONLY, &h) != ESP_OK) return false;
  uint8_t v = 0;
  bool on = nvs_get_u8(h, "lowtx", &v) == ESP_OK && v;  // Preferences stores a bool as u8
  nvs_close(h);
  return on;
}
static void saveLowTx(bool on) {
  Preferences p;
  p.begin("wifi_x", false);
  p.putBool("lowtx", on);
  p.end();
}
static bool tryJoin(const std::string &ssid, const std::string &pass, bool lowTx, int ms) {
  WiFi.disconnect(false, false);
  delay(150);
  lastReason = 0;
  WiFi.begin(ssid.c_str(), pass.c_str());
  if (lowTx) WiFi.setTxPower(WIFI_POWER_8_5dBm);
  uint32_t t0 = millis();
  while (millis() - t0 < (uint32_t)ms && WiFi.status() != WL_CONNECTED) delay(100);
  Serial.printf("[wifi] try %s lowTx=%d -> %s (reason %d)\n", ssid.c_str(), lowTx, WiFi.status() == WL_CONNECTED ? "OK" : "fail", lastReason.load());
  return WiFi.status() == WL_CONNECTED;
}

// Connect with the given credentials (saved by the Wi-Fi driver for next boot). Tries normal power,
// then reduced transmit power, then the password with stray spaces trimmed.
static bool doWifiConnect(const std::string &ssid, const std::string &pass, bool &portal) {
  WifiBusy busy;
  if (portal) {
    stopPortalSafe();
    portal = false;
  }
  wifiStatus("Connecting to " + ssid + "...", false, true);
  WiFi.persistent(true);
  WiFi.mode(WIFI_STA);
  bool low = lowTxSaved();
  bool ok = tryJoin(ssid, pass, low, 15000);
  if (!ok && isHandshakeFail(lastReason) && !low) {
    wifiStatus("Retrying " + ssid + " with lower radio power...", false, true);
    ok = tryJoin(ssid, pass, true, 15000);
    if (ok) saveLowTx(true);
  }
  std::string trimmed = pass;
  while (!trimmed.empty() && isspace((unsigned char)trimmed.back())) trimmed.pop_back();
  while (!trimmed.empty() && isspace((unsigned char)trimmed.front())) trimmed.erase(0, 1);
  if (!ok && isHandshakeFail(lastReason) && trimmed != pass) {
    wifiStatus("Retrying without the extra spaces in the password...", false, true);
    ok = tryJoin(ssid, trimmed, true, 15000);
    if (ok) saveLowTx(true);
  }
  if (ok) {
    wifiStatus("Connected to " + ssid, true, false);
    return true;
  }
  int r = lastReason;
  std::string why = reasonText(r);
  if (isHandshakeFail(r))
    why += ". Check the password (it's case-sensitive). If it's right, set the router's 2.4 GHz security to "
           "\"WPA2 Personal (AES)\" rather than WPA2/WPA3 mixed, and turn off \"PMF required\"";
  wifiStatus("Couldn't connect to " + ssid + ": " + why + " (code " + std::to_string(r) + ").", false, false);
  WiFi.disconnect(false, false);
  return false;
}
static void startPortal(bool &portal) {
  wm.setConfigPortalBlocking(false);
  wm.startConfigPortal(PORTAL_SSID);
  portal = true;
  Event *e = new Event();
  e->type = EV_PORTAL;
  e->ok = true;
  e->text = PORTAL_SSID;
  post(e);
}

// ================================================================ task
static void task(void *) {
  sdBegin();
  sdChecked = true;
  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("meshtop");
  wm.setConfigPortalBlocking(false);
  wm.setTitle("MESHtop setup");
  wm.setConnectTimeout(20);
  wm.setDarkMode(true);
  wm.setConfigPortalTimeout(300);  // router down? retry the saved network every 5 minutes
  std::string savedSsid = wm.getWiFiSSID(true).c_str();
  bool portal;
  wifiBusyN++;  // boot-time join: keep Bluetooth quiet until it's done
  if (lowTxSaved() && !savedSsid.empty()) {
    portal = !tryJoin(savedSsid, wm.getWiFiPass(true).c_str(), true, 20000);
    if (portal) portal = !wm.autoConnect(PORTAL_SSID);
  } else {
    portal = !wm.autoConnect(PORTAL_SSID);
    // saved network failed at full power with a handshake error: try once at lower power
    if (portal && !savedSsid.empty() && isHandshakeFail(lastReason)) {
      stopPortalSafe();
      WiFi.mode(WIFI_STA);
      if (tryJoin(savedSsid, wm.getWiFiPass(true).c_str(), true, 20000)) {
        saveLowTx(true);
        portal = false;
      } else {
        portal = !wm.autoConnect(PORTAL_SSID);
      }
    }
  }
  // The first join after flashing often fails while the radio calibrates; before showing the setup
  // portal, retry the saved network a couple more times.
  for (int attempt = 0; portal && !savedSsid.empty() && attempt < 2; attempt++) {
    Serial.printf("[wifi] retrying saved network %s (%d/2)...\n", savedSsid.c_str(), attempt + 1);
    stopPortalSafe();
    WiFi.mode(WIFI_STA);
    delay(2000);
    if (tryJoin(savedSsid, wm.getWiFiPass(true).c_str(), lowTxSaved(), 15000)) portal = false;
  }
  if (portal) {
    stopPortalSafe();
    WiFi.mode(WIFI_STA);
    delay(200);
    healRadioIfDeaf("couldn't join the saved network");  // restarts the panel if the radio is deaf
  }
  if (portal) {  // back to the setup portal (on a new panel too: with nothing saved it was left stopped,
                 // and the loop below restarts the panel whenever the portal isn't running)
    wm.setConnectTimeout(5);
    portal = !wm.autoConnect(PORTAL_SSID);
    wm.setConnectTimeout(20);
  }
  wifiBusyN--;
  if (portal) {
    setStatus(st::CONN_WAIT, std::string("Join Wi-Fi \"") + PORTAL_SSID + "\" to set up");
    Event *e = new Event();
    e->type = EV_PORTAL;
    e->ok = true;
    e->text = PORTAL_SSID;
    post(e);
    if (!savedSsid.empty())
      wifiStatus("Couldn't connect to " + savedSsid + ": " + reasonText(lastReason) +
                     ". Your settings are kept. If the router was restarting, wait a minute and tap it below; if its password changed, retype it.",
                 false, false);
  }
  bool online = false, needCenter = true, needDir = true;
  uint32_t lastDir = 0, lastObs = 0, dirRetryAt = 0;
  // After losing Wi-Fi we pace the retries ourselves. The Arduino default reconnects instantly after
  // every failure, so a router that is restarting (or briefly blocking a device after failed logins)
  // gets hammered several times a second and may keep refusing: seen as "reason 2" over and over.
  bool reconnecting = false;
  int reconnTry = 0;
  uint32_t reconnAt = 0, reconnStarted = 0;
  std::deque<Req *> tiles;

  for (;;) {
    hasCarto = !cfg.cartoKey.empty();
    if (portal) {
      wm.process();
      // setup requests from the panel's own Wi-Fi picker work while the hotspot is up
      Req *wr;
      while (portal && xQueueReceive(rqQ, &wr, 0) == pdTRUE) {
        if (wr->type == RQ_TILE) {  // keep for later (or the SD card)
          tiles.push_back(wr);
          continue;
        }
        if (wr->type == RQ_WIFI_SCAN) doWifiScan();
        else if (wr->type == RQ_WIFI_CONNECT) {
          if (!doWifiConnect(wr->a, wr->b, portal)) startPortal(portal);
        } else if (wr->type == RQ_CONFIG) cfg = wr->cfg;
        delete wr;
      }
      while (tiles.size() > 40) {
        delete tiles.front();
        tiles.pop_front();
      }
      if (portal && !wm.getConfigPortalActive() && WiFi.status() != WL_CONNECTED) {
        Serial.println("[wifi] setup portal closed without a connection: restarting");
        delay(100);
        ESP.restart();
      }
      if (WiFi.status() == WL_CONNECTED) {
        if (portal) wifiStatus("Connected to " + std::string(WiFi.SSID().c_str()), true, false);
        portal = false;
        Event *e = new Event();
        e->type = EV_PORTAL;
        e->ok = false;
        post(e);
      } else {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
    }
    // requests from the UI
    Req *r;
    while (xQueueReceive(rqQ, &r, 0) == pdTRUE) {
      if (r->type == RQ_TILE) {
        bool dup = false;  // the map asks again while a slow download is still queued: keep one
        for (Req *q : tiles) dup |= q->z == r->z && q->x == r->x && q->y == r->y && q->style == r->style && q->gen == r->gen;
        if (dup) delete r;
        else tiles.push_back(r);
        continue;
      }
      if (r->type == RQ_WIFI_SCAN) {
        doWifiScan();
        delete r;
        continue;
      }
      if (r->type == RQ_WIFI_CONNECT) {
        bool p = false;
        if (!doWifiConnect(r->a, r->b, p)) WiFi.begin();  // fall back to the saved network
        delete r;
        continue;
      }
      if (r->type == RQ_CONFIG) {
        bool serverChanged = r->cfg.s.server != cfg.s.server || r->cfg.s.demo != cfg.s.demo || r->cfg.s.useCoreScope != cfg.s.useCoreScope;
        bool areaChanged = r->cfg.s.area != cfg.s.area;
        bool tzChanged = r->cfg.tz != cfg.tz;
        cfg = r->cfg;
        if (areaChanged) needCenter = true;
        if (serverChanged) {
          needDir = true;
          wsStarted = false;
          ws.disconnect();
        }
        if (tzChanged && online) configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.nist.gov");
        delete r;
        continue;
      }
      if (!online || cfg.s.demo || coreScopeOff()) {  // the rest all go to CoreScope
        delete r;
        continue;
      }
      if (r->type == RQ_CHANNELS) doChannels();
      else if (r->type == RQ_HISTORY) doHistory(r->a, r->b);
      else if (r->type == RQ_HEALTH) doHealth(r->keys);
      else if (r->type == RQ_SEARCH) doSearch(r->a);
      delete r;
    }

    if (WiFi.status() != WL_CONNECTED) {
      if (online) {
        setStatus(st::CONN_ERR, "Wi-Fi lost, reconnecting...");
        WiFi.setAutoReconnect(false);
        reconnecting = false;
        reconnTry = 0;
        reconnAt = millis() + 3000;
      }
      online = false;
      if (!reconnecting && (int32_t)(millis() - reconnAt) >= 0) {
        Serial.printf("[wifi] reconnect attempt %d\n", reconnTry + 1);
        WiFi.reconnect();  // the saved network
        reconnecting = true;
        reconnStarted = millis();
      } else if (reconnecting && millis() - reconnStarted > 15000) {
        static const uint16_t gaps[] = {10, 20, 40, 60};  // seconds between tries, then every minute
        uint16_t gap = gaps[std::min(reconnTry, 3)];
        reconnecting = false;
        reconnTry++;
        reconnAt = millis() + gap * 1000u;
        Serial.printf("[wifi] still not back (%s); next try in %u s\n", reasonText(lastReason), (unsigned)gap);
        setStatus(st::CONN_ERR, std::string("Wi-Fi lost: ") + reasonText(lastReason) + ". Trying again in " + std::to_string(gap) + " s");
      }
      // offline: the map still works from tiles saved on the SD card
      while (!tiles.empty()) {
        Req *t = tiles.front();
        tiles.pop_front();
        doTile(*t, false);
        delete t;
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (!online) {
      online = true;
      if (reconnTry || reconnecting) Serial.printf("[wifi] back after %d paced tries\n", reconnTry + (reconnecting ? 1 : 0));
      reconnecting = false;
      reconnTry = 0;
      configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.nist.gov");
      static bool mdnsUp = false;
      if (!mdnsUp) {
#ifdef ST_HAS_MDNS
        mdnsUp = MDNS.begin("meshtop");
        if (mdnsUp) MDNS.addService("http", "tcp", 80);
#else
        mdnsUp = true;
#endif
      }
      web::begin();
      if (!wsStarted) setStatus(st::CONN_WAIT, "Wi-Fi connected, syncing clock...");
    }
    web::loop();
    if (!clockOk && time(nullptr) > 1700000000) {
      clockOk = true;
      Event *e = new Event();
      e->type = EV_TIME;
      post(e);
    }

    if (needCenter) {
      needCenter = false;
      geocode();
    }
    if (cfg.s.demo) {
      if (wsStarted) {
        ws.disconnect();
        wsStarted = false;
      }
      setStatus(st::CONN_DEMO, "Demo mode (no network data)");
    } else if (coreScopeOff()) {
      if (wsStarted) {
        ws.disconnect();
        wsStarted = false;
      }
      setStatus(st::CONN_LIVE, "Radio only: CoreScope is off");
    } else if (clockOk) {
      uint32_t now = millis();
      if ((needDir && (int32_t)(now - dirRetryAt) >= 0) || (lastDir && now - lastDir > 60 * 60000)) {
        if (loadDirectory()) {
          needDir = false;
          lastDir = lastObs = millis();
          if (!wsStarted) startWs();
        } else {
          dirRetryAt = millis() + 30000;
        }
      }
      if (wsStarted) {
        ws.loop();
        if (millis() - wsLastMsg > 120000) {  // stale: force a reconnect
          wsLastMsg = millis();
          ws.disconnect();
        }
      }
    }
    // one tile per pass keeps the live feed flowing
    while (!tiles.empty() && tiles.front()->gen != tileGen) {
      delete tiles.front();
      tiles.pop_front();
    }
    if (!tiles.empty()) {
      Req *t = tiles.front();
      tiles.pop_front();
      doTile(*t, true);
      delete t;
      // An idle kept-alive TLS session still holds internal RAM; let it go once the burst is done.
      if (tiles.empty() && tlsTile.connected()) tlsTile.stop();
    }
    vTaskDelay(pdMS_TO_TICKS(tiles.empty() ? 5 : 1));
  }
}

void start(const store::DeviceCfg &c) {
  cfg = c;
  hasCarto = !cfg.cartoKey.empty();
  ensureQueues();
  // Core 1, same priority as the UI. The display's refill interrupt lives on core 1 and pre-empts
  // anything there; JSON parsing and PNG decoding on core 0 used to compete with it for PSRAM and
  // starve the screen (lines, shaking). Core 0 is left to Wi-Fi and Bluetooth, which use internal RAM.
  xTaskCreatePinnedToCore(task, "net", 20480, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(cardTask, "card", 8192, nullptr, 1, nullptr, 1);  // map tiles from the SD card
}

void resetWifi() {
  wm.resetSettings();
}

}  // namespace net
