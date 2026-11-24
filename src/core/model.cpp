#include "core/model.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace st {

static const double MAX_HOP_MILES = 250;
static const int MAX_ROUTES_PER_SEC = 10;

Role parseRole(const std::string &r0) {
  std::string r = lower(r0);
  if (r == "repeater") return ROLE_REPEATER;
  if (r == "companion" || r == "chat" || r == "client") return ROLE_COMPANION;
  if (r == "room" || r == "room_server" || r == "roomserver") return ROLE_ROOM;
  if (r == "sensor") return ROLE_SENSOR;
  if (r == "observer") return ROLE_OBSERVER;
  return ROLE_UNKNOWN;
}

const char *roleName(Role r) {
  switch (r) {
    case ROLE_REPEATER: return "repeater";
    case ROLE_COMPANION: return "companion";
    case ROLE_ROOM: return "room";
    case ROLE_SENSOR: return "sensor";
    case ROLE_OBSERVER: return "observer";
    default: return "node";
  }
}

const char *groupName(Group g) {
  static const char *n[] = {"ADVERT", "CHANNEL", "DM", "TRACE", "PATH", "CONTROL"};
  return g < G_COUNT ? n[g] : "OTHER";
}

uint32_t groupColor(Group g) {
  static const uint32_t c[] = {0xFFB547, 0x39D0FF, 0x5EEAD4, 0xB78CFF, 0x3DDC97, 0x8B98A8};
  return g < G_COUNT ? c[g] : 0xC9A0FF;
}

std::string lower(std::string s) {
  for (auto &c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::vector<std::string> splitList(const std::string &s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t c = s.find(',', start);
    std::string part = trim(s.substr(start, c == std::string::npos ? std::string::npos : c - start));
    if (part.size() >= 2) out.push_back(part);
    if (c == std::string::npos) break;
    start = c + 1;
  }
  return out;
}

static bool isHex(const std::string &s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!std::isxdigit((unsigned char)c)) return false;
  return true;
}

static bool startsWith(const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; }

double milesBetween(const LatLon &a, const LatLon &b) {
  const double R = 3958.8, r = M_PI / 180;
  double dLat = (b.lat - a.lat) * r, dLon = (b.lon - a.lon) * r;
  double s = std::sin(dLat / 2) * std::sin(dLat / 2) +
             std::cos(a.lat * r) * std::cos(b.lat * r) * std::sin(dLon / 2) * std::sin(dLon / 2);
  return 2 * R * std::asin(std::min(1.0, std::sqrt(s)));
}

static bool nameChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '-'; }

// @[Name] (MeshCore app style) or @Name followed by a non-name character. Case-insensitive.
bool mentionMatch(const std::string &text0, const std::string &name0) {
  std::string text = lower(text0), name = lower(name0);
  if (name.empty()) return false;
  size_t at = 0;
  while ((at = text.find('@', at)) != std::string::npos) {
    size_t i = at + 1;
    if (i < text.size() && text[i] == '[') {
      size_t j = i + 1;
      while (j < text.size() && text[j] == ' ') j++;
      if (text.compare(j, name.size(), name) == 0) {
        size_t k = j + name.size();
        while (k < text.size() && text[k] == ' ') k++;
        if (k < text.size() && text[k] == ']') return true;
      }
    } else if (text.compare(i, name.size(), name) == 0) {
      size_t k = i + name.size();
      if (k >= text.size() || !nameChar(text[k])) return true;
    }
    at++;
  }
  return false;
}

int Model::pktPerMin(int64_t now) {
  while (!pktTimes.empty() && now - pktTimes.front() > 60000) pktTimes.erase(pktTimes.begin());
  return (int)pktTimes.size();
}

// ---------------- directory ----------------
Node *Model::findKey(const std::string &key) {
  auto it = nodeIndex.find(keyId(key.data(), key.size()));
  if (it == nodeIndex.end()) return nullptr;
  Node *n = &nodes[it->second];
  return n->key == key ? n : nullptr;
}

