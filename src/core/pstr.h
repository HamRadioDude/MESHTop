// PStr: a small immutable-ish string whose characters live in external PSRAM on the panel.
// The node directory holds thousands of keys and names; keeping them out of internal RAM leaves that
// for Wi-Fi / Bluetooth / TLS, which must use internal memory on this build.
// It behaves like std::string for the operations the app uses (==, +, c_str, substr, compare...).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace st {

void *extMalloc(size_t n);  // PSRAM on the device, plain malloc on the desktop simulator
void extFree(void *p);

class PStr {
 public:
  static constexpr size_t npos = std::string::npos;
  PStr() {}
  PStr(const std::string &s) { assign(s.data(), s.size()); }
  PStr(const char *s) { assign(s, s ? strlen(s) : 0); }
  PStr(const PStr &o) { assign(o.p_, o.n_); }
  PStr(PStr &&o) noexcept : p_(o.p_), n_(o.n_) {
    o.p_ = nullptr;
    o.n_ = 0;
  }
  ~PStr() { release(); }
  PStr &operator=(const PStr &o) {
    if (this != &o) assign(o.p_, o.n_);
    return *this;
  }
  PStr &operator=(PStr &&o) noexcept {
    if (this != &o) {
      release();
      p_ = o.p_;
      n_ = o.n_;
      o.p_ = nullptr;
      o.n_ = 0;
    }
    return *this;
  }
  PStr &operator=(const std::string &s) {
    assign(s.data(), s.size());
    return *this;
  }
  PStr &operator=(const char *s) {
    assign(s, s ? strlen(s) : 0);
    return *this;
  }

  const char *c_str() const { return p_ ? p_ : ""; }
  const char *data() const { return c_str(); }
  size_t size() const { return n_; }
  size_t length() const { return n_; }
  bool empty() const { return n_ == 0; }
  char operator[](size_t i) const { return p_[i]; }
  const char *begin() const { return c_str(); }
  const char *end() const { return c_str() + n_; }
  std::string str() const { return std::string(c_str(), n_); }
  operator std::string() const { return str(); }
  std::string substr(size_t pos, size_t len = npos) const {
    if (pos >= n_) return std::string();
    return std::string(c_str() + pos, std::min(len, n_ - pos));
  }
  int compare(size_t pos, size_t len, const std::string &s) const {
    if (pos > n_) pos = n_;
    size_t l = std::min(len, n_ - pos);
    int r = memcmp(c_str() + pos, s.data(), std::min(l, s.size()));
    if (r) return r;
    return l < s.size() ? -1 : l > s.size() ? 1 : 0;
  }
  size_t find(const std::string &s, size_t pos = 0) const { return str().find(s, pos); }
  size_t rfind(const std::string &s, size_t pos = npos) const { return str().rfind(s, pos); }

  friend bool operator==(const PStr &a, const PStr &b) { return a.n_ == b.n_ && !memcmp(a.c_str(), b.c_str(), a.n_); }
  friend bool operator==(const PStr &a, const std::string &b) { return a.n_ == b.size() && !memcmp(a.c_str(), b.data(), a.n_); }
  friend bool operator==(const std::string &b, const PStr &a) { return a == b; }
  friend bool operator==(const PStr &a, const char *b) { return a == std::string(b); }
  friend bool operator!=(const PStr &a, const PStr &b) { return !(a == b); }
  friend bool operator!=(const PStr &a, const std::string &b) { return !(a == b); }
  friend bool operator!=(const std::string &b, const PStr &a) { return !(a == b); }
  friend bool operator<(const PStr &a, const PStr &b) { return strcmp(a.c_str(), b.c_str()) < 0; }
  friend std::string operator+(const std::string &a, const PStr &b) { return a + b.str(); }
  friend std::string operator+(const PStr &a, const std::string &b) { return a.str() + b; }
  friend std::string operator+(const char *a, const PStr &b) { return std::string(a) + b.str(); }
  friend std::string operator+(const PStr &a, const char *b) { return a.str() + b; }

 private:
  char *p_ = nullptr;
  size_t n_ = 0;
  void release() {
    if (p_) extFree(p_);
    p_ = nullptr;
    n_ = 0;
  }
  void assign(const char *s, size_t n) {
    char *np = nullptr;
    if (n) {
      np = (char *)extMalloc(n + 1);
      memcpy(np, s, n);
      np[n] = 0;
    }
    release();
    p_ = np;
    n_ = np ? n : 0;
  }
};

// STL allocator in external RAM (for big containers whose nodes would otherwise be many small internal blocks)
template <class T>
struct ExtAlloc {
  using value_type = T;
  ExtAlloc() noexcept {}
  template <class U>
  ExtAlloc(const ExtAlloc<U> &) noexcept {}
  T *allocate(size_t n) {
    void *p = extMalloc(n * sizeof(T));
    if (!p) abort();
    return (T *)p;
  }
  void deallocate(T *p, size_t) noexcept { extFree(p); }
  template <class U>
  bool operator==(const ExtAlloc<U> &) const noexcept {
    return true;
  }
  template <class U>
  bool operator!=(const ExtAlloc<U> &) const noexcept {
    return false;
  }
};

// 64-bit id from the first 16 hex characters of a public key (collisions are practically impossible)
inline uint64_t keyId(const char *hex, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < 16 && i < n; i++) {
    char c = hex[i];
    v = v << 4 | (uint64_t)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
  }
  return v ^ (uint64_t)n << 60;
}

}  // namespace st
