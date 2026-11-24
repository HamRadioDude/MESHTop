#include "companion.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "aes128.h"
#include "sha256.h"

namespace st {

// companion protocol codes (MeshCore examples/companion_radio/MyMesh.cpp)
enum : uint8_t {
  CMD_APP_START = 1,
  CMD_SEND_TXT_MSG = 2,
  CMD_SEND_CHANNEL_TXT_MSG = 3,
  CMD_GET_CONTACTS = 4,
  CMD_SET_DEVICE_TIME = 6,
  CMD_SEND_SELF_ADVERT = 7,
  CMD_SET_ADVERT_NAME = 8,
  CMD_ADD_UPDATE_CONTACT = 9,
  CMD_SYNC_NEXT_MESSAGE = 10,
  CMD_SET_RADIO_PARAMS = 11,
  CMD_SET_RADIO_TX_POWER = 12,
  CMD_RESET_PATH = 13,
  CMD_SET_ADVERT_LATLON = 14,
  CMD_REMOVE_CONTACT = 15,
  CMD_SHARE_CONTACT = 16,
  CMD_REBOOT = 19,
  CMD_GET_BATT_AND_STORAGE = 20,
  CMD_DEVICE_QUERY = 22,
  CMD_SEND_LOGIN = 26,
  CMD_SEND_STATUS_REQ = 27,
  CMD_GET_CHANNEL = 31,
  CMD_SET_CHANNEL = 32,
  CMD_SEND_TRACE_PATH = 36,
  CMD_SET_DEVICE_PIN = 37,
  CMD_SET_OTHER_PARAMS = 38,
  CMD_SEND_BINARY_REQ = 50,
  CMD_SEND_PATH_DISCOVERY_REQ = 52,
  CMD_SEND_CONTROL_DATA = 55,  // v8+
  CMD_GET_STATS = 56,          // v8+, second byte = stats type

  RESP_OK = 0,
  RESP_ERR = 1,
  RESP_CONTACTS_START = 2,
  RESP_CONTACT = 3,
  RESP_END_OF_CONTACTS = 4,
  RESP_SELF_INFO = 5,
  RESP_SENT = 6,
  RESP_CONTACT_MSG_RECV = 7,
  RESP_CHANNEL_MSG_RECV = 8,
  RESP_NO_MORE_MESSAGES = 10,
  RESP_BATT_AND_STORAGE = 12,
  RESP_DEVICE_INFO = 13,
  RESP_CONTACT_MSG_RECV_V3 = 16,
  RESP_CHANNEL_MSG_RECV_V3 = 17,
  RESP_CHANNEL_INFO = 18,
  RESP_STATS = 24,  // v8+, second byte = stats type

  PUSH_ADVERT = 0x80,
  PUSH_PATH_UPDATED = 0x81,
  PUSH_SEND_CONFIRMED = 0x82,
  PUSH_MSG_WAITING = 0x83,
  PUSH_LOGIN_SUCCESS = 0x85,
  PUSH_LOGIN_FAIL = 0x86,
  PUSH_STATUS_RESPONSE = 0x87,
  PUSH_LOG_RX_DATA = 0x88,
  PUSH_TRACE_DATA = 0x89,
  PUSH_NEW_ADVERT = 0x8A,
  PUSH_BINARY_RESPONSE = 0x8C,
  PUSH_PATH_DISCOVERY = 0x8D,
  PUSH_CONTROL_DATA = 0x8E,  // v8+
  PUSH_CONTACT_DELETED = 0x8F,
  PUSH_CONTACTS_FULL = 0x90,
};
static constexpr int PUB_KEY = 32, MAX_PATH = 64;
static const char *ERR_TEXT[] = {"", "not supported by this radio", "not found", "radio storage is full", "radio is busy", "radio file error",
                                 "invalid value"};

// the well-known Public channel key (meshcore://channel/add?name=Public&secret=8b3387e9c5cdea6ac9e5edbaa115cd72)
const uint8_t PUBLIC_CHANNEL_KEY[16] = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a, 0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};

const char *radioStateName(RadioState s) {
  static const char *n[] = {"Off", "Looking for radio", "Connecting", "Pairing", "Syncing", "Connected", "Error"};
  return s <= R_ERROR ? n[s] : "?";
}

std::string toHex(const uint8_t *d, size_t n) {
  static const char *h = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    s += h[d[i] >> 4];
    s += h[d[i] & 15];
  }
  return s;
}
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void put32(std::vector<uint8_t> &v, uint32_t x) {
  for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i)));
}
static void putStr(std::vector<uint8_t> &v, const std::string &s, size_t fixed) {
  for (size_t i = 0; i < fixed; i++) v.push_back(i < s.size() ? (uint8_t)s[i] : 0);
}
static std::string cstr(const uint8_t *p, size_t max) {
  size_t n = 0;
  while (n < max && p[n]) n++;
  return std::string((const char *)p, n);
}
void Companion::putKey(std::vector<uint8_t> &v, const std::string &hex) {
  for (size_t i = 0; i + 1 < hex.size() && i < 64; i += 2) v.push_back((uint8_t)strtol(hex.substr(i, 2).c_str(), nullptr, 16));
}

std::string RChannel::secretHex() const { return toHex(secret, 16); }
std::string RChannel::shareLink() const {
  std::string n;
  char b[4];
  for (unsigned char ch : name) {
    if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') n += (char)ch;
    else {
      snprintf(b, sizeof b, "%%%02X", ch);
      n += b;
    }
  }
  return "meshcore://channel/add?name=" + n + "&secret=" + secretHex();
}
ChanKind Companion::classify(const std::string &name, const uint8_t secret[16]) {
  if (!memcmp(secret, PUBLIC_CHANNEL_KEY, 16)) return CK_PUBLIC;
  if (!name.empty() && name[0] == '#') {
    uint8_t k[16];
    hashtagKey(name, k);
    if (!memcmp(k, secret, 16)) return CK_HASHTAG;
  }
  return CK_PRIVATE;
}

void Companion::setState(RadioState s, const std::string &msg) {
  state = s;
  status = msg;
  if (onChange) onChange();
}

// ---------------------------------------------------------------- command queue (one in flight at a time)
void Companion::queue(Cmd c) {
  outbox_.push_back(std::move(c));
  pump();
}
void Companion::simple(std::vector<uint8_t> bytes, const std::string &ok, const std::string &err, std::function<void()> onOk) {
  if (state != R_READY && state != R_SYNCING) {
    toast(false, "Radio not connected");
    return;
  }
  Cmd c;
  c.bytes = std::move(bytes);
  c.kind = K_SIMPLE;
  c.okMsg = ok;
  c.errMsg = err;
  c.onOk = std::move(onOk);
  queue(std::move(c));
}
void Companion::pump() {
  if (inFlight_ || outbox_.empty() || !write) return;
  if (state != R_SYNCING && state != R_READY) return;
  Cmd &c = outbox_.front();
  c.sentAt = now();
  inFlight_ = true;
  write(c.bytes);
}
void Companion::done() {
  if (!outbox_.empty()) outbox_.pop_front();
  inFlight_ = false;
  pump();
}

void Companion::connected() {
  outbox_.clear();
  inFlight_ = false;
  contacts.clear();
  channels.clear();
  selfInfoValid = false;
  setState(R_SYNCING, "Connected, syncing...");
  queue({{CMD_DEVICE_QUERY, 3}, K_QUERY});
  std::vector<uint8_t> app = {CMD_APP_START, 1, 0, 0, 0, 0, 0, 0};
  for (char ch : std::string("MESHtop")) app.push_back((uint8_t)ch);
  queue({app, K_APPSTART});
  if (clockValid && clockValid()) {
    std::vector<uint8_t> t = {CMD_SET_DEVICE_TIME};
    put32(t, (uint32_t)(now() / 1000));
    queue({t, K_TIME});
  }
  queue({{CMD_GET_CONTACTS}, K_CONTACTS});
}
void Companion::disconnected() {
  outbox_.clear();
  inFlight_ = false;
  heardBuf_.clear();
  for (auto &c : convs)
    for (auto &m : c.msgs)
      if (m.mine && m.status == MS_SENDING) m.status = MS_FAILED;
  if (state != R_OFF) setState(R_CONNECTING, "Radio disconnected, reconnecting...");
}

