// Demo mode: fake nodes and traffic around the configured centre (no network needed).
#pragma once
#include "core/model.h"

namespace st {

class Demo {
 public:
  void seed(Model &m, int64_t now);
  // Generate one packet and feed it to the model. Returns ms until the next one.
  int tick(Model &m, int64_t now);
  void channels(Model &m, int64_t now);
  void history(Model &m, const std::string &channel, int64_t now);
  void myCards(Model &m, int64_t now);

 private:
  uint32_t s_ = 0x5C0FE;
  uint32_t rnd() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return s_;
  }
  double frand() { return (rnd() & 0xFFFFFF) / (double)0x1000000; }
};

}  // namespace st
