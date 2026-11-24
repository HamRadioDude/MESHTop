#include "core/model.h"
#include <cstdio>
#include <cassert>
using namespace st;
static int64_t T = 1794800000000LL;
int main() {
  Model m; m.nowMs = [] { return T; };
  int routes = 0; std::vector<Alert> alerts; Route last;
  m.onRoute = [&](const Route &r) { routes++; last = r; };
  m.onAlert = [&](const Alert &a) { alerts.push_back(a); };
  m.cfg.radiusMi = 40; m.cfg.keywords = "anyone"; m.cfg.mentionNames = "Bravo-RPT"; m.cfg.alertNewNodes = true;
  auto K = [](char c, char d) { return std::string(1, c) + std::string(1, d) + std::string(62, d); };
  m.upsertNode(K('a','1'), "Alpha-RPT", "repeater", true, 41.90, -87.70, 0);
  m.upsertNode(K('b','2'), "Bravo-RPT", "repeater", true, 41.80, -87.90, 0);
  m.upsertNode(K('c','3'), "Charlie", "companion", true, 41.70, -87.60, T - 3*3600000LL);
  m.upsertNode(K('d','4'), "FarAway", "repeater", true, 34.05, -118.24, 0);
  Observer o; o.id = K('f','6'); o.name = "Obs-Home"; o.hasLoc = true; o.lat = 41.88; o.lon = -87.63; m.setObservers({o});
  m.cfg.myNodesText = "Charlie"; m.markDirectoryReady();
  MyNodeCard c; c.key = K('c','3'); c.name = "Charlie"; c.last = T - 3*3600000LL; m.setMyCards({c});
  T += 1000;
  // 1 advert via resolved path
  PacketIn p; p.hash="h1"; p.typeName="ADVERT"; p.pubKey=K('a','1'); p.name="Alpha-RPT"; p.hasLoc=true; p.lat=41.9; p.lon=-87.7;
  p.hops={"b2"}; p.resolved={K('b','2')}; p.observerId=K('f','6'); p.observerName="Obs-Home"; m.handlePacket(p);
  printf("advert route pts=%zu (expect 3)\n", last.pts.size());
  // 2 channel msg via prefixes, keyword + mention
  PacketIn q; q.hash="h2"; q.typeName="GRP_TXT"; q.channel="#chicago"; q.sender="KB9"; q.text="anyone hear @[Bravo-RPT]?"; q.hops={"a1","b2"}; q.observerId=K('f','6'); m.handlePacket(q);
  printf("channel route who=%s txt=%s pts=%zu\n", last.who.c_str(), last.txt.c_str(), last.pts.size());
  // 3 far away advert dropped (LA hop to Chicago observer > 250mi)
  int before = routes; PacketIn f; f.hash="h3"; f.typeName="ADVERT"; f.pubKey=K('d','4'); f.observerId=K('f','6'); m.handlePacket(f);
  printf("far advert routes added=%d (expect 0)\n", routes - before);
  // 4 control filtered by default
  before = routes; PacketIn a; a.hash="h4"; a.typeName="ACK"; a.hops={"a1"}; a.observerId=K('f','6'); m.handlePacket(a);
  printf("ack routes added=%d (expect 0)\n", routes - before);
  // 5 duplicate same path dropped
  before = routes; m.handlePacket(q); printf("dup routes added=%d (expect 0)\n", routes - before);
  // 6 trace with Charlie (my node, quiet 3h) -> back alert
  PacketIn t; t.hash="h5"; t.typeName="TRACE"; t.hops={"c3","a1"}; t.observerId=K('f','6'); m.handlePacket(t);
  // 7 new node advert
  PacketIn n; n.hash="h6"; n.typeName="ADVERT"; n.pubKey=K('a','7'); n.name="Brand-New"; n.hasLoc=true; n.lat=41.85; n.lon=-87.65; n.observerId=K('f','6'); m.handlePacket(n);
  for (auto &al : alerts) printf("ALERT [%d] %s | %s\n", al.kind, al.title.c_str(), al.body.c_str());
  printf("mentionMatch: %d %d %d %d\n", mentionMatch("@[Ridge-RPT] up?","Ridge-RPT"), mentionMatch("hi @ridge-rpt!","Ridge-RPT"), mentionMatch("@Ridge-RPT2","Ridge-RPT"), mentionMatch("Ridge-RPT","Ridge-RPT"));
  printf("channels=%zu first=%s unread=%d msgs=%zu feed=%zu\n", m.channels.size(), m.channels[0].name.c_str(), m.channels[0].unread, m.messages["chicago"].size(), m.feed.size());
  // silent check: NoGps-like node never heard -> uses lastHeard from card
  T += 7*3600000LL; m.checkSilent(); printf("after 7h silent alerts total=%zu last=%s\n", alerts.size(), alerts.back().title.c_str());
}