void Companion::tick() {
  int64_t t = now();
  if (discovery.running && t >= discovery.until) {
    discovery.running = false;
    discovery.done = true;
    if (onChange) onChange();
  }
  for (auto &kv : remote)
    if (kv.second.waitingNeigh && t > kv.second.neighDeadline) {
      kv.second.waitingNeigh = false;
      kv.second.neighAt = t;  // asked, no answer
      if (onChange) onChange();
    }
  if (pathDisc.waiting && t > pathDisc.deadline) {
    pathDisc.waiting = false;
    pathDisc.done = pathDisc.failed = true;
    pathDisc.note = "No answer within a minute. They may be out of range or offline.";
    if (onChange) onChange();
  }
  if (inFlight_ && !outbox_.empty() && t - outbox_.front().sentAt > 8000) {
    Cmd &c = outbox_.front();
    if (c.kind == K_SEND_CH || c.kind == K_SEND_DM)
      if (Conv *cv = conv(c.convId))
        if (c.msgIndex < cv->msgs.size()) cv->msgs[c.msgIndex].status = MS_FAILED;
    if (!c.errMsg.empty()) toast(false, c.errMsg + " (radio didn't answer)");
    done();
  }
  for (auto &c : convs)
    for (auto &m : c.msgs)
      if (m.mine && m.status == MS_SENT && !c.isChannel && m.ackDeadline && t > m.ackDeadline) {
        m.status = MS_FAILED;  // no delivery receipt
        if (onChange) onChange();
      }
  if (trace.waiting && t > trace.deadline) {  // the trace never made it back
    say("[trace] no reply " + std::string(trace.viaSaved ? "along the saved route" : "straight to it") + "; radio heard " +
        std::to_string(trace.heardHops) + " of " + std::to_string(trace.route.size()) + " hops pass it on");
    if (!trace.alt.route.empty()) {  // try the other way once: saved routes go stale, and "straight" needs it in range
      TraceTry was;
      was.route = trace.route;
      was.turnAt = trace.turnAt;
      was.viaSaved = trace.viaSaved;
      was.heardHops = trace.heardHops;
      was.heardSnr = trace.heardSnr;
      trace.first = was;
      TraceTry next = trace.alt;
      trace.alt = TraceTry();
      trace.secondTry = true;
      trace.retriedDirect = !next.viaSaved;
      sendTrace(next);
    } else {
      trace.waiting = false;
      trace.done = trace.failed = true;
      trace.note = trace.secondTry ? "No reply along the saved route, or straight to it. It may be out of range, offline, or set not to repeat."
                                   : "No reply. It may be out of range, offline, or set not to repeat.";
      if (onChange) onChange();
    }
  }
  for (auto &r : remote) {  // login / status requests that never came back
    if ((r.second.waitingLogin || r.second.waitingStatus) && t - r.second.at > 30000) {
      bool wasLogin = r.second.waitingLogin;
      r.second.waitingLogin = r.second.waitingStatus = false;
      toast(false, wasLogin ? "No answer to the login. The repeater may be out of range." : "No status reply. Try again in a minute.");
      if (onChange) onChange();
    }
  }
  if (state == R_READY) {
    if (t - lastBatt_ > 120000) {
      lastBatt_ = t;
      queue({{CMD_GET_BATT_AND_STORAGE}, K_BATT});
    }
    if (t - lastSync_ > 60000) syncNow();  // belt and braces: MSG_WAITING pushes should cover this
  }
}

void Companion::syncNow() {
  lastSync_ = now();
  if (leaveForPhone) {  // only fetch while a remote console answer is due (it arrives as a message)
    bool cliWaiting = false;
    for (auto &r : remote)
      if (r.second.waitingCli) cliWaiting = true;
    if (!cliWaiting) return;
  }
  for (auto &c : outbox_)
    if (c.kind == K_SYNC) return;
  queue({{CMD_SYNC_NEXT_MESSAGE}, K_SYNC});
}
void Companion::refreshContacts() {
  for (auto &c : outbox_)
    if (c.kind == K_CONTACTS) return;
  queue({{CMD_GET_CONTACTS}, K_CONTACTS});
}

// ---------------------------------------------------------------- conversations
Conv *Companion::conv(const std::string &id) {
  for (auto &c : convs)
    if (c.id == id) return &c;
  return nullptr;
}
Conv &Companion::channelConv(int idx, const std::string &name) {
  std::string id = "c:" + std::to_string(idx);
  if (Conv *c = conv(id)) {
    if (!name.empty()) c->title = name;
    return *c;
  }
  Conv c;
  c.id = id;
  c.isChannel = true;
  c.chIdx = idx;
  c.title = name.empty() ? (idx == 0 ? "Public" : "Channel " + std::to_string(idx)) : name;
  convs.push_back(c);
  sortConvs();
  return *conv(id);
}
Conv &Companion::dmConv(const std::string &prefix) {
  std::string id = "d:" + prefix.substr(0, 12);
  if (Conv *c = conv(id)) return *c;
  Conv c;
  c.id = id;
  c.isChannel = false;
  RContact *rc = contactByPrefix(prefix);
  c.key = rc ? rc->key.str() : prefix;
  c.title = rc ? rc->name.str() : prefix.substr(0, 8);
  convs.push_back(c);
  sortConvs();
  return *conv(id);
}
Conv &Companion::openDm(const RContact &c) {
  Conv &cv = dmConv(c.key.substr(0, 12));
  cv.key = c.key;
  cv.title = c.name;
  return cv;
}
void Companion::sortConvs() {
  std::stable_sort(convs.begin(), convs.end(), [](const Conv &a, const Conv &b) {
    if (a.isChannel != b.isChannel) return a.isChannel;
    if (a.isChannel) return a.chIdx < b.chIdx;
    return a.lastTs > b.lastTs;
  });
}
RContact *Companion::contactByPrefix(const std::string &p) {
  if (p.empty()) return nullptr;
  for (auto &c : contacts)
    if (c.key.compare(0, p.size(), p) == 0) return &c;
  return nullptr;
}
int Companion::unreadTotal() const {
  int n = 0;
  for (auto &c : convs) n += c.unread;
  return n;
}
void Companion::markRead(const std::string &id) {
  if (Conv *c = conv(id)) c->unread = 0;
}
void Companion::addIncoming(Conv &c, RMsg m) {
  c.lastTs = std::max(c.lastTs, m.ts);
  c.msgs.push_back(m);
  if (onStore) onStore(c, c.msgs.back());
  if (c.msgs.size() > 100) c.msgs.erase(c.msgs.begin());
  c.unread++;
  std::string id = c.id;
  sortConvs();
  if (Conv *cv = conv(id))
    if (onMessage) onMessage(*cv, cv->msgs.back());
}

void Companion::restoreMsg(bool isChannel, const std::string &chanName, const std::string &dmKey, const RMsg &m) {
  Conv *c = nullptr;
  if (isChannel) {
    for (auto &cv : convs)
      if (cv.isChannel && strcasecmp(cv.title.c_str(), chanName.c_str()) == 0) c = &cv;
    if (!c) return;  // that channel isn't on this radio any more
  } else {
    if (dmKey.size() < 12) return;
    c = &dmConv(dmKey.substr(0, 12));
  }
  for (auto &x : c->msgs)
    if (x.ts == m.ts && x.text == m.text && x.mine == m.mine) return;  // already there
  auto at = std::upper_bound(c->msgs.begin(), c->msgs.end(), m.ts, [](int64_t ts, const RMsg &x) { return ts < x.ts; });
  c->msgs.insert(at, m);
  if (c->msgs.size() > 100) c->msgs.erase(c->msgs.begin());
  c->lastTs = std::max(c->lastTs, m.ts);
}

// ---------------------------------------------------------------- sending
bool Companion::sendText(const std::string &convId, const std::string &text0) {
  if (state != R_READY && state != R_SYNCING) return false;
  Conv *c = conv(convId);
  if (!c || text0.empty()) return false;
  std::string text = text0.substr(0, 150);
  uint32_t ts = (uint32_t)(now() / 1000);
  RMsg m;
  m.who = selfName.empty() ? "me" : selfName;
  m.text = text;
  m.ts = now();
  m.mine = true;
  m.status = MS_SENDING;
  m.sentTs = ts;
  c->msgs.push_back(m);
  c->lastTs = m.ts;
  if (onStore) onStore(*c, c->msgs.back());
  Cmd cmd;
  cmd.convId = convId;
  cmd.msgIndex = c->msgs.size() - 1;
  if (c->isChannel) {
    cmd.kind = K_SEND_CH;
    cmd.bytes = {CMD_SEND_CHANNEL_TXT_MSG, 0, (uint8_t)c->chIdx};
    put32(cmd.bytes, ts);
  } else {
    cmd.kind = K_SEND_DM;
    cmd.bytes = {CMD_SEND_TXT_MSG, 0, 0};
    put32(cmd.bytes, ts);
    std::vector<uint8_t> k;
    putKey(k, c->key.substr(0, 12));
    cmd.bytes.insert(cmd.bytes.end(), k.begin(), k.end());
  }
  for (char ch : text) cmd.bytes.push_back((uint8_t)ch);
  queue(cmd);
  if (onChange) onChange();
  return true;
}
void Companion::sendAdvert(bool flood) {
  simple({CMD_SEND_SELF_ADVERT, (uint8_t)(flood ? 1 : 0)},
         flood ? "Hello sent across the mesh (through repeaters)" : "Hello sent to radios in direct range", "Couldn't send the hello");
}

