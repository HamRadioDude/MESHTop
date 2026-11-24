#pragma once

namespace board {
bool begin();            // I2C, coprocessor, touch, RGB panel, LVGL display + input
void backlight(int pct);  // 0..100
void beep(int ms);
void loop();              // ends beeps (call from the UI loop)
bool touchHeld();         // raw touch state (used at boot for the Wi-Fi reset gesture)
}  // namespace board
