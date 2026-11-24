// MeshThing: MeshCore companion-radio protocol client (the protocol the MeshCore phone app speaks).
// Portable C++: the panel feeds it BLE notifications, the simulator feeds it a fake radio.
// Runs on the UI thread only. Protocol per meshcore-dev/MeshCore examples/companion_radio/MyMesh.cpp.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "pstr.h"

namespace st {

enum RadioState : uint8_t { R_OFF = 0, R_SCANNING, R_CONNECTING, R_PAIRING, R_SYNCING, R_READY, R_ERROR };
const char *radioStateName(RadioState s);

enum ContactType : uint8_t { CT_NONE = 0, CT_CHAT = 1, CT_REPEATER = 2, CT_ROOM = 3, CT_SENSOR = 4 };

struct RContact {
  PStr key;  // full public key, hex (external RAM)
  PStr name;
  uint8_t type = 0;
  uint8_t flags = 0;  // bit0 favourite
  int pathLen = -1;   // -1 = no known route yet (messages flood)
  int64_t lastAdvert = 0;
  bool hasLoc = false;
  double lat = 0, lon = 0;
  std::vector<uint8_t, ExtAlloc<uint8_t>> raw;  // the contact frame as the radio sent it (needed to update flags)
  bool fav() const { return flags & 1; }
};

enum ChanKind : uint8_t { CK_PUBLIC, CK_HASHTAG, CK_PRIVATE };
struct RChannel {
  int idx = 0;
  std::string name;
  uint8_t secret[16] = {0};
  ChanKind kind = CK_PRIVATE;
  std::string secretHex() const;
  std::string shareLink() const;  // meshcore://channel/add?name=..&secret=..
};

enum MsgStatus : uint8_t { MS_NONE = 0, MS_SENDING, MS_SENT, MS_DELIVERED, MS_FAILED };

// One repeater heard re-sending a message you sent (from the radio's raw-receive log, push 0x88)
struct Repeat {
  std::string hop;  // hex ID of the repeater that sent this copy (last path entry), 1-3 bytes
  int hops = 0;     // path length of the copy we heard
  float snr = 0;    // as heard by your radio, dB
  int rssi = 0;     // dBm
  int64_t at = 0;
};
// One copy of someone else's message as your radio heard it (raw-receive log, push 0x88)
struct RxPath {
  std::vector<std::string> hops;  // repeaters it passed, sender's side first (hex path hashes, 1-3 bytes)
  bool direct = false;            // came along a set route: repeaters remove themselves, so none are visible
  float snr = 0;                  // as heard by your radio, dB
  int rssi = 0;                   // dBm
  int64_t at = 0;
};
struct RMsg {
  std::string who, text;
  int64_t ts = 0;
  bool mine = false;
  MsgStatus status = MS_NONE;
  uint32_t ack = 0;
  int64_t ackDeadline = 0;
  bool hasSnr = false;
  float snr = 0;
  int hops = -1;
  uint32_t sentTs = 0;          // the timestamp sent inside the packet (seconds), to recognise its repeats
  std::vector<Repeat> heard;    // repeaters your radio heard re-sending it (channel messages you sent)
  std::vector<RxPath> paths;    // someone else's message: every copy your radio heard, and the route each took
  uint32_t pkt = 0;             // direct message: id of the packet it came in (to attach later copies)
};

struct Conv {
  std::string id;  // "c:<idx>" channel or "d:<12 hex key prefix>" direct
  std::string title;
  bool isChannel = true;
  int chIdx = 0;
  std::string key;  // DM: full key if known, else prefix
  std::vector<RMsg> msgs;
  int unread = 0;
  int64_t lastTs = 0;
};

// A trace route (CMD_SEND_TRACE_PATH / PUSH_CODE_TRACE_DATA): the packet goes out along a route of
// repeaters and back; every repeater it passes adds the SNR it heard the previous hop with.
// One way of sending a trace: the hop codes out and back, and how far your radio heard it get.
struct TraceTry {
  std::vector<std::string> route;  // hop hashes sent out and back, hex
  int turnAt = 0;                  // index in route of the repeater you traced (where it turns back)
  int width = 1;                   // bytes per hop code
  bool viaSaved = false;           // along the saved route (else straight to the repeater and back)
  int heardHops = 0;               // repeaters your radio heard pass it on (from its receive log)
  float heardSnr = 0;              // how well your radio heard the latest of them
};

struct TraceRun {
  std::string target;              // contact key (hex) the trace was aimed at
  std::vector<std::string> route;  // this try's hop hashes, out and back
  int turnAt = 0;                  // index in route of the repeater you traced
  uint32_t tag = 0;
  int64_t started = 0, deadline = 0;
  bool waiting = false, done = false, failed = false;
  std::vector<float> snrs;         // SNR heard at each hop (same order as route)
  float finalSnr = 0;              // SNR your radio heard the last hop with
  std::string note;                // why it couldn't run, if it couldn't
  bool viaSaved = false;           // this try goes along the saved route (else straight to the repeater and back)
  bool secondTry = false;          // the first way got no reply, so this is the other way
  bool retriedDirect = false;      // the second try is straight to it (the saved route didn't answer)
  int heardHops = 0;               // this try: repeaters your radio heard pass it on so far
  float heardSnr = 0;
  bool fromRxLog = false;          // finished from what your radio heard, not its own trace report
  TraceTry alt;                    // the other way, if this one gets no reply (route empty = none)
  TraceTry first;                  // the first way, once it got no reply (route empty = n/a)
};

// A packet your radio heard (from its raw receive log), described for radio-only mode, where the
// panel's map and live traffic come from your radio instead of CoreScope.
struct HeardPkt {
  uint8_t type = 0;               // MeshCore payload type (4 advert, 5 channel text, 2 direct text, 9 trace...)
  bool flood = true;
  std::vector<std::string> hops;  // path hash per repeater, hex, sender's side first (flood only)
  float snr = 0;
  int rssi = 0;
  uint32_t id = 0;                // packet id: the same for every copy of one transmission
  std::string key, name;          // adverts: the advertiser's full key (hex) and name
  uint8_t role = 0;               // adverts: 1 companion, 2 repeater, 3 room, 4 sensor
  bool hasLoc = false;
  double lat = 0, lon = 0;
  std::string channel, sender, text;  // channel messages your radio can read
  int srcHash = -1;               // direct messages: the sender's 1-byte hash
};

// Your radio's own counters (CMD_GET_STATS, companion firmware code 8+; MeshCore companion_radio/MyMesh.cpp)
struct RadioStats {
  bool core = false, radio = false, packets = false;
  int64_t at = 0;
  int battMv = 0, errFlags = 0, queueLen = 0;
  uint32_t upSecs = 0;
  int noiseFloor = 0, lastRssi = 0;  // dBm
  float lastSnr = 0;                 // dB
  uint32_t txAirSecs = 0, rxAirSecs = 0;
  uint32_t recv = 0, sent = 0, sentFlood = 0, sentDirect = 0, recvFlood = 0, recvDirect = 0, recvErrors = 0;
};

// "Who can hear me?": a zero-hop discovery request (CTL_TYPE_NODE_DISCOVER_REQ). Repeaters and
// sensors that hear it answer with how well they heard you; your radio notes how well it heard them.
struct Neighbour {
  std::string key;  // public key prefix (hex, 8 bytes) or full key
  uint8_t type = 0; // ADV_TYPE: 2 repeater, 3 room, 4 sensor
  float theyHear = 0, weHear = 0;  // SNR dB each way
  int rssi = 0;
  int64_t at = 0;
};
struct DiscoverRun {
  uint32_t tag = 0;
  int64_t started = 0, until = 0;
  bool running = false, done = false, failed = false;
  std::string note;
  std::vector<Neighbour> found;
};

// A repeater's neighbours: other repeaters it hears directly (REQ_TYPE_GET_NEIGHBOURS, after a login)
struct RNeighbour {
  std::string prefix;   // 6-byte key prefix, hex
  uint32_t secsAgo = 0; // since it last heard them
  float snr = 0;        // how well it hears them, dB
};
// Path discovery: a fresh route to someone and back (CMD_SEND_PATH_DISCOVERY_REQ, push 0x8D)
struct PathDiscovery {
  std::string key;      // full key of the contact
  bool waiting = false, done = false, failed = false;
  uint32_t tag = 0;
  int64_t started = 0, deadline = 0;
  std::vector<std::string> out, back;  // hop codes (hex), from your end: the way there, and the way back
  std::string note;
};
// Repeater / room server status (RepeaterStats in simple_repeater/MyMesh.h)
struct RemoteStatus {
  std::vector<RNeighbour> neigh;  // neighbours, strongest first
  int neighTotal = 0;
  bool waitingNeigh = false, hasNeigh = false;
  uint32_t neighTag = 0;
  int64_t neighAt = 0, neighDeadline = 0;
  std::vector<std::string> cli;   // remote admin console: "> command" lines and the repeater's replies
  bool waitingCli = false;
  bool loggedIn = false, isAdmin = false, loginFailed = false, waitingLogin = false, waitingStatus = false;
  bool hasStats = false;
  int64_t at = 0;
  int battMv = 0, txQueue = 0, noiseFloor = 0, lastRssi = 0, errEvents = 0;
  float lastSnr = 0;
  uint32_t recv = 0, sent = 0, airSecs = 0, upSecs = 0, sentFlood = 0, sentDirect = 0, recvFlood = 0, recvDirect = 0;
};

class Companion {
 public:
  RadioState state = R_OFF;
  std::string status = "Radio off";
  std::string devName;  // BLE name, e.g. MeshCore-DudeXIAO
  std::string selfName, selfKey, fwVersion, model;
  double freqMHz = 0, bwKHz = 0;
  int sf = 0, cr = 0, txPower = 0, maxTxPower = 22, maxChannels = 8;
  int battMv = -1;
  bool hasSelfLoc = false;
  double selfLat = 0, selfLon = 0;
  uint8_t multiAcks = 0, locPolicy = 0, telemetry = 0, manualAdd = 0;
  int fwCode = 0;  // FIRMWARE_VER_CODE from the device query (stats and discovery need 8+)
  RadioStats stats;
  bool statsUnsupported = false;
  DiscoverRun discovery;
  bool selfInfoValid = false;
  std::vector<RContact> contacts;
  std::vector<RChannel> channels;  // configured slots only
  std::vector<Conv> convs;         // channels first (by index), then DMs (most recent first)
  std::map<std::string, RemoteStatus> remote;  // by 12-hex key prefix