// ---------------------------------------------------------------- channels
RChannel *Companion::channelByIdx(int idx) {
  for (auto &c : channels)
    if (c.idx == idx) return &c;
  return nullptr;
}
int Companion::freeChannelSlot() const {
  for (int i = 0; i < maxChannels; i++) {
    bool used = false;
    for (auto &c : channels) used |= c.idx == i;
    if (!used) return i;
  }
  return -1;
}
void Companion::setChannelSlot(int idx, const std::string &name, const uint8_t secret[16], const std::string &okMsg) {
  std::vector<uint8_t> f = {CMD_SET_CHANNEL, (uint8_t)idx};
  putStr(f, name, 32);
  f.insert(f.end(), secret, secret + 16);
  std::string nm = name;
  std::vector<uint8_t> sec(secret, secret + 16);
  simple(f, okMsg, "The radio didn't accept that channel", [this, idx, nm, sec]() {
    channels.erase(std::remove_if(channels.begin(), channels.end(), [idx](const RChannel &c) { return c.idx == idx; }), channels.end());
    if (!nm.empty()) {
      RChannel ch;
      ch.idx = idx;
      ch.name = nm;
      memcpy(ch.secret, sec.data(), 16);
      ch.kind = classify(nm, ch.secret);
      channels.push_back(ch);
      std::sort(channels.begin(), channels.end(), [](const RChannel &a, const RChannel &b) { return a.idx < b.idx; });
      channelConv(idx, nm);
    } else {
      std::string id = "c:" + std::to_string(idx);
      convs.erase(std::remove_if(convs.begin(), convs.end(), [&](const Conv &c) { return c.id == id; }), convs.end());
    }
    if (onChange) onChange();
  });
}
bool Companion::addHashtagChannel(const std::string &raw) {
  std::string name;
  for (char ch : raw)
    if (ch != ' ') name += (char)tolower((unsigned char)ch);
  if (name.empty() || name == "#") return false;
  if (name[0] != '#') name = "#" + name;
  if (name.size() > 31) name = name.substr(0, 31);
  for (auto &c : channels)
    if (c.name == name) {
      toast(true, name + " is already on your radio");
      return false;
    }
  int slot = freeChannelSlot();
  if (slot < 0) {
    toast(false, "All channel slots are full. Remove one first.");
    return false;
  }
  uint8_t k[16];
  hashtagKey(name, k);
  setChannelSlot(slot, name, k, "Joined " + name);
  return true;
}
bool Companion::addPublicChannel() {
  for (auto &c : channels)
    if (c.kind == CK_PUBLIC) {
      toast(true, "Public is already on your radio");
      return false;
    }
  int slot = freeChannelSlot();
  if (slot < 0) {
    toast(false, "All channel slots are full. Remove one first.");
    return false;
  }
  setChannelSlot(slot, "Public", PUBLIC_CHANNEL_KEY, "Joined Public");
  return true;
}
bool Companion::addPrivateChannel(const std::string &name, const uint8_t secret[16], bool created) {
  if (name.empty()) return false;
  int slot = freeChannelSlot();
  if (slot < 0) {
    toast(false, "All channel slots are full. Remove one first.");
    return false;
  }
  setChannelSlot(slot, name.substr(0, 31), secret, created ? "Created private channel " + name : "Joined " + name);
  return true;
}
bool Companion::createPrivateChannel(const std::string &name) {
  uint8_t k[16];
  if (randomBytes) randomBytes(k, 16);
  else
    for (auto &b : k) b = (uint8_t)rand();
  return addPrivateChannel(name, k, true);
}
void Companion::removeChannel(int idx) {
  RChannel *c = channelByIdx(idx);
  std::string nm = c ? c->name : "channel";
  static const uint8_t zero[16] = {0};
  setChannelSlot(idx, "", zero, "Removed " + nm);
}
void Companion::renameChannel(int idx, const std::string &name) {
  RChannel *c = channelByIdx(idx);
  if (!c || name.empty()) return;
  uint8_t sec[16];
  memcpy(sec, c->secret, 16);
  setChannelSlot(idx, name.substr(0, 31), sec, "Renamed to " + name);
}

// ---------------------------------------------------------------- contacts
void Companion::removeContact(const std::string &key) {
  std::vector<uint8_t> f = {CMD_REMOVE_CONTACT};
  putKey(f, key);
  RContact *c = contactByPrefix(key);
  std::string nm = c ? c->name : "contact";
  simple(f, "Removed " + nm, "Couldn't remove " + nm, [this, key]() {
    contacts.erase(std::remove_if(contacts.begin(), contacts.end(), [&](const RContact &c) { return c.key == key; }), contacts.end());
    if (onChange) onChange();
  });
}
void Companion::resetPath(const std::string &key) {
  std::vector<uint8_t> f = {CMD_RESET_PATH};
  putKey(f, key);
  simple(f, "Route forgotten. The next message will search the whole mesh.", "Couldn't reset the route", [this, key]() {
    if (RContact *c = contactByPrefix(key)) c->pathLen = -1;
    if (onChange) onChange();
  });
}
void Companion::shareContact(const std::string &key) {
  std::vector<uint8_t> f = {CMD_SHARE_CONTACT};
  putKey(f, key);
  RContact *c = contactByPrefix(key);
  simple(f, "Shared " + (c ? c->name.str() : std::string("contact")) + " with radios in direct range", "Couldn't share the contact");
}
void Companion::setFavorite(const std::string &key, bool fav) {
  RContact *c = contactByPrefix(key);
  if (!c || c->raw.size() < 1 + PUB_KEY + 3 + MAX_PATH + 32 + 4) return;
  std::vector<uint8_t> f(c->raw.begin(), c->raw.end());
  f[0] = CMD_ADD_UPDATE_CONTACT;
  f[1 + PUB_KEY + 1] = fav ? (f[1 + PUB_KEY + 1] | 1) : (f[1 + PUB_KEY + 1] & ~1);
  std::string nm = c->name;
  simple(f, fav ? nm + " is now a favorite" : nm + " removed from favorites", "Couldn't update " + nm, [this, key, fav]() {
    if (RContact *cc = contactByPrefix(key)) {
      cc->flags = fav ? (cc->flags | 1) : (cc->flags & ~1);
      if (cc->raw.size() > 1 + PUB_KEY + 1) cc->raw[1 + PUB_KEY + 1] = cc->flags;
    }
    if (onChange) onChange();
  });
}
void Companion::login(const std::string &key, const std::string &password) {
  if (state != R_READY) return;
  Cmd c;
  c.kind = K_LOGIN;
  c.key = key;
  c.bytes = {CMD_SEND_LOGIN};
  putKey(c.bytes, key);
  for (char ch : password.substr(0, 15)) c.bytes.push_back((uint8_t)ch);
  RemoteStatus &r = remote[key.substr(0, 12)];
  r.waitingLogin = true;
  r.loginFailed = false;
  r.at = now();
  c.errMsg = "Couldn't send the login";
  queue(c);
  if (onChange) onChange();
}
void Companion::requestStatus(const std::string &key) {
  if (state != R_READY) return;
  Cmd c;
  c.kind = K_STATUS;
  c.key = key;
  c.bytes = {CMD_SEND_STATUS_REQ};
  putKey(c.bytes, key);
  RemoteStatus &r = remote[key.substr(0, 12)];
  r.waitingStatus = true;
  r.at = now();
  c.errMsg = "Couldn't ask for status";
  queue(c);
  if (onChange) onChange();
}

// ---------------------------------------------------------------- repeater neighbours, remote admin
// REQ_TYPE_GET_NEIGHBOURS (simple_repeater): [6][version 0][count][offset u16][order: 2 = strongest
// first][key prefix bytes][4 random]. Reply (push 0x8C after the 4-byte tag): total u16, count u16,
// then per neighbour: key prefix, seconds since heard u32, SNR*4 i8. Up to 11 fit in one reply.
void Companion::requestNeighbours(const std::string &key) {
  if (state != R_READY) return;
  Cmd c;
  c.kind = K_NEIGH;
  c.key = key;
  c.bytes = {CMD_SEND_BINARY_REQ};
  putKey(c.bytes, key);
  uint8_t rnd[4] = {0};
  if (randomBytes) randomBytes(rnd, 4);
  for (uint8_t b : {(uint8_t)6, (uint8_t)0, (uint8_t)11, (uint8_t)0, (uint8_t)0, (uint8_t)2, (uint8_t)6, rnd[0], rnd[1], rnd[2], rnd[3]})
    c.bytes.push_back(b);
  RemoteStatus &r = remote[key.substr(0, 12)];
  r.waitingNeigh = true;
  r.neighDeadline = now() + 60000;
  c.errMsg = "Couldn't ask for its neighbours";
  queue(c);
  if (onChange) onChange();
}
// Admin commands go as a text message of type CLI (txt_type 1); the repeater answers the same way.
void Companion::sendCli(const std::string &key, const std::string &cmd) {
  if (state != R_READY || cmd.empty()) return;
  Cmd c;
  c.kind = K_CLI;
  c.key = key;
  c.bytes = {CMD_SEND_TXT_MSG, 1, 0};
  put32(c.bytes, (uint32_t)(now() / 1000));
  putKey(c.bytes, key.substr(0, 12));
  for (char ch : cmd.substr(0, 140)) c.bytes.push_back((uint8_t)ch);
  RemoteStatus &r = remote[key.substr(0, 12)];
  r.cli.push_back("> " + cmd);
  if (r.cli.size() > 60) r.cli.erase(r.cli.begin(), r.cli.begin() + (r.cli.size() - 60));
  r.waitingCli = true;
  c.errMsg = "Couldn't send the command";
  queue(c);
  if (onChange) onChange();
}
// Path discovery: a flooded request the contact answers with the route it came by and the route back.
bool Companion::discoverPath(const std::string &key) {
  if (state != R_READY || key.size() < 64) return false;
  Cmd c;
  c.kind = K_PATHDISC;
  c.key = key;
  c.bytes = {CMD_SEND_PATH_DISCOVERY_REQ, 0};
  putKey(c.bytes, key);
  pathDisc = PathDiscovery();
  pathDisc.key = key;
  pathDisc.waiting = true;
  pathDisc.started = now();
  pathDisc.deadline = now() + 60000;
  c.errMsg = "Couldn't start finding a route";
  queue(c);
  if (onChange) onChange();
  return true;
}

