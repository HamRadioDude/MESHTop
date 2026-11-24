// ScopeThing core model: portable C++ (no Arduino), shared by the panel firmware and the desktop simulator.
// Everything here runs on ONE thread (the UI loop). The network task only hands over parsed data.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pstr.h"

namespace st {

struct LatLon {
  double lat = 0, lon = 0;
};

enum Role : uint8_t { ROLE_UNKNOWN = 0, ROLE_COMPANION, ROLE_REPEATER, ROLE_ROOM, ROLE_SENSOR, ROLE_OBSERVER };
Role parseRole(const std::string &r);
const char *roleName(Role r);

// Packet groups used by the feed filter (same as the Car Thing version)
enum Group : uint8_t { G_ADVERT = 0, G_CHANNEL, G_DM, G_TRACE, G_PATH, G_CONTROL, G_COUNT };
const char *groupName(Group g);
uint32_t groupColor(Group g);  // 0xRRGGBB

struct Node {
  PStr key;  // full public key, lower-case hex (stored in external RAM)
  PStr name;
  Role role = ROLE_UNKNOWN;
  bool hasLoc = false;
  double lat = 0, lon = 0;
  int64_t last = 0;  // epoch ms, 0 = unknown
};

struct Observer {
  std::string id, name, iata;
  bool hasLoc = false;
  double lat = 0, lon = 0;
  int64_t last = 0;
  int pkh = 0;  // packets last hour
};

// One decoded packet from CoreScope's live feed (already parsed from JSON)
struct PacketIn {
  std::string hash, typeName, observerId, observerName;
  std::vector<std::string> hops;      // hop prefixes (hex)
  std::vector<std::string> resolved;  // full keys per hop, "" when unknown
  bool hasSnr = false;
  float snr = 0;
  // decoded payload
  std::string pubKey, name, channel, sender, text;
  bool hasLoc = false;
  double lat = 0, lon = 0;
  bool noChatter = false;  // radio only: CHATTER is hidden, so the message isn't added there (alerts still run)
};

struct Route {
  std::vector<LatLon> pts;
  Group group = G_CONTROL;
  std::string label, who, txt, obs, senderKey;
  int hops = 0;
  bool hasSnr = false;
  float snr = 0;
  bool mine = false;
  bool feed = true;  // first copy of this transmission (others only draw streaks)
  int64_t ts = 0;
};

struct ChannelInfo {
  std::string hash, name, last, who;
  int count = 0;
  int64_t ts = 0;
  int unread = 0;
};

struct ChatMsg {
  std::string id, who, text;
  int64_t ts = 0;
  int rep = 1, hops = -1;
  bool hasSnr = false;
  float snr = 0;
  std::vector<std::string> obs;
  bool fresh = false;
};

enum AlertKind : uint8_t { A_BACK = 0, A_SILENT, A_KEYWORD, A_NEWNODE, A_MENTION, A_TEST };
struct Alert {
  AlertKind kind = A_TEST;
  std::string title, body, channel, key;
  bool hasLoc = false;
  double lat = 0, lon = 0;
  uint32_t color = 0xE6EDF3;
  int64_t ts = 0;
  bool openChannel = false;
  std::string radioConv;  // MeshThing conversation to open
};

struct MyNodeCard {
  std::string key, name;
  Role role = ROLE_UNKNOWN;
  int64_t last = 0;
  bool hasSnr = false;
  float avgSnr = 0;
  int today = -1, observers = -1;
  bool hasLoc = false;
  double lat = 0, lon = 0;
  int status = 2;  // 0 active, 1 degraded, 2 silent
};

struct Watch {  // a node watched for @mentions
  std::string key, name;
  int src = 0;  // 0 picked, 1 settings, 2 my node
};

struct Settings {
  std::string server = "https://corescope.chicagolandmesh.org";
  std::string area = "Chicago, IL";
  int radiusMi = 60;
  std::string watchRepeater;
  std::string myNodesText;   // comma list from the setup page
  std::string keywords;      // comma list
  std::string mentionNames;  // comma list
  std::string mapStyle = "dark";  // dark | light | none
  bool showControl = false;
  bool alertMyNodes = true;
  int quietMin = 60;
  int silentHours = 6;
  bool alertNewNodes = false;
  bool mentionMyNodes = true;
  int idleMin = 5;                // minutes untouched before going back to the map and dimming (0 = never)
  std::string dimMode = "auto";  // when idle: auto (at night) | always | off
  bool feedFolded = false;       // the map's live traffic panel folded down to one line
  std::string theme = "auto";    // auto (day after sunrise) | dark | day
  bool radioOn = false;          // connect to a MeshCore companion radio over Bluetooth
  bool useCoreScope = true;      // false = radio only: CoreScope is never contacted (forces radioOn)
  bool radioOnly() const { return !useCoreScope; }
  int radioPin = 123456;
  std::string radioName;         // optional BLE name filter
  bool chatsToSd = true;         // keep your radio's chats on the SD card (survive restarts; the web page shows them)
  bool phoneMode = false;        // leave messages in the radio's inbox for the phone app
  bool alertSound = true;
  bool demo = false;
  std::string mutedChannels;  // comma list of channel names that never alert (CoreScope chatter and your radio alike)
};

double milesBetween(const LatLon &a, const LatLon &b);
std::string lower(std::string s);
std::vector<std::string> splitList(const std::string &s);
bool mentionMatch(const std::string &text, const std::string &name);

class Model {
 public:
  Settings cfg;
  LatLon center{41.8781, -87.6298};
  std::string areaLabel = "Chicago, IL";
  std::string repeaterKey, repeaterName;
  bool directoryReady = false;