static bool validLL(double lat, double lon) {
  return !(lat == 0 && lon == 0) && std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && !std::isnan(lat) && !std::isnan(lon);
}

Node *Model::upsertNode(const std::string &key0, const std::string &name, const std::string &role, bool hasLoc,
                        double lat, double lon, int64_t last) {
  std::string key = lower(key0);
  if (key.empty()) return nullptr;
  Node *n = findKey(key);
  if (!n) {
    Node nn;
    nn.key = key;
    nn.name = key.substr(0, 8);
    nodes.push_back(nn);
    nodeIndex[keyId(key.data(), key.size())] = (int)nodes.size() - 1;
    n = &nodes.back();
  }
  if (!name.empty()) n->name = name.substr(0, 32);
  if (!role.empty()) n->role = parseRole(role);
  if (hasLoc && validLL(lat, lon)) {
    n->hasLoc = true;
    n->lat = lat;
    n->lon = lon;
  }
  if (last > n->last) n->last = last;
  return n;
}

void Model::setObservers(std::vector<Observer> obs) {
  observers = std::move(obs);
  for (auto &o : observers) o.id = lower(o.id);
}

Observer *Model::findObserver(const std::string &id) {
  for (auto &o : observers)
    if (o.id == id) return &o;
  return nullptr;
}

Node *Model::findByName(const std::string &q0) {
  std::string q = lower(trim(q0));
  if (q.empty()) return nullptr;
  for (auto &n : nodes)
    if (lower(n.name) == q) return &n;
  if (q.size() >= 4 && isHex(q))
    for (auto &n : nodes)
      if (startsWith(n.key, q)) return &n;
  for (auto &n : nodes)
    if (lower(n.name).find(q) != std::string::npos) return &n;
  return nullptr;
}

std::vector<Node *> Model::search(const std::string &q0, size_t max) {
  std::vector<Node *> out;
  std::string q = lower(trim(q0));
  if (q.empty()) return out;
  auto add = [&](Node &n) {
    if (out.size() >= max) return;
    for (auto *x : out)
      if (x == &n) return;
    out.push_back(&n);
  };
  for (auto &n : nodes)
    if (startsWith(lower(n.name), q)) add(n);
  for (auto &n : nodes)
    if (lower(n.name).find(q) != std::string::npos) add(n);
  if (isHex(q) && q.size() >= 2)
    for (auto &n : nodes)
      if (startsWith(n.key, q)) add(n);
  return out;
}

void Model::markDirectoryReady() {
  directoryReady = true;
  resolveMyNodes();
}

Node *Model::nodeByPrefix(const std::string &prefix0, const LatLon *near) {
  std::string p = lower(prefix0);
  if (p.empty()) return nullptr;
  Node *best = nullptr;
  double bestD = 1e18;
  int count = 0;
  LatLon ref = near ? *near : center;
  for (auto &n : nodes) {
    if (!n.hasLoc || !startsWith(n.key, p)) continue;
    count++;
    double d = milesBetween(ref, {n.lat, n.lon});
    if (d < bestD) {
      bestD = d;
      best = &n;
    }
  }
  if (!best) return nullptr;
  if (p.size() <= 2 && count > 1 && bestD > 40) return nullptr;  // 1-byte prefixes collide a lot
  if (bestD > 250) return nullptr;
  return best;
}

// ---------------- packets ----------------
Group Model::groupOfType(const std::string &t, std::string &label) {
  if (t == "ADVERT") { label = "ADVERT"; return G_ADVERT; }
  if (t == "GRP_TXT") { label = "CHANNEL"; return G_CHANNEL; }
  if (t == "TXT_MSG") { label = "DM"; return G_DM; }
  if (t == "TRACE") { label = "TRACE"; return G_TRACE; }
  if (t == "PATH") { label = "PATH"; return G_PATH; }
  if (t == "ACK") { label = "ACK"; return G_CONTROL; }
  if (t == "RESPONSE") { label = "RESP"; return G_CONTROL; }
  if (t == "REQ" || t == "ANON_REQ") { label = "REQ"; return G_CONTROL; }
  if (t == "GRP_DATA") { label = "DATA"; return G_CONTROL; }
  label = t.substr(0, 7);
  return G_CONTROL;
}