  // wiring
  std::function<void(const std::vector<uint8_t> &)> write;  // transport: send one frame
  std::function<void()> onChange;
  std::function<void(const Conv &, const RMsg &)> onMessage;      // incoming message (not ours)
  std::function<void(bool ok, const std::string &text)> onResult;  // toast for settings changes etc.
  std::function<int64_t()> nowMs;
  std::function<bool()> clockValid;
  std::function<void(uint8_t *, size_t)> randomBytes;  // for new private channel keys
  std::function<void(const std::string &)> log;        // optional: one-line diagnostics (the panel prints them)
  std::function<void(const HeardPkt &)> onHeard;       // optional: every packet your radio hears (radio-only mode)
  std::function<void(const Conv &, const RMsg &)> onStore;  // optional: a message to keep (sent or received)

  // "Leave messages for my phone": don't collect messages from the radio's inbox, so the phone app still
  // gets them. Channel messages are read from what the radio hears instead (needs the channel's key);
  // direct messages stay on the radio for the phone. Remote-admin answers are still collected while
  // a console command is waiting.
  bool leaveForPhone = false;
  // Put saved messages back (from the SD card) without counting them as unread. Channels by name.
  void restoreMsg(bool isChannel, const std::string &chanName, const std::string &dmKey, const RMsg &m);