// ---------------------------------------------------------------- trace route
// As MeshCore's apps do it: the contact's stored route, then the target itself, then the route back
// (A, B, T, B, A). Hashes are public-key prefixes; the trace's hash width is 1, 2 or 4 bytes
// (flags & 3 = log2 width). Only repeaters answer traces.
void Companion::requestStats() {
  if (state != R_READY || statsUnsupported) return;
  for (uint8_t type = 0; type < 3; type++) {
    Cmd c;
    c.kind = K_STATS;
    c.bytes = {CMD_GET_STATS, type};
    queue(c);
  }
}
bool Companion::discover() {
  discovery = DiscoverRun();
  if (state != R_READY) {
    discovery.done = discovery.failed = true;
    discovery.note = "The radio isn't connected.";
    if (onChange) onChange();
    return false;
  }
  if (fwCode && fwCode < 8) {
    discovery.done = discovery.failed = true;
    discovery.note = "Your radio's firmware is too old for this. Update the companion firmware to use it.";
    if (onChange) onChange();
    return false;
  }
  uint8_t tag[4] = {0};
  if (randomBytes) randomBytes(tag, 4);
  discovery.tag = (uint32_t)tag[0] | (uint32_t)tag[1] << 8 | (uint32_t)tag[2] << 16 | (uint32_t)tag[3] << 24;
  if (!discovery.tag) discovery.tag = (uint32_t)(now() & 0x7FFFFFFF) | 1;
  discovery.started = now();
  discovery.until = discovery.started + 20000;  // repeaters answer after a random delay
  discovery.running = true;
  Cmd c;
  c.kind = K_DISCOVER;
  // [0x80 | 1 = discover request, key prefix only][type filter: everyone][tag 4]
  c.bytes = {CMD_SEND_CONTROL_DATA, 0x81, 0xFF};
  for (int i = 0; i < 4; i++) c.bytes.push_back((uint8_t)(discovery.tag >> (8 * i)));
  queue(c);
  if (onChange) onChange();
  return true;
}

std::vector<std::string> Companion::savedRoute(const std::string &key) {
  std::vector<std::string> out;
  RContact *c = contactByPrefix(key.substr(0, 12));
  if (!c || c->raw.size() < 1 + PUB_KEY + 3 + MAX_PATH) return out;
  uint8_t pl = c->raw[1 + PUB_KEY + 2];
  if (pl == 0xFF) return out;
  int hops = pl & 63, hsz = (pl >> 6) + 1;
  const uint8_t *path = &c->raw[1 + PUB_KEY + 3];
  for (int i = 0; i < hops && (i + 1) * hsz <= MAX_PATH; i++) out.push_back(toHex(path + i * hsz, hsz));
  return out;
}

// Trace route. Straight to the repeater and back when it's in direct range (its saved route is
// direct or unknown, or it answered "Who can hear me?" in the last 10 minutes); otherwise along the
// saved route and mirrored back (meshcore-open's convention). If the saved route never answers, it is
// tried once more straight to the repeater: saved routes go stale, and a repeater right next to you
// is often saved by way of whichever repeater passed on its advert first.
bool Companion::traceRoute(const std::string &key) {
  trace = TraceRun();
  trace.target = key;
  RContact *c = contactByPrefix(key.substr(0, 12));
  if (state != R_READY || !c || c->raw.size() < 1 + PUB_KEY + 3 + MAX_PATH) {
    trace.failed = trace.done = true;
    trace.note = "The radio isn't ready, or this contact isn't on it.";
    if (onChange) onChange();
    return false;
  }
  uint8_t pl = c->raw[1 + PUB_KEY + 2];
  std::vector<std::string> out;
  int width = 1;
  if (pl != 0xFF) {
    int hops = pl & 63, hsz = (pl >> 6) + 1;
    width = hsz >= 2 ? 2 : 1;  // a 3-byte hash's first 2 bytes are the same key prefix
    const uint8_t *path = &c->raw[1 + PUB_KEY + 3];
    for (int i = 0; i < hops && (i + 1) * hsz <= MAX_PATH; i++) out.push_back(toHex(path + i * hsz, width));
  }
  std::string full = c->key.c_str();
  std::string tgt = full.substr(0, width * 2);
  if (!out.empty() && out.back() == tgt) out.pop_back();
  TraceTry direct;  // straight there and back: one hop, 1-byte hash
  direct.route = {full.substr(0, 2)};
  TraceTry saved;   // along the saved route and mirrored back
  if (!out.empty()) {
    saved.route = out;
    saved.route.push_back(tgt);
    for (int i = (int)out.size() - 1; i >= 0; i--) saved.route.push_back(out[i]);
    saved.turnAt = (int)out.size();
    saved.width = width;
    saved.viaSaved = true;
  }
  if (saved.route.empty() || heardDirectly(full)) {
    trace.alt = saved;  // empty when there is no saved route
    sendTrace(direct);
  } else {
    trace.alt = direct;
    sendTrace(saved);
  }
  return true;
}

bool Companion::heardDirectly(const std::string &full) {
  if (full.size() < 12) return false;
  int64_t t = now();
  for (auto &nb : discovery.found)
    if (nb.key.size() >= 8 && full.compare(0, 8, nb.key, 0, 8) == 0 && t - nb.at < 600000) return true;
  auto it = firstHand_.find(full.substr(0, 12));
  if (it != firstHand_.end() && t - it->second < 3 * 3600000LL) return true;
  int64_t last = lastHopAt_[(uint8_t)strtoul(full.substr(0, 2).c_str(), nullptr, 16)];
  return last && t - last < 30 * 60000LL;
}
int64_t Companion::lastFirstHand(const std::string &full) {
  if (full.size() < 12) return 0;
  int64_t best = lastHopAt_[(uint8_t)strtoul(full.substr(0, 2).c_str(), nullptr, 16)];
  auto it = firstHand_.find(full.substr(0, 12));
  if (it != firstHand_.end()) best = std::max(best, it->second);
  for (auto &nb : discovery.found)
    if (nb.key.size() >= 8 && full.compare(0, 8, nb.key, 0, 8) == 0) best = std::max(best, nb.at);
  return best;
}

// The radio's raw receive log shows the trace each time a repeater within earshot passes it on: that
// is the progress the trace screen shows. When it has reached the end of its path it is the answer
// itself (the same SNRs the radio's own trace report carries), so it also finishes the trace in case
// that report never arrives.
void Companion::traceHeard(const uint8_t *path, int hops, int hsz, const uint8_t *pay, size_t payLen, float snr) {
  if (!trace.waiting || hsz != 1 || payLen < 9) return;
  uint32_t tag = (uint32_t)pay[0] | (uint32_t)pay[1] << 8 | (uint32_t)pay[2] << 16 | (uint32_t)pay[3] << 24;
  if (tag != trace.tag) return;
  int sz = pay[8] & 3;
  int total = (int)((payLen - 9) >> sz);  // hop codes in the trace
  if (hops < 1 || hops > total) return;   // each repeater that passes it on adds one SNR
  if (hops > trace.heardHops) {
    trace.heardHops = hops;
    trace.heardSnr = snr;
  }
  if (hops < total) {
    say("[trace] heard hop " + std::to_string(hops) + " of " + std::to_string(total) + " pass it on");
    if (onChange) onChange();
    return;
  }
  trace.snrs.clear();
  for (int i = 0; i < hops; i++) trace.snrs.push_back((int8_t)path[i] / 4.0f);
  trace.finalSnr = snr;
  trace.fromRxLog = true;
  traceFinished();
}

void Companion::traceFinished() {
  say("[trace] came back after " + std::to_string((now() - trace.started) / 1000.0).substr(0, 4) + " s" +
      (trace.fromRxLog ? " (heard on air)" : ""));
  trace.waiting = false;
  trace.done = true;
  if (onChange) onChange();
}

void Companion::sendTrace(const TraceTry &way) {
  const std::vector<std::string> &route = way.route;
  int width = way.width;
  trace.route = route;
  trace.turnAt = way.turnAt;
  trace.viaSaved = way.viaSaved;
  trace.heardHops = 0;
  trace.heardSnr = 0;
  trace.fromRxLog = false;
  trace.snrs.clear();
  uint8_t tb[4] = {0};
  if (randomBytes) randomBytes(tb, 4);
  trace.tag = ((uint32_t)tb[0] | (uint32_t)tb[1] << 8 | (uint32_t)tb[2] << 16 | (uint32_t)tb[3] << 24) ^ (uint32_t)(now() & 0x7FFFFFFF);
  trace.started = now();
  trace.deadline = trace.started + 20000;
  trace.waiting = true;
  trace.done = trace.failed = false;
  Cmd cmd;
  cmd.kind = K_TRACE;
  cmd.bytes = {CMD_SEND_TRACE_PATH};
  for (int i = 0; i < 4; i++) cmd.bytes.push_back((uint8_t)(trace.tag >> (8 * i)));
  for (int i = 0; i < 4; i++) cmd.bytes.push_back(0);  // auth code (unused for public traces)
  cmd.bytes.push_back(width == 2 ? 1 : 0);              // flags: log2 hash width
  std::string shown;
  for (auto &h : route) {
    for (size_t i = 0; i + 1 < h.size(); i += 2) cmd.bytes.push_back((uint8_t)strtol(h.substr(i, 2).c_str(), nullptr, 16));
    shown += h + " ";
  }
  cmd.errMsg = "Couldn't start the trace";
  say("[trace] to " + trace.target.substr(0, 8) + (trace.viaSaved ? " along the saved route: " : " straight there and back: ") + shown);
  queue(cmd);
  if (onChange) onChange();
}