struct Pt {
  LatLon ll;
  std::string key;
};

void Model::handlePacket(const PacketIn &p) {
  int64_t now = nowMs ? nowMs() : 0;
  pktTimes.push_back(now);
  if (pktTimes.size() > 4000) pktTimes.erase(pktTimes.begin(), pktTimes.begin() + 1000);

  // Adverts teach us about nodes (and new ones can alert)
  if (p.typeName == "ADVERT" && !p.pubKey.empty()) {
    bool isNew = directoryReady && !findKey(lower(p.pubKey));
    Node *n = upsertNode(p.pubKey, p.name, "", p.hasLoc, p.lat, p.lon, now);
    if (isNew && n && cfg.alertNewNodes && n->hasLoc) {
      double d = milesBetween(center, {n->lat, n->lon});
      if (d <= cfg.radiusMi) {
        Alert a;
        a.kind = A_NEWNODE;
        a.title = "New node: " + n->name;
        char buf[64];
        snprintf(buf, sizeof buf, "%s%.1f mi from center", n->role != ROLE_UNKNOWN ? (std::string(roleName(n->role)) + " | ").c_str() : "", d);
        a.body = buf;
        a.key = n->key;
        a.hasLoc = true;
        a.lat = n->lat;
        a.lon = n->lon;
        raiseAlert(a, "new:" + n->key);
      }
    }
  }
  if (p.typeName == "GRP_TXT" && !p.text.empty()) {
    checkKeywords(p);
    checkMentions(p);
    if (!p.noChatter) addChatMsg(p);
  }

  std::string label;
  Group g = groupOfType(p.typeName, label);

  // Build the path: sender -> relays -> observer
  std::vector<Pt> pts;
  auto push = [&](LatLon ll, const std::string &key) {
    if (!pts.empty()) {
      auto &last = pts.back();
      if (last.key == key || (last.ll.lat == ll.lat && last.ll.lon == ll.lon)) return;
      if (milesBetween(last.ll, ll) > MAX_HOP_MILES) return;  // bad match or internet bridge
    }
    pts.push_back({ll, key});
  };
  std::string senderKey = lower(p.pubKey);
  if (!senderKey.empty()) {
    if (p.hasLoc && validLL(p.lat, p.lon)) push({p.lat, p.lon}, senderKey);
    else if (Node *s = findKey(senderKey); s && s->hasLoc) push({s->lat, s->lon}, senderKey);
  }
  bool useResolved = p.resolved.size() == p.hops.size();
  for (size_t i = 0; i < p.hops.size(); i++) {
    Node *n = nullptr;
    if (useResolved && !p.resolved[i].empty()) n = findKey(lower(p.resolved[i]));
    if (!n) {
      LatLon nearLL = pts.empty() ? center : pts.back().ll;
      n = nodeByPrefix(p.hops[i], &nearLL);
    }
    if (n && n->hasLoc) push({n->lat, n->lon}, n->key);
  }
  std::string oid = lower(p.observerId);
  if (!oid.empty()) {
    Observer *o = findObserver(oid);
    if (o && o->hasLoc) push({o->lat, o->lon}, oid);
    else if (Node *on = findKey(oid); on && on->hasLoc) push({on->lat, on->lon}, oid);
  }

  if (!myKeys.empty()) {
    if (!senderKey.empty()) noteHeard(senderKey);
    for (auto &pt : pts) noteHeard(pt.key);
  }
  if (pts.empty()) return;
  if (!groupOn[g]) return;

  // Area / repeater filter
  if (!repeaterKey.empty()) {
    bool through = false;
    for (auto &pt : pts)
      if (pt.key == repeaterKey) through = true;
    for (auto &h : p.hops)
      if (h.size() >= 4 && startsWith(repeaterKey, lower(h))) through = true;
    if (!through) return;
  } else {
    bool in = false;
    for (auto &pt : pts)
      if (milesBetween(center, pt.ll) <= cfg.radiusMi) in = true;
    if (!in) return;
  }

  // The same transmission is heard by many observers: keep up to 6 distinct paths
  std::string pathKey = p.hash + "|";
  for (auto &pt : pts) pathKey += pt.key.substr(0, 6) + ">";
  if (recentPaths_.count(pathKey)) return;
  recentPaths_[pathKey] = now;
  if (recentPaths_.size() > 1500) {
    for (auto it = recentPaths_.begin(); it != recentPaths_.end();)
      it = (now - it->second > 60000) ? recentPaths_.erase(it) : std::next(it);
    hashCount_.clear();
  }
  int &seen = hashCount_[p.hash];
  seen++;
  if (!p.hash.empty() && seen > 6) return;

  if (now - budgetAt_ >= 1000) {
    budgetAt_ = now;
    routeBudget_ = MAX_ROUTES_PER_SEC;
  }
  if (routeBudget_ <= 0) return;
  routeBudget_--;

  Route r;
  r.group = g;
  r.label = label;
  for (auto &pt : pts) r.pts.push_back(pt.ll);
  Node *sender = senderKey.empty() ? nullptr : findKey(senderKey);
  r.who = !p.name.empty() ? p.name : (sender ? sender->name.str() : "");
  if (p.typeName == "GRP_TXT") {
    r.who = !p.sender.empty() ? p.sender : (!r.who.empty() ? r.who : (!p.channel.empty() ? p.channel : "channel"));
    std::string ch = p.channel;
    if (!ch.empty() && ch[0] != '#') ch = "#" + ch;
    r.txt = ch + (p.text.empty() ? "" : ": " + p.text.substr(0, 90));
  }
  if (r.who.empty()) {
    Node *first = findKey(pts[0].key);
    r.who = first ? first->name.str() : pts[0].key.substr(0, 8);
  }
  r.senderKey = senderKey;
  r.hops = (int)p.hops.size();
  r.hasSnr = p.hasSnr;
  r.snr = p.snr;
  r.obs = p.observerName.substr(0, 30);
  for (auto &pt : pts)
    if (isMine(pt.key)) r.mine = true;
  r.feed = seen <= 1;
  r.ts = now;
  if (r.feed) {
    feed.insert(feed.begin(), r);
    if (feed.size() > 60) feed.pop_back();
  }
  if (onRoute) onRoute(r);
}