  std::vector<Node, ExtAlloc<Node>> nodes;
  std::unordered_map<uint64_t, int, std::hash<uint64_t>, std::equal_to<uint64_t>, ExtAlloc<std::pair<const uint64_t, int>>> nodeIndex;
  std::vector<Observer> observers;

  std::vector<Route> feed;  // newest first, max 60
  std::vector<ChannelInfo> channels;
  std::unordered_map<std::string, std::vector<ChatMsg>> messages;  // by lower(channel name without #)
  std::vector<Alert> alerts;                                       // newest first
  std::vector<MyNodeCard> myCards;
  std::vector<std::pair<std::string, std::string>> savedMyNodes;   // key, name
  std::vector<std::pair<std::string, std::string>> savedMentions;  // key, name
  std::vector<std::string> myKeys;  // resolved My Nodes keys
  bool groupOn[G_COUNT] = {true, true, true, true, true, false};

  // stats
  std::vector<int64_t> pktTimes;
  int pktPerMin(int64_t now);

  // callbacks to the UI / platform
  std::function<void(const Route &)> onRoute;
  std::function<void(const Alert &)> onAlert;
  std::function<void(const std::string &channel, const ChatMsg &)> onChatMsg;
  std::function<void()> onSaveLists;  // persist savedMyNodes / savedMentions
  std::function<int64_t()> nowMs;

  // directory
  Node *findKey(const std::string &key);
  Node *upsertNode(const std::string &key, const std::string &name, const std::string &role, bool hasLoc, double lat,
                   double lon, int64_t last);
  void setObservers(std::vector<Observer> obs);
  Observer *findObserver(const std::string &id);
  Node *findByName(const std::string &q);
  std::vector<Node *> search(const std::string &q, size_t max = 40);
  void markDirectoryReady();

  // live packets
  void handlePacket(const PacketIn &p);

  // my nodes + mentions
  void resolveMyNodes();
  bool isMine(const std::string &key) const;
  void setMyNode(const std::string &key, const std::string &name, bool on);
  std::vector<Watch> watches();
  const Watch *watchFor(const std::string &key, const std::string &name);
  void setMention(const std::string &key, const std::string &name, bool on);
  void setMyCards(std::vector<MyNodeCard> cards);  // from health API (or demo)
  void checkSilent();
  void resetLive() {  // forget heard-times, packet rates and de-duplication (switching data source)
    recentPaths_.clear();
    hashCount_.clear();
    myLastHeard_.clear();
    silentAlerted_.clear();
    pktTimes.clear();
  }
  int64_t lastHeardMine(const std::string &key) const {  // when one of My Nodes was last heard on any path (0 = not yet)
    auto it = myLastHeard_.find(key);
    return it == myLastHeard_.end() ? 0 : it->second;
  }

  // channels
  void setChannels(std::vector<ChannelInfo> ch);
  void setHistory(const std::string &channel, std::vector<ChatMsg> msgs);
  static std::string chKey(const std::string &name);

  void raiseAlert(Alert a, const std::string &dedupe);
  // a muted channel raises no alerts or sounds (keywords and @mentions included); one list for the
  // CoreScope chatter and your radio's channels, matched by name ("Public", "#test")
  bool isMuted(const std::string &channel) const;
  void setMuted(const std::string &channel, bool muted);
  void testAlert();

  // filters
  static Group groupOfType(const std::string &typeName, std::string &label);

 private:
  std::unordered_map<std::string, int64_t> recentPaths_;
  std::unordered_map<std::string, int> hashCount_;
  std::unordered_map<std::string, int64_t> alertCooldown_;
  std::unordered_map<std::string, int64_t> myLastHeard_;
  std::unordered_map<std::string, bool> silentAlerted_;
  int routeBudget_ = 12;
  int64_t budgetAt_ = 0;

  Node *nodeByPrefix(const std::string &prefix, const LatLon *near);
  void noteHeard(const std::string &key);
  void checkKeywords(const PacketIn &p);
  void checkMentions(const PacketIn &p);
  void addChatMsg(const PacketIn &p);
};

}  // namespace st
