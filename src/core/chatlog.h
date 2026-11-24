// Saved chats: one line per message, tab-separated, in /scopething/chats/<radio key>.txt on the SD card.
//   v1 <c|d> <channel name> <DM key> <conversation title> <ms> <mine 0|1> <who> <hops> <snr|-> <text>
// Tabs, newlines and backslashes inside fields are escaped (\t \n \\).
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace st {

struct ChatLine {
  bool isChannel = true;
  std::string chanName, dmKey, title, who, text;
  long long ts = 0;
  bool mine = false;
  int hops = -1;
  bool hasSnr = false;
  float snr = 0;
};

inline std::string chatEsc(const std::string &s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    if (c == '\\') o += "\\\\";
    else if (c == '\t') o += "\\t";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') continue;
    else o += c;
  }
  return o;
}
inline std::string chatUnesc(const std::string &s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char n = s[++i];
      o += n == 't' ? '\t' : n == 'n' ? '\n' : n;
    } else o += s[i];
  }
  return o;
}

inline std::string chatLineEncode(const ChatLine &l) {
  char num[96];
  snprintf(num, sizeof num, "%lld\t%d", l.ts, l.mine ? 1 : 0);
  std::string snr = "-";
  if (l.hasSnr) {
    char b[16];
    snprintf(b, sizeof b, "%.1f", l.snr);
    snr = b;
  }
  return "v1\t" + std::string(l.isChannel ? "c" : "d") + "\t" + chatEsc(l.chanName) + "\t" + chatEsc(l.dmKey) + "\t" +
         chatEsc(l.title) + "\t" + num + "\t" + chatEsc(l.who) + "\t" + std::to_string(l.hops) + "\t" + snr + "\t" +
         chatEsc(l.text) + "\n";
}

inline bool chatLineDecode(const std::string &line, ChatLine &l) {
  std::vector<std::string> f;
  size_t a = 0;
  for (;;) {
    size_t b = line.find('\t', a);
    f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  if (f.size() < 11 || f[0] != "v1") return false;
  l.isChannel = f[1] == "c";
  l.chanName = chatUnesc(f[2]);
  l.dmKey = chatUnesc(f[3]);
  l.title = chatUnesc(f[4]);
  l.ts = atoll(f[5].c_str());
  l.mine = f[6] == "1";
  l.who = chatUnesc(f[7]);
  l.hops = atoi(f[8].c_str());
  l.hasSnr = f[9] != "-";
  l.snr = l.hasSnr ? (float)atof(f[9].c_str()) : 0;
  std::string t = f[10];
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
  l.text = chatUnesc(t);
  return l.ts > 0;
}

inline std::vector<ChatLine> chatParseAll(const std::string &data) {
  std::vector<ChatLine> out;
  size_t a = 0;
  while (a < data.size()) {
    size_t b = data.find('\n', a);
    std::string line = data.substr(a, b == std::string::npos ? std::string::npos : b - a);
    ChatLine l;
    if (chatLineDecode(line, l)) out.push_back(l);
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return out;
}

}  // namespace st