// ---------------- channels ----------------
// "Public" (MeshCore's built-in channel) and "#public" (a hashtag channel) are different channels with
// different keys, so the '#' is part of the identity. Only case is ignored.
std::string Model::chKey(const std::string &name) { return lower(name); }

void Model::setChannels(std::vector<ChannelInfo> ch) {
  // keep unread counts we already had
  for (auto &c : ch)
    for (auto &old : channels)
      if (chKey(old.name) == chKey(c.name)) c.unread = old.unread;
  std::sort(ch.begin(), ch.end(), [](const ChannelInfo &a, const ChannelInfo &b) { return a.ts > b.ts; });
  if (ch.size() > 40) ch.resize(40);
  channels = std::move(ch);
}

void Model::setHistory(const std::string &channel, std::vector<ChatMsg> msgs) {
  auto &live = messages[chKey(channel)];
  int64_t lastTs = msgs.empty() ? 0 : msgs.back().ts;
  for (auto &m : live)
    if (m.fresh && m.ts > lastTs) msgs.push_back(m);
  if (msgs.size() > 80) msgs.erase(msgs.begin(), msgs.begin() + (msgs.size() - 80));
  live = std::move(msgs);
}

void Model::addChatMsg(const PacketIn &p) {
  int64_t now = nowMs ? nowMs() : 0;
  std::string k = chKey(p.channel.empty() ? "public" : p.channel);
  auto &list = messages[k];
  // the same message arrives once per observer: merge
  for (int i = (int)list.size() - 1; i >= 0 && i >= (int)list.size() - 15; i--) {
    auto &m = list[i];
    if ((!p.hash.empty() && m.id == p.hash) || (m.who == p.sender && m.text == p.text && std::llabs(m.ts - now) < 120000)) {
      m.rep++;
      if (!p.observerName.empty() && std::find(m.obs.begin(), m.obs.end(), p.observerName) == m.obs.end())
        m.obs.push_back(p.observerName);
      if (onChatMsg) onChatMsg(p.channel, m);
      return;
    }
  }
  ChatMsg m;
  m.id = p.hash;
  m.who = p.sender.empty() ? "unknown" : p.sender.substr(0, 30);
  m.text = p.text.substr(0, 300);
  m.ts = now;
  m.hops = (int)p.hops.size();
  m.hasSnr = p.hasSnr;
  m.snr = p.snr;
  if (!p.observerName.empty()) m.obs.push_back(p.observerName);
  m.fresh = true;
  list.push_back(m);
  if (list.size() > 80) list.erase(list.begin());
  bool found = false;
  for (size_t j = 0; j < channels.size(); j++) {
    if (chKey(channels[j].name) == k) {
      ChannelInfo c = channels[j];
      c.last = m.text;
      c.who = m.who;
      c.ts = now;
      c.count++;
      c.unread++;
      channels.erase(channels.begin() + j);
      channels.insert(channels.begin(), c);
      found = true;
      break;
    }
  }
  if (!found) {
    ChannelInfo c;
    c.hash = p.channel.empty() ? "Public" : p.channel;
    c.name = c.hash;
    c.last = m.text;
    c.who = m.who;
    c.count = 1;
    c.ts = now;
    c.unread = 1;
    channels.insert(channels.begin(), c);
  }
  if (onChatMsg) onChatMsg(p.channel, m);
}