// ---------------------------------------------------------------- my radio
void Companion::setName(const std::string &name0) {
  std::string name = name0.substr(0, 31);
  if (name.empty()) return;
  std::vector<uint8_t> f = {CMD_SET_ADVERT_NAME};
  for (char ch : name) f.push_back((uint8_t)ch);
  simple(f, "Name saved: " + name + ". Say hello so others see it.", "Couldn't save the name", [this, name]() {
    selfName = name;
    if (onChange) onChange();
  });
}
void Companion::setRadio(double f, double bw, int s, int c) {
  std::vector<uint8_t> v = {CMD_SET_RADIO_PARAMS};
  put32(v, (uint32_t)(f * 1000 + 0.5));
  put32(v, (uint32_t)(bw * 1000 + 0.5));
  v.push_back((uint8_t)s);
  v.push_back((uint8_t)c);
  simple(v, "Radio settings saved", "The radio rejected those settings", [this, f, bw, s, c]() {
    freqMHz = f;
    bwKHz = bw;
    sf = s;
    cr = c;
    if (onChange) onChange();
  });
}
void Companion::setTxPower(int dbm) {
  dbm = std::max(-9, std::min(dbm, maxTxPower));
  simple({CMD_SET_RADIO_TX_POWER, (uint8_t)(int8_t)dbm}, "Transmit power set to " + std::to_string(dbm) + " dBm", "Couldn't set transmit power",
         [this, dbm]() {
           txPower = dbm;
           if (onChange) onChange();
         });
}
void Companion::setLocation(bool share, double lat, double lon) {
  if (share) {
    std::vector<uint8_t> v = {CMD_SET_ADVERT_LATLON};
    put32(v, (uint32_t)(int32_t)(lat * 1e6));
    put32(v, (uint32_t)(int32_t)(lon * 1e6));
    simple(v, "", "Couldn't save the location", [this, lat, lon]() {
      hasSelfLoc = true;
      selfLat = lat;
      selfLon = lon;
    });
  }
  // SET_OTHER_PARAMS rewrites all four values, so send the current ones back with the new policy
  std::vector<uint8_t> o = {CMD_SET_OTHER_PARAMS, manualAdd, telemetry, (uint8_t)(share ? 1 : 0), multiAcks};
  simple(o, share ? "Your location will be included when you say hello" : "Location sharing turned off", "Couldn't change location sharing",
         [this, share]() {
           locPolicy = share ? 1 : 0;
           if (onChange) onChange();
         });
}
void Companion::setAutoAdd(bool autoAdd) {
  uint8_t m = autoAdd ? 0 : 1;
  std::vector<uint8_t> o = {CMD_SET_OTHER_PARAMS, m, telemetry, locPolicy, multiAcks};
  simple(o, autoAdd ? "New radios you hear will be added to People automatically" : "New radios won't be added automatically",
         "Couldn't change that setting", [this, m]() {
           manualAdd = m;
           if (onChange) onChange();
         });
}
void Companion::setPin(uint32_t pin) {
  std::vector<uint8_t> v = {CMD_SET_DEVICE_PIN};
  put32(v, pin);
  simple(v, "Radio Bluetooth PIN changed. Use the new PIN next time.", "PIN must be 6 digits");
}
void Companion::reboot() {
  if (state != R_READY) return;
  std::vector<uint8_t> v = {CMD_REBOOT, 'r', 'e', 'b', 'o', 'o', 't'};
  write(v);  // no reply: the radio restarts
  toast(true, "Radio restarting. It reconnects by itself.");
}

// ---------------------------------------------------------------- receiving
void Companion::parseContact(const uint8_t *d, size_t n, RContact &c) {
  // [code][pubkey 32][type][flags][out_path_len][out_path 64][name 32][last_advert 4][lat 4][lon 4][lastmod 4]
  if (n < 1 + PUB_KEY + 3 + MAX_PATH + 32 + 12) return;
  c.raw.assign(d, d + n);
  const uint8_t *p = d + 1;
  c.key = toHex(p, PUB_KEY);
  p += PUB_KEY;
  c.type = *p++;
  c.flags = *p++;
  uint8_t pl = *p++;  // 0xFF = no route; else low 6 bits = hops, top 2 bits = hash size - 1
  c.pathLen = pl == 0xFF ? -1 : (pl & 63);
  p += MAX_PATH;
  c.name = cstr(p, 32);
  p += 32;
  c.lastAdvert = (int64_t)u32(p) * 1000;
  p += 4;
  int32_t lat = (int32_t)u32(p), lon = (int32_t)u32(p + 4);
  c.hasLoc = lat != 0 || lon != 0;
  c.lat = lat / 1e6;
  c.lon = lon / 1e6;
}

