#include "demo.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace st {

static const char *NAMES[] = {"Ridge", "Harbor", "Mill", "Prairie", "Tower", "Creek", "Summit", "Depot", "Grove", "Bluff", "Lakeside", "Junction"};
static const char *CHAT[] = {"Anyone on?", "Copy that", "Testing from the car", "Good signal here", "73", "Hearing you 2 hops out",
                             "New repeater is up on the water tower", "Net tonight at 8"};

static std::string key(const char *pfx, int i) {
  char b[80];
  snprintf(b, sizeof b, "%s%02x", pfx, i);
  std::string k = b;
  k.resize(64, '0');
  return k;
}

void Demo::seed(Model &m, int64_t now) {
  m.nodes.clear();
  m.nodeIndex.clear();
  double spread = std::min(m.cfg.radiusMi, 60) / 69.0;
  double lonScale = 1 / std::max(0.2, std::cos(m.center.lat * M_PI / 180));
  auto r = [&](double s) { return (frand() * 2 - 1) * s; };
  for (int i = 0; i < 12; i++)
    m.upsertNode(key("d0de", i), std::string(NAMES[i]) + "-RPT", "repeater", true, m.center.lat + r(spread * 0.8),
                 m.center.lon + r(spread * 0.8 * lonScale), now - (int64_t)(frand() * 3600000));
  for (int i = 0; i < 26; i++)
    m.upsertNode(key("c0de", i), "Demo-" + std::to_string(i + 1), i % 9 == 4 ? "room" : "companion", true,
                 m.center.lat + r(spread), m.center.lon + r(spread * lonScale), now - (int64_t)(frand() * 7200000));
  std::string ok = key("0b5e", 0);
  m.upsertNode(ok, "Observer", "observer", true, m.center.lat, m.center.lon, now);
  Observer o;
  o.id = ok;
  o.name = "Demo Observer";
  o.iata = "DEMO";
  o.hasLoc = true;
  o.lat = m.center.lat;
  o.lon = m.center.lon;
  o.last = now;
  o.pkh = 240;
  Observer o2 = o;
  o2.id = key("0b5e", 1);
  o2.name = "Rooftop Observer";
  o2.pkh = 131;
  o2.lat += 0.12;
  o2.last = now - 120000;
  m.setObservers({o, o2});
  m.markDirectoryReady();
}

int Demo::tick(Model &m, int64_t now) {
  std::vector<Node *> rpts, comps;
  for (auto &n : m.nodes) {
    if (n.role == ROLE_REPEATER) rpts.push_back(&n);
    else if (n.role != ROLE_OBSERVER) comps.push_back(&n);
  }
  if (rpts.empty() || comps.empty() || m.observers.empty()) return 1000;
  static const char *types[] = {"ADVERT", "GRP_TXT", "GRP_TXT", "TXT_MSG", "TRACE", "PATH", "ADVERT", "GRP_TXT"};
  std::string t = types[rnd() % 8];
  Node *src = (t == "ADVERT" && (rnd() & 1)) ? rpts[rnd() % rpts.size()] : comps[rnd() % comps.size()];
  const Observer &obs = m.observers[rnd() % m.observers.size()];
  int hopsN = 1 + rnd() % 4;
  PacketIn p;
  LatLon cur{src->lat, src->lon};
  std::vector<Node *> used;
  for (int i = 0; i < hopsN; i++) {
    std::vector<Node *> c;
    for (auto *r : rpts)
      if (std::find(used.begin(), used.end(), r) == used.end()) c.push_back(r);
    std::sort(c.begin(), c.end(), [&](Node *a, Node *b) { return milesBetween(cur, {a->lat, a->lon}) < milesBetween(cur, {b->lat, b->lon}); });
    if (c.empty()) break;
    Node *pick = c[rnd() % std::min<size_t>(3, c.size())];
    used.push_back(pick);
    p.hops.push_back(pick->key.substr(0, 4));
    p.resolved.push_back(pick->key);
    cur = {pick->lat, pick->lon};
  }
  char h[20];
  snprintf(h, sizeof h, "%08x%04x", rnd(), rnd() & 0xFFFF);
  p.hash = h;
  p.typeName = t;
  p.observerId = obs.id;
  p.observerName = obs.name;
  p.hasSnr = true;
  p.snr = std::round((frand() * 18 - 6) * 4) / 4;
  p.pubKey = src->key;
  if (t == "GRP_TXT") {
    p.channel = (rnd() % 3 == 0) ? "#chicago" : "Public";
    p.sender = src->name;
    p.text = CHAT[rnd() % 8];
  } else {
    p.name = src->name;
  }
  m.handlePacket(p);
  return 350 + (int)(rnd() % 1400);
}

void Demo::channels(Model &m, int64_t now) {
  std::vector<ChannelInfo> ch;
  ChannelInfo a;
  a.hash = "Public";
  a.name = "Public";
  a.last = "Good signal here";
  a.who = "Demo-3";
  a.count = 1284;
  a.ts = now - 60000;
  ch.push_back(a);
  ChannelInfo b = a;
  b.hash = b.name = "#chicago";
  b.last = "Net tonight at 8";
  b.who = "Demo-11";
  b.count = 342;
  b.ts = now - 420000;
  ch.push_back(b);
  ChannelInfo c = a;
  c.hash = c.name = "#emcomm";
  c.last = "Drill Saturday 0900";
  c.who = "Demo-7";
  c.count = 57;
  c.ts = now - 3 * 3600000;
  ch.push_back(c);
  m.setChannels(ch);
}

void Demo::history(Model &m, const std::string &channel, int64_t now) {
  std::vector<ChatMsg> v;
  for (int i = 0; i < 6; i++) {
    ChatMsg c;
    c.id = "demo" + std::to_string(i);
    c.who = "Demo-" + std::to_string(i + 2);
    c.text = CHAT[(i + channel.size()) % 8];
    c.ts = now - (6 - i) * 240000;
    c.rep = 1 + i;
    c.obs = {"Demo Observer"};
    c.hops = i % 4;
    c.hasSnr = true;
    c.snr = 4 - i * 1.5f;
    v.push_back(c);
  }
  m.setHistory(channel, v);
}

void Demo::myCards(Model &m, int64_t now) {
  std::vector<MyNodeCard> cards;
  int i = 0;
  for (auto &k : m.myKeys) {
    Node *n = m.findKey(k);
    MyNodeCard c;
    c.key = k;
    c.name = n ? n->name.str() : k.substr(0, 8);
    c.role = n ? n->role : ROLE_UNKNOWN;
    c.last = i % 4 == 3 ? now - 30 * 3600000LL : (i % 4 == 2 ? now - 5 * 3600000LL : now - (60 + i * 97) * 1000LL);
    c.hasSnr = true;
    c.avgSnr = 6.5f - i * 1.75f;
    c.today = 412 - i * 57;
    c.observers = 3 + (i % 3);
    if (n && n->hasLoc) {
      c.hasLoc = true;
      c.lat = n->lat;
      c.lon = n->lon;
    }
    cards.push_back(c);
    i++;
  }
  m.setMyCards(cards);
}

}  // namespace st