// ---------------- alerts ----------------
static std::string fmtDur(int64_t ms) {
  int m = (int)(ms / 60000);
  char b[32];
  if (m < 60) snprintf(b, sizeof b, "%d min", m);
  else if (m < 48 * 60) {
    if (m % 60) snprintf(b, sizeof b, "%dh %dm", m / 60, m % 60);
    else snprintf(b, sizeof b, "%dh", m / 60);
  } else snprintf(b, sizeof b, "%d days", m / 1440);
  return b;
}

void Model::raiseAlert(Alert a, const std::string &dedupe) {
  int64_t now = nowMs ? nowMs() : 0;
  auto it = alertCooldown_.find(dedupe);
  if (it != alertCooldown_.end() && now - it->second < 60000) return;
  alertCooldown_[dedupe] = now;
  if (alertCooldown_.size() > 400) alertCooldown_.clear();
  static const uint32_t colors[] = {0x3DDC97, 0xFF7A59, 0x39D0FF, 0xFFB547, 0xF472B6, 0x39D0FF};
  a.color = colors[a.kind];
  a.ts = now;
  if (a.body.size() > 160) a.body = a.body.substr(0, 160);
  alerts.insert(alerts.begin(), a);
  if (alerts.size() > 40) alerts.pop_back();
  if (onAlert) onAlert(a);
}

void Model::testAlert() {
  Alert a;
  a.kind = A_TEST;
  a.title = "Test alert";
  a.body = "Alerts are working. Tap to dismiss.";
  raiseAlert(a, "test:" + std::to_string(nowMs ? nowMs() : 0));
}

