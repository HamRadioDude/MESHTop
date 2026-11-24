// MeshThing Bluetooth link: the panel acts as a BLE central to a MeshCore companion radio
// (Nordic UART service), exactly like the MeshCore phone app.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/companion.h"

namespace bleradio {

enum EvType : uint8_t { BE_FRAME, BE_CONNECTED, BE_DISCONNECTED, BE_STATUS };
struct Event {
  EvType type;
  std::vector<uint8_t> data;
  st::RadioState state = st::R_OFF;
  std::string text;
};

void start();
void configure(bool enabled, uint32_t pin, const std::string &nameFilter);
void forget();  // drop the remembered radio + bonds, scan again
bool poll(Event *&e);                        // UI thread
uint32_t droppedFrames();  // frames lost because the queue was full (should stay 0)
void write(const std::vector<uint8_t> &f);   // UI thread

}  // namespace bleradio
