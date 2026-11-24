#include "ble_radio.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>

#include "net.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include <mutex>

namespace bleradio {

static const char *NUS_SVC = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char *NUS_RX = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";  // we write
static const char *NUS_TX = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";  // radio notifies

static QueueHandle_t evQ, wrQ;
static std::mutex cfgMtx;
static bool enabled = false;
static uint32_t pin = 123456;
static std::string nameFilter;
static std::atomic<bool> forgetReq{false};
static std::atomic<bool> linkUp{false};

static std::atomic<uint32_t> dropped{0};
// Never hold up the Bluetooth stack for long: while this waits, the radio's incoming data piles up in
// NimBLE's small buffer pool ("Failed to allocate buffer, retrying"). The queue is deep enough that the
// main loop empties it long before it fills.
static void post(Event *e, uint32_t waitMs = 50) {
  if (xQueueSend(evQ, &e, pdMS_TO_TICKS(waitMs)) != pdTRUE) {
    delete e;
    dropped++;
  }
}
uint32_t droppedFrames() { return dropped; }
static void status(st::RadioState s, const std::string &t) {
  Event *e = new Event();
  e->type = BE_STATUS;
  e->state = s;
  e->text = t;
  post(e);
}
bool poll(Event *&e) { return evQ && xQueueReceive(evQ, &e, 0) == pdTRUE; }
void write(const std::vector<uint8_t> &f) {
  if (!wrQ) return;
  auto *v = new std::vector<uint8_t>(f);
  if (xQueueSend(wrQ, &v, 0) != pdTRUE) delete v;
}
void configure(bool on, uint32_t p, const std::string &filter) {
  std::lock_guard<std::mutex> g(cfgMtx);
  enabled = on;
  pin = p ? p : 123456;
  nameFilter = filter;
}
void forget() { forgetReq = true; }

class Callbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *, int reason) override {
    linkUp = false;
    Event *e = new Event();
    e->type = BE_DISCONNECTED;
    e->text = "reason " + std::to_string(reason);
    post(e);
  }
  void onPassKeyEntry(NimBLEConnInfo &info) override {
    uint32_t p;
    {
      std::lock_guard<std::mutex> g(cfgMtx);
      p = pin;
    }
    NimBLEDevice::injectPassKey(info, p);
  }
  void onConfirmPasskey(NimBLEConnInfo &info, uint32_t) override { NimBLEDevice::injectConfirmPasskey(info, true); }
};
static Callbacks callbacks;

static void onNotify(NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
  Event *e = new Event();
  e->type = BE_FRAME;
  e->data.assign(data, data + len);
  post(e, 5);
}

static std::string lower(std::string s) {
  for (auto &c : s) c = (char)tolower((unsigned char)c);
  return s;
}

