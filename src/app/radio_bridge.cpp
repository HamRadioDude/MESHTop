#include "radio_bridge.h"

#include <algorithm>
#include <cstdio>
#include <functional>

namespace st {

static const char *roleFor(uint8_t t) {
  switch (t) {
    case 1: return "companion";
    case 2: return "repeater";
    case 3: return "room";
    case 4: return "sensor";
    default: return "";
  }
}

static uint64_t contactsSig = 0;

bool bridgeContacts(Model &m, Companion &r, bool force) {
  uint64_t sig = (uint64_t)r.contacts.size() * 1000003ULL;
  for (auto &c : r.contacts) sig = sig * 31 + (uint64_t)(c.lastAdvert / 1000) + (c.hasLoc ? 7 : 0) + c.name.size() + c.type;
  sig = sig * 31 + std::hash<std::string>()(r.selfKey) + (r.hasSelfLoc ? 1 : 0) + (uint64_t)(m.center.lat * 1000);
  if (!force && sig == contactsSig) return false;
  contactsSig = sig;
  for (auto &c : r.contacts) {
    if (c.key.empty()) continue;
    m.upsertNode(c.key.str(), c.name.str(), roleFor(c.type), c.hasLoc, c.lat, c.lon, c.lastAdvert);
  }
  if (!r.selfKey.empty()) {
    // your own radio, so streaks end at you; with no position set on the radio the area centre stands in
    bool own = r.hasSelfLoc && !(r.selfLat == 0 && r.selfLon == 0);
    m.upsertNode(r.selfKey, r.selfName.empty() ? "Your radio" : r.selfName, "companion", true, own ? r.selfLat : m.center.lat,
                 own ? r.selfLon : m.center.lon, 0);
  }
  return true;
}

void bridgeHeard(Model &m, Companion &r, const HeardPkt &h) {
  static const char *names[16] = {"REQ",  "RESPONSE", "TXT_MSG",   "ACK",     "ADVERT", "GRP_TXT", "GRP_DATA", "ANON_REQ",
                                  "PATH", "TRACE",    "MULTIPART", "CONTROL", "TYPE12", "TYPE13",  "TYPE14",   "RAW"};
  PacketIn p;
  p.typeName = names[h.type & 15];
  char idb[12];
  snprintf(idb, sizeof idb, "%08x", (unsigned)h.id);
  p.hash = idb;
  p.hops = h.hops;
  p.hasSnr = true;
  p.snr = h.snr;
  p.observerId = r.selfKey;
  p.observerName = "Your radio";
  if (h.type == 4) {
    p.pubKey = h.key;
    p.name = h.name;
    p.hasLoc = h.hasLoc;
    p.lat = h.lat;
    p.lon = h.lon;
  } else if (h.type == 5) {
    p.channel = h.channel;
    p.sender = h.sender;
    p.text = h.text;
    p.noChatter = true;  // CHATTER is hidden (MESH shows your radio's chats); keyword and @mention alerts still run
  } else if (h.type == 2 && h.srcHash >= 0) {
    // a direct message: name the sender when exactly one contact has that 1-byte hash
    char hx[3];
    snprintf(hx, sizeof hx, "%02x", h.srcHash);
    const RContact *who = nullptr;
    int count = 0;
    for (auto &c : r.contacts)
      if (c.key.size() >= 2 && strncmp(c.key.c_str(), hx, 2) == 0) {
        who = &c;
        count++;
      }
    if (count == 1) p.name = who->name.str();
  }
  m.handlePacket(p);
  if (h.type == 4 && !h.key.empty() && h.role) m.upsertNode(h.key, "", roleFor(h.role), false, 0, 0, 0);
}

void radioOnlyCards(Model &m) {
  int64_t now = m.nowMs ? m.nowMs() : 0;
  std::vector<MyNodeCard> cards;
  for (auto &k : m.myKeys) {
    Node *n = m.findKey(k);
    MyNodeCard c;
    c.key = k;
    c.name = n ? n->name.str() : k.substr(0, 8);
    c.role = n ? n->role : ROLE_UNKNOWN;
    c.last = std::max(n ? n->last : 0, m.lastHeardMine(k));
    c.status = c.last && now - c.last < (int64_t)m.cfg.silentHours * 3600000LL ? 0 : 2;
    if (n && n->hasLoc) {
      c.hasLoc = true;
      c.lat = n->lat;
      c.lon = n->lon;
    }
    cards.push_back(c);
  }
  m.setMyCards(cards);
}

void clearForSourceSwitch(Model &m) {
  m.nodes.clear();
  m.nodeIndex.clear();
  m.feed.clear();
  m.channels.clear();
  m.messages.clear();
  m.observers.clear();
  m.myCards.clear();
  m.directoryReady = false;
  m.resetLive();
  contactsSig = 0;
}

}  // namespace st
