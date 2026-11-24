// Make mesh message text displayable with the panel's fonts (basic Latin only):
//  - MeshCore mentions "@[Name]" become "@Name"
//  - emoji and other symbols the font can't draw are dropped instead of showing as boxes
//  - common typographic characters and accented letters get plain equivalents
#pragma once
#include <cstdint>
#include <string>

namespace st {

inline std::string displayText(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  size_t i = 0, n = in.size();
  while (i < n) {
    unsigned char c = (unsigned char)in[i];
    // @[Name] -> @Name
    if (c == '@' && i + 1 < n && in[i + 1] == '[') {
      size_t close = in.find(']', i + 2);
      if (close != std::string::npos && close - i < 40) {
        out += '@';
        out += displayText(in.substr(i + 2, close - i - 2));
        i = close + 1;
        continue;
      }
    }
    if (c < 0x80) {
      out += (char)c;
      i++;
      continue;
    }
    // decode one UTF-8 code point
    uint32_t cp = 0;
    int len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    if (len == 1 || i + len > n) {
      i++;
      continue;
    }
    cp = c & (0xFF >> (len + 1));
    for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)in[i + k] & 0x3F);
    i += len;
    const char *rep = nullptr;
    switch (cp) {
      case 0x2018: case 0x2019: case 0x201B: case 0x2032: rep = "'"; break;
      case 0x201C: case 0x201D: case 0x201E: case 0x2033: rep = "\""; break;
      case 0x2013: case 0x2014: case 0x2212: rep = "-"; break;
      case 0x2026: rep = "..."; break;
      case 0x00A0: case 0x2009: case 0x202F: rep = " "; break;
      case 0x00B0: rep = " deg"; break;
      case 0x00D7: rep = "x"; break;
      case 0x2022: case 0x00B7: rep = "*"; break;
      case 0x2192: case 0x27A1: rep = "->"; break;
      case 0x2190: rep = "<-"; break;
      case 0x00DF: rep = "ss"; break;
      case 0x00E6: rep = "ae"; break;
      case 0x00C6: rep = "AE"; break;
      default: break;
    }
    if (!rep && cp >= 0xC0 && cp <= 0x17F) {
      // accented Latin letters -> base letter
      static const char *lat1 = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
      static const char *latA = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
      char b = cp < 0x100 ? lat1[cp - 0xC0] : (cp - 0x100 < 128 ? latA[cp - 0x100] : '?');
      out += b;
      continue;
    }
    if (rep) out += rep;
    // everything else (emoji, pictographs, variation selectors, joiners, CJK...) is dropped
  }
  // tidy a leading space left by a dropped emoji
  size_t s = out.find_first_not_of(' ');
  return s == std::string::npos ? std::string() : out.substr(s);
}

}  // namespace st