void Model::noteHeard(const std::string &key) {
  if (!isMine(key)) return;
  int64_t now = nowMs ? nowMs() : 0;
  int64_t prev = myLastHeard_[key];
  myLastHeard_[key] = now;
  silentAlerted_.erase(key);
  if (!cfg.alertMyNodes || !prev) return;
  int64_t gap = now - prev;
  if (gap >= (int64_t)cfg.quietMin * 60000) {
    Node *n = findKey(key);
    Alert a;
    a.kind = A_BACK;
    a.title = (n ? n->name.str() : key.substr(0, 8)) + " is back on the air";
    a.body = "Quiet for " + fmtDur(gap);
    a.key = key;
    if (n && n->hasLoc) {
      a.hasLoc = true;
      a.lat = n->lat;
      a.lon = n->lon;
    }
    raiseAlert(a, "back:" + key);
  }
}

void Model::checkSilent() {
  if (!cfg.alertMyNodes) return;
  int64_t now = nowMs ? nowMs() : 0;
  for (auto &key : myKeys) {
    int64_t last = myLastHeard_[key];
    if (!last || silentAlerted_.count(key)) continue;
    if (now - last >= (int64_t)cfg.silentHours * 3600000LL) {
      silentAlerted_[key] = true;
      Node *n = findKey(key);
      Alert a;
      a.kind = A_SILENT;
      a.title = (n ? n->name.str() : key.substr(0, 8)) + " has gone silent";
      a.body = "Not heard for " + fmtDur(now - last);
      a.key = key;
      if (n && n->hasLoc) {
        a.hasLoc = true;
        a.lat = n->lat;
        a.lon = n->lon;
      }
      raiseAlert(a, "silent:" + key);
    }
  }
}

bool Model::isMuted(const std::string &channel) const {
  if (channel.empty() || cfg.mutedChannels.empty()) return false;
  std::string k = chKey(channel);
  for (auto &m : splitList(cfg.mutedChannels))
    if (chKey(m) == k) return true;
  return false;
}
void Model::setMuted(const std::string &channel, bool muted) {
  if (channel.empty()) return;
  std::string k = chKey(channel), out;
  for (auto &m : splitList(cfg.mutedChannels))
    if (chKey(m) != k) out += (out.empty() ? "" : ", ") + m;
  if (muted) out += (out.empty() ? "" : ", ") + channel;
  cfg.mutedChannels = out;
}

void Model::checkKeywords(const PacketIn &p) {
  if (isMuted(p.channel)) return;
  auto kws = splitList(cfg.keywords);
  if (kws.empty()) return;
  std::string hay = lower(p.sender + " " + p.text);
  for (auto &k : kws) {
    if (hay.find(lower(k)) == std::string::npos) continue;
    Alert a;
    a.kind = A_KEYWORD;
    a.title = (p.channel.empty() ? "Channel" : p.channel) + ": \"" + lower(k) + "\"";
    a.body = (p.sender.empty() ? "" : p.sender + ": ") + p.text;
    a.channel = p.channel;
    raiseAlert(a, "kw:" + p.channel + ":" + p.sender + ":" + p.text.substr(0, 40));
    return;
  }
}

void Model::checkMentions(const PacketIn &p) {
  if (p.text.find('@') == std::string::npos || isMuted(p.channel)) return;
  for (auto &w : watches()) {
    if (!mentionMatch(p.text, w.name)) continue;
    Node *n = w.key.empty() ? nullptr : findKey(w.key);
    Alert a;
    a.kind = A_MENTION;
    a.title = "@" + w.name + " mentioned in " + (p.channel.empty() ? "a channel" : p.channel);
    a.body = (p.sender.empty() ? "" : p.sender + ": ") + p.text;
    a.channel = p.channel;
    a.key = w.key;
    a.openChannel = true;
    if (n && n->hasLoc) {
      a.hasLoc = true;
      a.lat = n->lat;
      a.lon = n->lon;
    }
    raiseAlert(a, "mention:" + w.name + ":" + p.sender + ":" + p.text.substr(0, 40));
  }
}