  void setState(RadioState s, const std::string &msg);
  void connected();     // transport is up: start the handshake
  void disconnected();  // transport lost
  void frame(const uint8_t *d, size_t n);
  void tick();  // timeouts, periodic battery

  // messaging
  bool sendText(const std::string &convId, const std::string &text);
  void sendAdvert(bool flood = true);
  void syncNow();
  Conv *conv(const std::string &id);
  Conv &openDm(const RContact &c);  // creates the conversation if needed
  RContact *contactByPrefix(const std::string &hexPrefix);
  // a repeater's name from your contacts by its path hash (1-3 bytes hex), "" if unknown
  std::string nameForHop(const std::string &hopHex);
  int unreadTotal() const;
  void markRead(const std::string &id);

  // channels
  int freeChannelSlot() const;
  bool addHashtagChannel(const std::string &name);
  bool addPublicChannel();
  bool addPrivateChannel(const std::string &name, const uint8_t secret[16], bool created);
  bool createPrivateChannel(const std::string &name);
  void removeChannel(int idx);
  void renameChannel(int idx, const std::string &name);
  RChannel *channelByIdx(int idx);
  static ChanKind classify(const std::string &name, const uint8_t secret[16]);

  // contacts
  void removeContact(const std::string &key);
  void resetPath(const std::string &key);
  void shareContact(const std::string &key);
  void setFavorite(const std::string &key, bool fav);
  void login(const std::string &key, const std::string &password);
  void requestStatus(const std::string &key);
  void requestNeighbours(const std::string &key);           // logged in (guest is enough)
  void sendCli(const std::string &key, const std::string &cmd);  // logged in as admin
  bool discoverPath(const std::string &key);
  PathDiscovery pathDisc;
  // trace the route to a repeater (along its stored route, then mirrored back to you)
  bool traceRoute(const std::string &key);
  // your radio's counters (three small requests); results land in `stats`
  void requestStats();
  // ask every repeater/sensor in direct range to answer; results arrive in `discovery` for 20 s
  bool discover();
  // the route your radio has saved to a contact, as repeater hashes from you outwards (empty = none)
  std::vector<std::string> savedRoute(const std::string &key);
  // A repeater your radio hears first-hand: it answered "Who can hear me?" lately, its own advert came
  // straight in, or packets reached your radio straight from it (a flooded packet's last hop).
  bool heardDirectly(const std::string &key);
  int64_t lastFirstHand(const std::string &key);  // when it was last heard first-hand (0 = never, as far as we know)
  void forgetFirstHand() {
    for (auto &t : lastHopAt_) t = 0;
    firstHand_.clear();
  }
  TraceRun trace;
  void refreshContacts();