static void task(void *) {
  bool inited = false;
  NimBLEClient *client = nullptr;
  NimBLERemoteCharacteristic *rx = nullptr;
  int failures = 0;
  Preferences prefs;
  for (;;) {
    bool on;
    std::string filter;
    uint32_t p;
    {
      std::lock_guard<std::mutex> g(cfgMtx);
      on = enabled;
      filter = lower(nameFilter);
      p = pin;
    }
    if (forgetReq.exchange(false)) {
      prefs.begin("scopething", false);
      prefs.remove("radio_addr");
      prefs.end();
      if (inited) NimBLEDevice::deleteAllBonds();
      if (client && client->isConnected()) client->disconnect();
    }
    static bool wasOn = false;
    static uint32_t lastScanAt = 0, scanBackoff = 5000;
    if (!on) {
      if (client && client->isConnected()) client->disconnect();
      if (wasOn) status(st::R_OFF, "Radio off");
      wasOn = false;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    wasOn = true;
    if (!inited) {
      NimBLEDevice::init("MESHtop");
      NimBLEDevice::setMTU(512);
      NimBLEDevice::setSecurityAuth(true, true, true);  // bond, MITM (PIN), secure connections
      NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
      inited = true;
    }
    if (client && client->isConnected() && linkUp) {
      std::vector<uint8_t> *f;
      while (xQueueReceive(wrQ, &f, 0) == pdTRUE) {
        if (rx && !rx->writeValue(f->data(), f->size(), true)) Serial.println("[radio] write failed");
        delete f;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // ---- find the radio
    // Wi-Fi and Bluetooth share one antenna. Never scan while Wi-Fi is scanning/joining, scan only
    // a small fraction of the time, and back off when no radio is around.
    if (net::wifiBusy()) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (WiFi.getMode() != WIFI_OFF && !WiFi.isConnected() && millis() - lastScanAt < 30000) {
      vTaskDelay(pdMS_TO_TICKS(500));  // Wi-Fi still setting up: give it the air
      continue;
    }
    if (millis() - lastScanAt < scanBackoff) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    std::string savedAddr;
    prefs.begin("scopething", true);
    if (prefs.isKey("radio_addr")) savedAddr = prefs.getString("radio_addr", "").c_str();
    prefs.end();
    const NimBLEAdvertisedDevice *target = nullptr;
    status(st::R_SCANNING, "Looking for your MeshCore radio...");
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(160);  // ms
    scan->setWindow(30);     // listen ~19% of the time; Wi-Fi gets the rest
    NimBLEScanResults res = scan->getResults(5000, false);
    lastScanAt = millis();
    int best = -999;
    for (int i = 0; i < res.getCount(); i++) {
      const NimBLEAdvertisedDevice *d = res.getDevice(i);
      std::string nm = d->getName();
      bool meshcore = d->isAdvertisingService(NimBLEUUID(NUS_SVC)) || lower(nm).rfind("meshcore", 0) == 0;
      if (!meshcore) continue;
      if (!filter.empty() && lower(nm).find(filter) == std::string::npos) continue;
      bool isSaved = !savedAddr.empty() && d->getAddress().toString() == savedAddr;
      int score = d->getRSSI() + (isSaved ? 1000 : 0);  // prefer the radio we paired with before
      if (score > best) {
        best = score;
        target = d;
      }
    }
    if (!target) {
      status(st::R_SCANNING, filter.empty() ? "No MeshCore radio found yet. Is it on, and disconnected from the phone app?"
                                            : "No radio matching \"" + filter + "\" found yet");
      scanBackoff = std::min<uint32_t>(scanBackoff * 2, 60000);  // 5 s, 10 s, 20 s ... 1 min
      continue;
    }
    scanBackoff = 5000;
    std::string name = target->getName().empty() ? target->getAddress().toString() : target->getName();
    status(st::R_CONNECTING, "Connecting to " + name + "...");
    if (!client) {
      client = NimBLEDevice::createClient();
      client->setClientCallbacks(&callbacks, false);
      client->setConnectTimeout(12000);
    }
    if (!client->connect(target, true, false, true)) {
      status(st::R_ERROR, "Couldn't connect to " + name + ", retrying...");
      if (++failures >= 3) {
        failures = 0;
        NimBLEDevice::deleteBond(target->getAddress());
      }
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }
    status(st::R_PAIRING, "Pairing with " + name + " (PIN " + std::to_string(p) + ")...");
    if (!client->secureConnection()) {
      status(st::R_ERROR, "Pairing with " + name + " failed: wrong PIN? Tap Change PIN and type the PIN your radio shows (or the one set in the MeshCore app).");
      NimBLEDevice::deleteBond(client->getPeerAddress());
      client->disconnect();
      vTaskDelay(pdMS_TO_TICKS(6000));
      continue;
    }
    NimBLERemoteService *svc = client->getService(NUS_SVC);
    NimBLERemoteCharacteristic *tx = svc ? svc->getCharacteristic(NUS_TX) : nullptr;
    rx = svc ? svc->getCharacteristic(NUS_RX) : nullptr;
    if (!tx || !rx || !tx->subscribe(true, onNotify, true)) {
      status(st::R_ERROR, name + " doesn't look like MeshCore companion firmware");
      client->disconnect();
      vTaskDelay(pdMS_TO_TICKS(6000));
      continue;
    }
    failures = 0;
    prefs.begin("scopething", false);
    prefs.putString("radio_addr", client->getPeerAddress().toString().c_str());
    prefs.end();
    std::vector<uint8_t> *stale;
    while (xQueueReceive(wrQ, &stale, 0) == pdTRUE) delete stale;
    linkUp = true;
    Event *e = new Event();
    e->type = BE_CONNECTED;
    e->text = name;
    post(e);
  }
}

void start() {
  evQ = xQueueCreate(256, sizeof(Event *));  // a sync burst from the radio fits without waiting
  wrQ = xQueueCreate(32, sizeof(std::vector<uint8_t> *));
  xTaskCreatePinnedToCore(task, "radio", 8192, nullptr, 2, nullptr, 0);
}

}  // namespace bleradio