// ---------------- my nodes + mentions ----------------
void Model::resolveMyNodes() {
  std::vector<std::string> keys;
  for (auto &s : savedMyNodes) keys.push_back(s.first);
  for (auto &entry : splitList(cfg.myNodesText)) {
    std::string e = lower(entry);
    for (auto &n : nodes) {
      if (lower(n.name) == e || (e.size() >= 4 && isHex(e) && startsWith(n.key, e))) {
        if (std::find(keys.begin(), keys.end(), n.key) == keys.end()) keys.push_back(n.key);
        break;
      }
    }
  }
  myKeys = keys;
}

bool Model::isMine(const std::string &key) const {
  return std::find(myKeys.begin(), myKeys.end(), key) != myKeys.end();
}

void Model::setMyNode(const std::string &key0, const std::string &name, bool on) {
  std::string key = lower(key0);
  auto it = std::find_if(savedMyNodes.begin(), savedMyNodes.end(), [&](auto &s) { return s.first == key; });
  if (on && it == savedMyNodes.end()) savedMyNodes.push_back({key, name.substr(0, 32)});
  else if (!on && it != savedMyNodes.end()) savedMyNodes.erase(it);
  else return;
  resolveMyNodes();
  if (!on)
    myCards.erase(std::remove_if(myCards.begin(), myCards.end(), [&](auto &c) { return c.key == key; }), myCards.end());
  if (on && std::none_of(myCards.begin(), myCards.end(), [&](auto &c) { return c.key == key; })) {
    MyNodeCard c;
    c.key = key;
    Node *n = findKey(key);
    c.name = n ? n->name.str() : name;
    c.role = n ? n->role : ROLE_UNKNOWN;
    c.last = n ? n->last : 0;
    if (n && n->hasLoc) {
      c.hasLoc = true;
      c.lat = n->lat;
      c.lon = n->lon;
    }
    myCards.push_back(c);
  }
  if (onSaveLists) onSaveLists();
}

std::vector<Watch> Model::watches() {
  std::vector<Watch> out;
  auto add = [&](const std::string &name0, const std::string &key, int src) {
    Node *n = key.empty() ? nullptr : findKey(key);
    std::string nm = trim(n ? n->name.str() : name0);
    if (nm.size() < 2) return;
    for (auto &w : out)
      if (lower(w.name) == lower(nm)) return;
    out.push_back({key, nm, src});
  };
  for (auto &m : savedMentions) add(m.second, m.first, 0);
  for (auto &n : splitList(cfg.mentionNames)) add(n, "", 1);
  if (cfg.mentionMyNodes)
    for (auto &k : myKeys) {
      Node *n = findKey(k);
      if (n) add(n->name, k, 2);
    }
  return out;
}

const Watch *Model::watchFor(const std::string &key, const std::string &name) {
  static std::vector<Watch> cache;
  cache = watches();
  for (auto &w : cache)
    if ((!key.empty() && w.key == key) || lower(w.name) == lower(name)) return &w;
  return nullptr;
}

void Model::setMention(const std::string &key0, const std::string &name, bool on) {
  std::string key = lower(key0);
  auto it = std::find_if(savedMentions.begin(), savedMentions.end(), [&](auto &s) {
    return (!key.empty() && s.first == key) || (key.empty() && lower(s.second) == lower(name));
  });
  if (on && it == savedMentions.end()) savedMentions.push_back({key, name.substr(0, 32)});
  else if (!on && it != savedMentions.end()) savedMentions.erase(it);
  else return;
  if (onSaveLists) onSaveLists();
}

void Model::setMyCards(std::vector<MyNodeCard> cards) {
  int64_t now = nowMs ? nowMs() : 0;
  for (auto &c : cards) {
    if (c.last) {
      int64_t &mine = myLastHeard_[c.key];
      if (mine < c.last) mine = c.last;
    }
    int64_t age = c.last ? now - c.last : INT64_MAX;
    c.status = age < 2 * 3600000LL ? 0 : age < 24 * 3600000LL ? 1 : 2;
  }
  myCards = std::move(cards);
}

}  // namespace st