// ---------------------------------------------------------------- "heard by" and routes
// The radio forwards every packet it hears as PUSH_LOG_RX_DATA: [0x88][snr*4][rssi][raw packet].
// Raw packet (MeshCore Packet::readFrom): header (route type bits 0-1, payload type bits 2-5),
// 4 bytes of transport codes for transport routes, path_len (low 6 bits = hops, top 2 bits = hash
// size - 1), the path, then the payload.
//  - Group text payload: [channel hash][MAC 2][AES-128-ECB data]; channel hash = SHA-256(key)[0],
//    MAC = HMAC-SHA256(key padded to 32)[0..1], and the data decrypts to [timestamp 4][flags]["Name: text"].
//  - Direct text payload: [destination hash][source hash][MAC 2][data], encrypted with a secret only the
//    two radios know, so copies are matched to messages by the two hashes and the packet itself.
// A flood packet collects the hash of every repeater that sends it on, in order, so each copy your
// radio hears shows the whole route that copy took. A copy of a message *we* sent was just re-sent
// by the repeater at the end of its path ("heard by"). A packet sent along a set route (direct) has
// each repeater remove itself, so it arrives with an empty path. Same approach as meshcore-open.
static uint32_t packetId(uint8_t ptype, const uint8_t *pay, size_t len) {
  std::vector<uint8_t> b(1 + len);
  b[0] = ptype;
  memcpy(b.data() + 1, pay, len);
  uint8_t h[32];
  sha256(b.data(), b.size(), h);
  return (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
}
// For radio-only mode: what a heard packet is, as far as your radio can tell (MeshCore's Packet,
// AdvertDataHelpers and the channel scheme above). Adverts: [key 32][timestamp 4][signature 64][app data],
// app data = [flags: type in the low 4 bits, 0x10 lat/lon (int32 x 1e6 each), 0x20/0x40 two extra
// bytes each, 0x80 name].
void Companion::describeHeard(uint8_t ptype, bool flood, const uint8_t *path, int hops, int hsz, const uint8_t *pay, size_t payLen,
                              float snr, int rssi) {
  HeardPkt h;
  h.type = ptype;
  h.flood = flood;
  h.snr = snr;
  h.rssi = rssi;
  if (flood)
    for (int k = 0; k < hops; k++) h.hops.push_back(toHex(path + k * hsz, hsz));
  h.id = packetId(ptype, pay, payLen);
  if (ptype == 0x04 && payLen > 100) {
    h.key = toHex(pay, 32);
    const uint8_t *ad = pay + 100;
    size_t adLen = payLen - 100, i = 1;
    uint8_t fl = ad[0];
    h.role = fl & 0x0F;
    if ((fl & 0x10) && i + 8 <= adLen) {
      int32_t la, lo;
      memcpy(&la, ad + i, 4);
      memcpy(&lo, ad + i + 4, 4);
      i += 8;
      h.hasLoc = la != 0 || lo != 0;
      h.lat = la / 1e6;
      h.lon = lo / 1e6;
    }
    if (fl & 0x20) i += 2;
    if (fl & 0x40) i += 2;
    if ((fl & 0x80) && i < adLen) h.name.assign((const char *)ad + i, strnlen((const char *)ad + i, adLen - i));
  } else if (ptype == 0x05 && payLen >= 3 + 16 && (payLen - 3) % 16 == 0) {
    for (auto &ch : channels) {
      uint8_t hh[32];
      sha256(ch.secret, 16, hh);
      if (hh[0] != pay[0]) continue;
      uint8_t key32[32] = {0};
      memcpy(key32, ch.secret, 16);
      hmacSha256(key32, 32, pay + 3, payLen - 3, hh);
      if (hh[0] != pay[1] || hh[1] != pay[2]) continue;
      std::vector<uint8_t> plain(payLen - 3);
      aes128EcbDecrypt(ch.secret, pay + 3, plain.data(), plain.size());
      if (plain.size() < 6) break;
      std::string all((const char *)&plain[5], strnlen((const char *)&plain[5], plain.size() - 5));
      size_t colon = all.find(": ");
      h.channel = ch.name;
      if (colon != std::string::npos && colon < 40) {
        h.sender = all.substr(0, colon);
        h.text = all.substr(colon + 2);
      } else h.text = all;
      break;
    }
  } else if (ptype == 0x02 && payLen >= 2) {
    h.srcHash = pay[1];
  }
  onHeard(h);
}

bool Companion::addPath(RMsg &m, const RxPath &p) {
  for (auto &q : m.paths)
    if (q.direct == p.direct && q.hops == p.hops) {  // the same route heard again: keep the stronger copy
      if (p.snr > q.snr) {
        q.snr = p.snr;
        q.rssi = p.rssi;
      }
      return false;
    }
  if (m.paths.size() >= 16) return false;
  m.paths.push_back(p);
  return true;
}
void Companion::claimChannelCopies(int idx, RMsg &m, const std::string &full) {
  for (auto it = heardBuf_.begin(); it != heardBuf_.end();) {
    if (!it->dm && it->chIdx == idx && it->ts == m.sentTs && it->text == full) {
      addPath(m, it->path);
      it = heardBuf_.erase(it);
    } else ++it;
  }
}
void Companion::claimDmCopies(uint8_t src, RMsg &m) {
  // The radio hands over messages in the order it received them, so the oldest unclaimed packet from
  // this sender is this message's; every later copy of that packet belongs to it as well.
  uint32_t pkt = 0;
  for (auto &h : heardBuf_)
    if (h.dm && h.src == src) {
      pkt = h.pkt;
      break;
    }
  if (!pkt) return;
  m.pkt = pkt;
  for (auto it = heardBuf_.begin(); it != heardBuf_.end();) {
    if (it->dm && it->pkt == pkt) {
      addPath(m, it->path);
      it = heardBuf_.erase(it);
    } else ++it;
  }
}

void Companion::rxLog(const uint8_t *d, size_t n) {
  if (n < 3 + 2) return;
  float snr = (int8_t)d[1] / 4.0f;
  int rssi = (int8_t)d[2];
  const uint8_t *p = d + 3;
  size_t len = n - 3, i = 0;
  uint8_t header = p[i++];
  uint8_t route = header & 0x03, ptype = (header >> 2) & 0x0F;
  if (route == 0x00 || route == 0x03) i += 4;  // transport codes
  if (i >= len) return;
  uint8_t pl = p[i++];
  int hops = pl & 63, hsz = (pl >> 6) + 1;
  if (hsz > 3 || i + (size_t)(hops * hsz) >= len) return;
  const uint8_t *path = p + i;
  i += hops * hsz;
  const uint8_t *pay = p + i;
  size_t payLen = len - i;
  bool flood = route == 0x00 || route == 0x01;
  int64_t t = now();
  if (onHeard) describeHeard(ptype, flood, path, hops, hsz, pay, payLen, snr, rssi);
  // who your radio hears first-hand: the last repeater on a flooded packet's path passed it straight
  // to us; an advert with no hops came straight from the node itself
  if (flood && hops > 0) lastHopAt_[path[(hops - 1) * hsz]] = t;
  if (ptype == 0x04 && hops == 0 && payLen >= 32) {
    firstHand_[toHex(pay, 6)] = t;
    if (firstHand_.size() > 300) firstHand_.clear();  // bounded; it refills from fresh adverts
  }
  if (ptype == 0x09 && !flood) {  // a trace going by: ours, on its way out or back?
    traceHeard(path, hops, hsz, pay, payLen, snr);
    return;
  }
  if (ptype != 0x05 && ptype != 0x02) return;  // group text, direct text
  // drop copies nobody claimed (a message the radio couldn't decrypt, or one never fetched)
  while (!heardBuf_.empty() && (heardBuf_.size() > 48 || t - heardBuf_.front().path.at > 180000)) heardBuf_.pop_front();

  RxPath rp;
  rp.direct = !flood;
  if (flood)
    for (int h = 0; h < hops; h++) rp.hops.push_back(toHex(path + h * hsz, hsz));
  rp.snr = snr;
  rp.rssi = rssi;
  rp.at = t;

  if (ptype == 0x02) {  // direct text: [dest hash][src hash][MAC 2][data]
    if (payLen < 2 + 2 + 16 || selfKey.size() < 2) return;
    uint8_t self0 = (uint8_t)strtoul(selfKey.substr(0, 2).c_str(), nullptr, 16);
    if (pay[0] != self0) return;  // not for us
    uint32_t pkt = packetId(ptype, pay, payLen);
    for (auto &c : convs) {  // a later copy of a message already shown
      if (c.isChannel || c.key.size() < 2 || (uint8_t)strtoul(c.key.substr(0, 2).c_str(), nullptr, 16) != pay[1]) continue;
      for (int k = (int)c.msgs.size() - 1; k >= 0 && k >= (int)c.msgs.size() - 30; k--)
        if (!c.msgs[k].mine && c.msgs[k].pkt == pkt) {
          if (addPath(c.msgs[k], rp) && onChange) onChange();
          return;
        }
    }
    Heard h;
    h.dm = true;
    h.src = pay[1];
    h.pkt = pkt;
    h.path = rp;
    heardBuf_.push_back(h);
    return;
  }

  if (payLen < 1 + 2 + 16 || (payLen - 3) % 16) return;
  for (auto &ch : channels) {
    uint8_t h[32];
    sha256(ch.secret, 16, h);
    if (h[0] != pay[0]) continue;
    uint8_t key32[32] = {0};
    memcpy(key32, ch.secret, 16);
    hmacSha256(key32, 32, pay + 3, payLen - 3, h);
    if (h[0] != pay[1] || h[1] != pay[2]) continue;
    std::vector<uint8_t> plain(payLen - 3);
    aes128EcbDecrypt(ch.secret, pay + 3, plain.data(), plain.size());
    if (plain.size() < 6) return;
    uint32_t ts = (uint32_t)plain[0] | (uint32_t)plain[1] << 8 | (uint32_t)plain[2] << 16 | (uint32_t)plain[3] << 24;
    std::string text((const char *)&plain[5], strnlen((const char *)&plain[5], plain.size() - 5));
    Conv *c = conv("c:" + std::to_string(ch.idx));
    if (c) {
      for (int k = (int)c->msgs.size() - 1; k >= 0 && k >= (int)c->msgs.size() - 30; k--) {
        RMsg &m = c->msgs[k];
        if (m.sentTs != ts) continue;
        if (m.mine) {
          std::string expect = (selfName.empty() ? std::string() : selfName + ": ") + m.text;
          bool same = text == expect ||
                      (text.size() >= m.text.size() && text.compare(text.size() - m.text.size(), m.text.size(), m.text) == 0);
          if (!same) continue;
          if (hops == 0) return;  // not repeated by anyone
          std::string hop = toHex(path + (hops - 1) * hsz, hsz);
          for (auto &r : m.heard)
            if (r.hop == hop) return;  // this repeater already counted
          Repeat r;
          r.hop = hop;
          r.hops = hops;
          r.snr = snr;
          r.rssi = rssi;
          r.at = t;
          m.heard.push_back(r);
          if (m.status == MS_SENDING) m.status = MS_SENT;
          if (onChange) onChange();
          return;
        }
        std::string full = m.who.empty() ? m.text : m.who + ": " + m.text;
        if (full != text) continue;
        if (addPath(m, rp) && onChange) onChange();  // another copy of a message already shown
        return;
      }
    }
    if (leaveForPhone) {  // the inbox is left for the phone: show the channel message from what was heard
      RMsg m;
      m.ts = ts ? (int64_t)ts * 1000 : t;
      m.sentTs = ts;
      m.hops = flood ? hops : 0;
      m.hasSnr = true;
      m.snr = snr;
      size_t colon = text.find(": ");
      if (colon != std::string::npos && colon < 40) {
        m.who = text.substr(0, colon);
        m.text = text.substr(colon + 2);
      } else m.text = text;
      m.paths.push_back(rp);
      addIncoming(channelConv(ch.idx, ch.name), m);
      return;
    }
    Heard hd;  // the radio hasn't handed this message over yet
    hd.chIdx = ch.idx;
    hd.ts = ts;
    hd.text = text;
    hd.path = rp;
    heardBuf_.push_back(hd);
    return;
  }
}

std::string Companion::nameForHop(const std::string &hopHex) {
  if (hopHex.empty()) return "";
  RContact *best = nullptr;
  int count = 0;
  for (auto &c : contacts)
    if (c.key.size() >= hopHex.size() && strncasecmp(c.key.c_str(), hopHex.c_str(), hopHex.size()) == 0) {
      if (!best || c.type == 2) best = &c;  // prefer repeaters
      count++;
    }
  if (!best) return "";
  return count > 1 && best->type != 2 ? "" : std::string(best->name.c_str());
}

void Companion::frame(const uint8_t *d, size_t n) {
  if (!n) return;
  uint8_t code = d[0];
  bool expecting = inFlight_ && !outbox_.empty();
  Kind k = expecting ? outbox_.front().kind : K_BATT;

  switch (code) {
    case PUSH_MSG_WAITING:
      syncNow();
      return;
    case PUSH_LOG_RX_DATA:
      rxLog(d, n);
      return;
    case PUSH_TRACE_DATA: {
      // [0x89][reserved][path_len][flags][tag 4][auth 4][path hashes][snr*4 per hop][final snr*4]
      if (n < 12) return;
      uint8_t plen = d[2], sz = d[3] & 3;
      uint32_t tag = u32(d + 4);
      int hops = plen >> sz;
      if (!trace.waiting || tag != trace.tag || n < (size_t)(12 + plen + hops + 1)) return;
      trace.snrs.clear();
      for (int i = 0; i < hops; i++) trace.snrs.push_back((int8_t)d[12 + plen + i] / 4.0f);
      trace.finalSnr = (int8_t)d[12 + plen + hops] / 4.0f;
      traceFinished();
      return;
    }
    case PUSH_SEND_CONFIRMED:
      if (n >= 5) {
        uint32_t ack = u32(d + 1);
        for (auto &c : convs)
          for (auto &m : c.msgs)
            if (m.mine && m.ack == ack && m.status != MS_DELIVERED) m.status = MS_DELIVERED;
        if (onChange) onChange();
      }
      return;
    case PUSH_NEW_ADVERT: {
      RContact c;
      parseContact(d, n, c);
      if (!c.key.empty()) {
        if (RContact *old = contactByPrefix(c.key)) *old = c;
        else contacts.push_back(c);
        if (onChange) onChange();
      }
      return;
    }
    case PUSH_ADVERT:
    case PUSH_PATH_UPDATED:
      // a known contact re-advertised / found a route: refresh quietly
      if (state == R_READY) refreshContacts();
      return;
    case PUSH_CONTACT_DELETED:
      if (n >= 1 + PUB_KEY) {
        std::string key = toHex(d + 1, PUB_KEY);
        contacts.erase(std::remove_if(contacts.begin(), contacts.end(), [&](const RContact &c) { return c.key == key; }), contacts.end());
        if (onChange) onChange();
      }
      return;
    case PUSH_CONTACTS_FULL:
      toast(false, "Your radio's contact list is full. Remove some people or repeaters.");
      return;
    case PUSH_LOGIN_SUCCESS:
    case PUSH_LOGIN_FAIL:
      if (n >= 8) {
        std::string p = toHex(d + 2, 6);
        RemoteStatus &r = remote[p];
        r.waitingLogin = false;
        r.loggedIn = code == PUSH_LOGIN_SUCCESS;
        r.loginFailed = !r.loggedIn;
        r.isAdmin = r.loggedIn && (d[1] & 1);
        RContact *c = contactByPrefix(p);
        std::string nm = c ? c->name : "repeater";
        toast(r.loggedIn, r.loggedIn ? "Logged in to " + nm + (r.isAdmin ? " as admin" : " as guest") : "Login to " + nm + " failed. Check the password.");
        if (r.loggedIn && c) requestStatus(c->key);
        if (onChange) onChange();
      }
      return;
    case PUSH_BINARY_RESPONSE: {
      // [0x8C][0][tag u32][reply...]: neighbours list for the request with this tag
      if (n < 6) break;
      uint32_t tag = u32(d + 2);
      for (auto &kv : remote) {
        RemoteStatus &r = kv.second;
        if (!r.waitingNeigh || r.neighTag != tag) continue;
        r.waitingNeigh = false;
        r.hasNeigh = true;
        r.neighAt = now();
        r.neigh.clear();
        if (n >= 10) {
          r.neighTotal = d[6] | d[7] << 8;
          int count = d[8] | d[9] << 8;
          size_t o = 10;
          for (int i = 0; i < count && o + 11 <= n; i++, o += 11) {
            RNeighbour nb;
            nb.prefix = toHex(d + o, 6);
            nb.secsAgo = u32(d + o + 6);
            nb.snr = (int8_t)d[o + 10] / 4.0f;
            r.neigh.push_back(nb);
          }
        }
        say("[neighbours] " + kv.first + ": " + std::to_string(r.neigh.size()) + " of " + std::to_string(r.neighTotal));
        if (onChange) onChange();
        break;
      }
      break;
    }
    case PUSH_PATH_DISCOVERY: {
      // [0x8D][0][key prefix 6][out path len][out path][in path len][in path]; path length bytes:
      // low 6 bits = hops, top 2 bits = hash size - 1
      if (n < 10 || !pathDisc.waiting) break;
      if (strncasecmp(toHex(d + 2, 6).c_str(), pathDisc.key.c_str(), 12) != 0) break;
      auto readPath = [&](size_t &o, std::vector<std::string> &hops) {
        if (o >= n) return false;
        uint8_t pl = d[o++];
        int cnt = pl & 63, hs = (pl >> 6) + 1;
        if (o + (size_t)cnt * hs > n) return false;
        for (int i = 0; i < cnt; i++) hops.push_back(toHex(d + o + i * hs, hs));
        o += (size_t)cnt * hs;
        return true;
      };
      size_t o = 8;
      std::vector<std::string> out, back;
      bool ok = readPath(o, out) && readPath(o, back);
      pathDisc.waiting = false;
      pathDisc.done = true;
      pathDisc.failed = !ok;
      pathDisc.out = out;   // as stored for sending to them: from your end
      pathDisc.back = back; // the way their answer came: from their end
      std::reverse(pathDisc.back.begin(), pathDisc.back.end());  // from your end too
      say("[path] " + std::to_string(out.size()) + " hops there, " + std::to_string(back.size()) + " back");
      if (onChange) onChange();
      break;
    }
    case PUSH_CONTROL_DATA: {
      // [0x8E][snr*4][rssi][path len][payload]; a discovery answer's payload is
      // [0x90 | node type][their snr*4 of us][tag 4][key: 8-byte prefix or 32]
      if (n < 4 + 6 + 8 || (d[4] & 0xF0) != 0x90 || !discovery.running || u32(d + 6) != discovery.tag) return;
      Neighbour nb;
      nb.key = toHex(d + 10, std::min<size_t>(n - 10, PUB_KEY));
      nb.type = d[4] & 0x0F;
      nb.theyHear = (int8_t)d[5] / 4.0f;
      nb.weHear = (int8_t)d[1] / 4.0f;
      nb.rssi = (int8_t)d[2];
      nb.at = now();
      bool dup = false;
      for (auto &f : discovery.found)
        if (f.key.compare(0, 16, nb.key, 0, 16) == 0) {
          f = nb;
          dup = true;
        }
      if (!dup && discovery.found.size() < 40) discovery.found.push_back(nb);
      if (onChange) onChange();
      return;
    }
    case PUSH_STATUS_RESPONSE:
      if (n >= 8) {
        RemoteStatus &r = remote[toHex(d + 2, 6)];
        r.waitingStatus = false;
        const uint8_t *s = d + 8;
        size_t m = n - 8;
        if (m >= 44) {  // through last_snr; newer repeaters append more counters
          r.hasStats = true;
          r.at = now();
          r.battMv = u16(s);
          r.txQueue = u16(s + 2);
          r.noiseFloor = (int16_t)u16(s + 4);
          r.lastRssi = (int16_t)u16(s + 6);
          r.recv = u32(s + 8);
          r.sent = u32(s + 12);
          r.airSecs = u32(s + 16);
          r.upSecs = u32(s + 20);
          r.sentFlood = u32(s + 24);
          r.sentDirect = u32(s + 28);
          r.recvFlood = u32(s + 32);
          r.recvDirect = u32(s + 36);
          r.errEvents = u16(s + 40);
          r.lastSnr = (int16_t)u16(s + 42) / 4.0f;
        }
        if (onChange) onChange();
      }
      return;
    default:
      if (code >= 0x80) return;  // other pushes (logs, telemetry...) are ignored
  }
  if (!expecting) return;

  switch (code) {
    case RESP_DEVICE_INFO:
      if (n >= 2) fwCode = d[1];
      if (n >= 4) maxChannels = std::max(1, std::min<int>(d[3], 40));
      if (n >= 80) {
        model = cstr(d + 20, 40);
        fwVersion = cstr(d + 60, 20);
      }
      done();
      break;
    case RESP_SELF_INFO:
      if (n >= 58) {
        txPower = (int8_t)d[2];
        maxTxPower = d[3];
        selfKey = toHex(d + 4, PUB_KEY);
        int32_t lat = (int32_t)u32(d + 36), lon = (int32_t)u32(d + 40);
        hasSelfLoc = lat || lon;
        selfLat = lat / 1e6;
        selfLon = lon / 1e6;
        multiAcks = d[44];
        locPolicy = d[45];
        telemetry = d[46];
        manualAdd = d[47];
        freqMHz = u32(d + 48) / 1000.0;
        bwKHz = u32(d + 52) / 1000.0;
        sf = d[56];
        cr = d[57];
        selfName = std::string((const char *)d + 58, n - 58);
        selfInfoValid = true;
      }
      done();
      break;
    case RESP_CONTACTS_START:
      pendingContacts_.clear();
      break;
    case RESP_CONTACT: {
      RContact c;
      parseContact(d, n, c);
      if (!c.key.empty()) pendingContacts_.push_back(c);
      break;
    }
    case RESP_END_OF_CONTACTS: {
      contacts.swap(pendingContacts_);
      pendingContacts_.clear();
      for (auto &cv : convs)  // DM titles from the contact list
        if (!cv.isChannel)
          if (RContact *rc = contactByPrefix(cv.key.substr(0, 12))) {
            cv.title = rc->name;
            cv.key = rc->key;
          }
      bool first = state == R_SYNCING && channels.empty();
      done();
      if (first) {
        for (int i = 0; i < maxChannels; i++) queue({{CMD_GET_CHANNEL, (uint8_t)i}, K_CHANNEL, i});
        queue({{CMD_GET_BATT_AND_STORAGE}, K_BATT});
        syncNow();
      }
      if (onChange) onChange();
      break;
    }
    case RESP_CHANNEL_INFO:
      if (n >= 2 + 32) {
        int idx = d[1];
        std::string name = cstr(d + 2, 32);
        channels.erase(std::remove_if(channels.begin(), channels.end(), [idx](const RChannel &c) { return c.idx == idx; }), channels.end());
        if (!name.empty()) {
          RChannel ch;
          ch.idx = idx;
          ch.name = name;
          if (n >= 2 + 32 + 16) memcpy(ch.secret, d + 34, 16);
          ch.kind = classify(name, ch.secret);
          channels.push_back(ch);
          std::sort(channels.begin(), channels.end(), [](const RChannel &a, const RChannel &b) { return a.idx < b.idx; });
          channelConv(idx, name);
        }
      }
      done();
      break;
    case RESP_BATT_AND_STORAGE:
      if (n >= 3) battMv = d[1] | d[2] << 8;
      lastBatt_ = now();
      done();
      if (onChange) onChange();
      break;
    case RESP_CONTACT_MSG_RECV:
    case RESP_CONTACT_MSG_RECV_V3: {
      bool v3 = code == RESP_CONTACT_MSG_RECV_V3;
      size_t o = v3 ? 4 : 1;
      if (n < o + 6 + 2 + 4) {
        done();
        break;
      }
      std::string prefix = toHex(d + o, 6);
      uint8_t pathLen = d[o + 6], txtType = d[o + 7];
      uint32_t ts = u32(d + o + 8);
      size_t t = o + 12;
      RMsg m;
      if (v3) {
        m.hasSnr = true;
        m.snr = (int8_t)d[1] / 4.0f;
      }
      m.hops = pathLen == 0xFF ? -1 : (pathLen & 63);  // low 6 bits = hop count
      m.ts = ts ? (int64_t)ts * 1000 : now();
      m.sentTs = ts;
      if (txtType == 1) {  // a repeater's answer to an admin command
        RemoteStatus &r = remote[prefix];
        std::string t = n > o + 12 ? std::string((const char *)d + o + 12, n - (o + 12)) : "";
        size_t a = 0;
        while (a <= t.size()) {  // one console line per line of the reply
          size_t b = t.find('\n', a);
          if (b == std::string::npos) b = t.size();
          std::string line = t.substr(a, b - a);
          if (!line.empty() && line.back() == '\r') line.pop_back();
          if (!line.empty()) r.cli.push_back(line);
          a = b + 1;
        }
        if (r.cli.size() > 60) r.cli.erase(r.cli.begin(), r.cli.begin() + (r.cli.size() - 60));
        r.waitingCli = false;
        done();
        syncNow();
        if (onChange) onChange();
        break;
      }
      if (txtType == 0 || txtType == 2) claimDmCopies(d[o], m);
      Conv &c = dmConv(prefix);
      m.who = c.title;
      if (txtType == 2 && n >= t + 4) {  // room server post: 4-byte author key prefix, then the text
        std::string author = toHex(d + t, 4);
        RContact *a = contactByPrefix(author);
        m.who = a ? a->name.str() : author;
        t += 4;
      }
      m.text = n > t ? std::string((const char *)d + t, n - t) : "";
      if (txtType == 0 || txtType == 2) addIncoming(c, m);  // skip CLI data
      done();
      syncNow();
      break;
    }
    case RESP_CHANNEL_MSG_RECV:
    case RESP_CHANNEL_MSG_RECV_V3: {
      bool v3 = code == RESP_CHANNEL_MSG_RECV_V3;
      size_t o = v3 ? 4 : 1;
      if (n < o + 3 + 4) {
        done();
        break;
      }
      int idx = d[o];
      uint8_t pathLen = d[o + 1];
      uint32_t ts = u32(d + o + 3);
      std::string text = n > o + 7 ? std::string((const char *)d + o + 7, n - o - 7) : "";
      RMsg m;
      if (v3) {
        m.hasSnr = true;
        m.snr = (int8_t)d[1] / 4.0f;
      }
      m.hops = pathLen == 0xFF ? -1 : (pathLen & 63);  // low 6 bits = hop count
      m.ts = ts ? (int64_t)ts * 1000 : now();
      m.sentTs = ts;
      size_t colon = text.find(": ");  // group messages arrive as "Name: text"
      if (colon != std::string::npos && colon < 40) {
        m.who = text.substr(0, colon);
        m.text = text.substr(colon + 2);
      } else {
        m.text = text;
      }
      claimChannelCopies(idx, m, text);
      bool shown = false;  // already shown from the air (phone mode)
      if (Conv *cv = conv("c:" + std::to_string(idx)))
        for (int k = (int)cv->msgs.size() - 1; k >= 0 && k >= (int)cv->msgs.size() - 40; k--)
          if (!cv->msgs[k].mine && cv->msgs[k].sentTs == ts && cv->msgs[k].text == m.text) shown = true;
      if (!shown) addIncoming(channelConv(idx, ""), m);
      done();
      syncNow();
      break;
    }
    case RESP_NO_MORE_MESSAGES:
      done();
      break;
    case RESP_SENT: {
      Cmd &c = outbox_.front();
      if (k == K_SEND_DM && n >= 10) {
        if (Conv *cv = conv(c.convId))
          if (c.msgIndex < cv->msgs.size()) {
            RMsg &m = cv->msgs[c.msgIndex];
            m.status = MS_SENT;
            m.ack = u32(d + 2);
            m.ackDeadline = now() + std::max<int64_t>(u32(d + 6), 3000) * 2 + 10000;
          }
      }
      if (k == K_TRACE && n >= 10 && trace.waiting) {
        trace.deadline = now() + std::max<int64_t>(u32(d + 6), 5000) + 8000;
        say("[trace] sent, radio expects it back within " + std::to_string(u32(d + 6)) + " ms");
      }
      if (k == K_NEIGH && n >= 10) {
        RemoteStatus &r = remote[c.key.substr(0, 12)];
        r.neighTag = u32(d + 2);
        r.neighDeadline = now() + std::min<int64_t>(u32(d + 6), 60000) + 15000;
      }
      if (k == K_PATHDISC && n >= 10 && pathDisc.waiting) {
        pathDisc.tag = u32(d + 2);
        pathDisc.deadline = now() + std::min<int64_t>(u32(d + 6), 90000) + 20000;
      }
      if ((k == K_LOGIN || k == K_STATUS) && n >= 10) {
        RemoteStatus &r = remote[c.key.substr(0, 12)];
        r.at = now() + std::min<int64_t>(u32(d + 6), 60000);  // allow the estimated round trip before giving up
      }
      done();
      if (onChange) onChange();
      break;
    }
    case RESP_STATS: {
      // [24][type] core: batt u16, uptime u32, err flags u16, queue u8 | radio: noise floor i16,
      // last rssi i8, last snr*4 i8, tx air s u32, rx air s u32 | packets: 7 x u32
      if (n >= 2) {
        auto u16 = [&](size_t o) { return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8; };
        if (d[1] == 0 && n >= 11) {
          stats.battMv = (int)u16(2);
          stats.upSecs = u32(d + 4);
          stats.errFlags = (int)u16(8);
          stats.queueLen = d[10];
          stats.core = true;
        } else if (d[1] == 1 && n >= 14) {
          stats.noiseFloor = (int16_t)u16(2);
          stats.lastRssi = (int8_t)d[4];
          stats.lastSnr = (int8_t)d[5] / 4.0f;
          stats.txAirSecs = u32(d + 6);
          stats.rxAirSecs = u32(d + 10);
          stats.radio = true;
        } else if (d[1] == 2 && n >= 30) {
          uint32_t *v[] = {&stats.recv, &stats.sent, &stats.sentFlood, &stats.sentDirect, &stats.recvFlood, &stats.recvDirect, &stats.recvErrors};
          for (int k = 0; k < 7; k++) *v[k] = u32(d + 2 + 4 * k);
          stats.packets = true;
        }
        stats.at = now();
      }
      if (k == K_STATS) done();
      if (onChange) onChange();
      break;
    }
    case RESP_OK:
    case RESP_ERR: {
      if (outbox_.empty()) break;  // nothing was waiting for it
      Cmd c = outbox_.front();
      if (c.kind == K_STATS && code == RESP_ERR) statsUnsupported = true;
      if (c.kind == K_TRACE && code == RESP_ERR) {
        trace.waiting = false;
        trace.done = trace.failed = true;
        trace.note = "Your radio refused to send the trace (error " + std::to_string(n >= 2 ? d[1] : 0) + ").";
        say("[trace] " + trace.note);
      }
      if (c.kind == K_DISCOVER && code == RESP_ERR) {
        discovery.running = false;
        discovery.done = discovery.failed = true;
        discovery.note = "Your radio couldn't send the request (busy, or its firmware is too old).";
      }
      if (c.kind == K_SEND_CH || c.kind == K_SEND_DM)
        if (Conv *cv = conv(c.convId))
          if (c.msgIndex < cv->msgs.size()) cv->msgs[c.msgIndex].status = code == RESP_OK ? MS_SENT : MS_FAILED;
      if (c.kind == K_LOGIN || c.kind == K_STATUS) {
        RemoteStatus &r = remote[c.key.substr(0, 12)];
        r.waitingLogin = r.waitingStatus = false;
      }
      if (code == RESP_ERR && c.kind == K_NEIGH) remote[c.key.substr(0, 12)].waitingNeigh = false;
      if (code == RESP_ERR && c.kind == K_CLI) {
        RemoteStatus &r = remote[c.key.substr(0, 12)];
        r.waitingCli = false;
        r.cli.push_back("(your radio couldn't send it)");
      }
      if (code == RESP_ERR && c.kind == K_PATHDISC) {
        pathDisc.waiting = false;
        pathDisc.done = pathDisc.failed = true;
        pathDisc.note = "Your radio couldn't send the request (not a contact, or its firmware is too old).";
      }
      done();
      if (code == RESP_OK) {
        if (c.onOk) c.onOk();
        toast(true, c.okMsg);
      } else {
        int e = n >= 2 ? d[1] : 0;
        toast(false, c.errMsg.empty() ? "" : c.errMsg + (e > 0 && e <= 6 ? std::string(" (") + ERR_TEXT[e] + ")" : ""));
      }
      if (onChange) onChange();
      break;
    }
    default:
      done();
      break;
  }
  if (state == R_SYNCING && outbox_.empty() && !inFlight_) setState(R_READY, "Connected");
}

}  // namespace st