  // my radio
  void setName(const std::string &name);
  void setRadio(double freqMHz, double bwKHz, int sf, int cr);
  void setTxPower(int dbm);
  void setLocation(bool share, double lat, double lon);
  void setAutoAdd(bool autoAdd);
  void setPin(uint32_t pin);
  void reboot();

 private:
  enum Kind : uint8_t {
    K_QUERY, K_APPSTART, K_TIME, K_CONTACTS, K_CHANNEL, K_SYNC, K_SEND_CH, K_SEND_DM, K_ADVERT, K_BATT,
    K_SIMPLE, K_SET_CHANNEL, K_LOGIN, K_STATUS, K_TRACE, K_STATS, K_DISCOVER, K_NEIGH, K_CLI, K_PATHDISC
  };
  struct Cmd {
    std::vector<uint8_t> bytes;
    Kind kind;
    int arg = 0;             // channel index
    std::string convId;      // for sends
    size_t msgIndex = 0;     // for sends
    int64_t sentAt = 0;
    std::string okMsg, errMsg;  // toast text
    std::string key;            // contact key for login/status
    std::function<void()> onOk;
  };
  std::deque<Cmd> outbox_;
  bool inFlight_ = false;
  int64_t lastBatt_ = 0, lastSync_ = 0;
  std::vector<RContact> pendingContacts_;

  void queue(Cmd c);
  void simple(std::vector<uint8_t> bytes, const std::string &ok, const std::string &err, std::function<void()> onOk = nullptr);
  void pump();
  void done();
  int64_t now() { return nowMs ? nowMs() : 0; }
  Conv &channelConv(int idx, const std::string &name);
  Conv &dmConv(const std::string &keyPrefix12);
  void addIncoming(Conv &c, RMsg m);
  void sortConvs();
  void parseContact(const uint8_t *d, size_t n, RContact &c);
  void rxLog(const uint8_t *d, size_t n);
  // copies of other people's messages logged before the message itself is fetched from the radio
  struct Heard {
    bool dm = false;
    int chIdx = 0;          // channel copies: slot, sender's timestamp and "Name: text"
    uint32_t ts = 0;
    std::string text;
    uint8_t src = 0;        // direct-message copies: sender's 1-byte hash and packet id
    uint32_t pkt = 0;
    RxPath path;
  };
  std::deque<Heard> heardBuf_;
  static bool addPath(RMsg &m, const RxPath &p);
  void claimChannelCopies(int idx, RMsg &m, const std::string &full);
  void claimDmCopies(uint8_t src, RMsg &m);
  void setChannelSlot(int idx, const std::string &name, const uint8_t secret[16], const std::string &okMsg);
  void sendTrace(const TraceTry &way);
  void traceHeard(const uint8_t *path, int hops, int hsz, const uint8_t *pay, size_t payLen, float snr);
  void traceFinished();
  void describeHeard(uint8_t ptype, bool flood, const uint8_t *path, int hops, int hsz, const uint8_t *pay, size_t payLen, float snr, int rssi);
  int64_t lastHopAt_[256] = {0};                // when a repeater with this first key byte last passed a packet straight to us
  std::map<std::string, int64_t> firstHand_;  // 12-hex key prefix -> when its own advert last came straight in
  void say(const std::string &m) {
    if (log) log(m);
  }
  void toast(bool ok, const std::string &t) {
    if (onResult && !t.empty()) onResult(ok, t);
  }
  static void putKey(std::vector<uint8_t> &v, const std::string &hex);
};

std::string toHex(const uint8_t *d, size_t n);
extern const uint8_t PUBLIC_CHANNEL_KEY[16];

}  // namespace st
