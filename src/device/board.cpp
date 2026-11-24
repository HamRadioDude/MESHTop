// Elecrow CrowPanel Advance 7" (ESP32-S3, 800x480, hardware V1.3 / V1.4 / V1.5)
// Display, touch, backlight and buzzer. The init sequence follows Elecrow's own V1.3+ example.
#include "board.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lvgl.h>

namespace board {

static constexpr int W = 800, H = 480;
static constexpr int I2C_SDA = 15, I2C_SCL = 16;
static constexpr uint8_t ADDR_MCU = 0x30;  // STC8H coprocessor: backlight + buzzer
static constexpr uint8_t ADDR_TOUCH = 0x5D;

static esp_lcd_panel_handle_t panel = nullptr;
static SemaphoreHandle_t vsyncSem = nullptr;
static lgfx::Touch_GT911 touch;
static bool i2cViaLgfx = false;

static bool IRAM_ATTR onVsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(vsyncSem, &woken);
  return woken == pdTRUE;
}

// LVGL renders small strips into fast internal RAM; each finished strip is copied once into the
// single PSRAM framebuffer the panel scans out. Rendering straight into PSRAM (and syncing two
// full-screen buffers there) used more PSRAM bandwidth than the panel could spare, so the scan-out
// fell behind and the picture shifted / tore, worst on the map.
static void flush(lv_display_t *disp, const lv_area_t *a, uint8_t *px) {
  esp_lcd_panel_draw_bitmap(panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
  lv_display_flush_ready(disp);
}

static void readTouch(lv_indev_t *, lv_indev_data_t *data) {
  lgfx::touch_point_t p;
  if (touch.getTouchRaw(&p, 1)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = p.x;
    data->point.y = p.y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static uint32_t tick() { return millis(); }

static bool probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// After the touch driver owns the I2C port we talk to the coprocessor through LovyanGFX's I2C
// helper so the two never fight over the bus. Everything runs on the UI thread.
static void mcu(uint8_t cmd) {
  if (i2cViaLgfx) {
    lgfx::i2c::transactionWrite(I2C_NUM_0, ADDR_MCU, &cmd, 1, 400000);
  } else {
    Wire.beginTransmission(ADDR_MCU);
    Wire.write(cmd);
    Wire.endTransmission();
  }
}

bool begin() {
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(50);
  for (int tries = 0; tries < 40; tries++) {
    if (probe(ADDR_MCU) && probe(ADDR_TOUCH)) break;
    Serial.println("[board] waiting for touch + backlight controller...");
    mcu(250);  // activate touch
    pinMode(1, OUTPUT);
    digitalWrite(1, LOW);
    delay(120);
    pinMode(1, INPUT);
    delay(100);
  }
  mcu(0);  // full brightness

  {
    auto cfg = touch.config();
    cfg.x_min = 0;
    cfg.x_max = W;
    cfg.y_min = 0;
    cfg.y_max = H;
    cfg.pin_int = -1;
    cfg.pin_rst = -1;
    cfg.bus_shared = false;
    cfg.offset_rotation = 0;
    cfg.i2c_port = I2C_NUM_0;
    cfg.pin_sda = I2C_SDA;
    cfg.pin_scl = I2C_SCL;
    cfg.freq = 400000;
    cfg.i2c_addr = ADDR_TOUCH;
    touch.config(cfg);
  }
  Wire.end();
  if (!touch.init()) Serial.println("[board] GT911 touch init failed");
  i2cViaLgfx = true;

  vsyncSem = xSemaphoreCreateBinary();
  esp_lcd_rgb_panel_config_t pc = {};
  pc.clk_src = LCD_CLK_SRC_DEFAULT;
  pc.timings.pclk_hz = 16000000;  // Elecrow's own V1.3-V1.5 setting; below ~15 MHz this panel loses lock (v2.1.32 'bouncing')
  pc.timings.h_res = W;
  pc.timings.v_res = H;
  pc.timings.hsync_pulse_width = 4;
  pc.timings.hsync_back_porch = 8;
  pc.timings.hsync_front_porch = 8;
  pc.timings.vsync_pulse_width = 4;
  pc.timings.vsync_back_porch = 8;
  pc.timings.vsync_front_porch = 8;
  pc.timings.flags.pclk_active_neg = 1;
  pc.timings.flags.pclk_idle_high = 1;
  pc.data_width = 16;
  pc.bits_per_pixel = 16;
  pc.num_fbs = 1;
  // Bounce buffers (Espressif's recommendation for RGB panels with Wi-Fi/BLE): the LCD DMA reads two
  // small buffers in internal RAM that an interrupt refills from the PSRAM framebuffer. Safe here
  // because code runs from PSRAM (XIP), so flash operations never disable the cache under the ISR.
  // 20 lines is Espressif's recommended minimum: it gives the refill interrupt ~1 ms of slack.
  // The interrupt is installed on core 1 (this task); everything that hammers PSRAM (JSON, PNG,
  // LVGL, map) also runs on core 1, where this interrupt pre-empts it, so it never waits on them.
  pc.bounce_buffer_size_px = W * 20;
  pc.dma_burst_size = 64;
  pc.hsync_gpio_num = GPIO_NUM_40;
  pc.vsync_gpio_num = GPIO_NUM_41;
  pc.de_gpio_num = GPIO_NUM_42;
  pc.pclk_gpio_num = GPIO_NUM_39;
  pc.disp_gpio_num = -1;
  const int pins[16] = {21, 47, 48, 45, 38, 9, 10, 11, 12, 13, 14, 7, 17, 18, 3, 46};
  for (int i = 0; i < 16; i++) pc.data_gpio_nums[i] = pins[i];
  pc.flags.fb_in_psram = 1;
  // bb_invalidate_cache stays off: invalidating framebuffer cache lines while LVGL is writing them
  // dropped fresh pixels (thin flashing lines through text, v2.1.29-v2.1.30).
  if (esp_lcd_new_rgb_panel(&pc, &panel) != ESP_OK) {
    Serial.println("[board] 20-line bounce buffers failed, retrying with 10");
    pc.bounce_buffer_size_px = W * 10;
    if (esp_lcd_new_rgb_panel(&pc, &panel) != ESP_OK) return false;
  }
  Serial.printf("[board] panel on core %d, pclk %u, bounce %u lines, internal free %u\n", xPortGetCoreID(),
                (unsigned)pc.timings.pclk_hz, (unsigned)(pc.bounce_buffer_size_px / W), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_vsync = onVsync;
  esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, nullptr);
  esp_lcd_panel_reset(panel);
  esp_lcd_panel_init(panel);

  lv_init();
  lv_tick_set_cb(tick);
  void *fb = nullptr;
  esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &fb);
  if (fb) memset(fb, 0, W * H * 2);
  // One 12-line strip buffer in internal RAM. flush() copies each strip into the framebuffer before
  // it returns, so LVGL never draws into a buffer that is still being sent: a second strip buffer
  // (used up to v2.1.33) gained nothing and cost 19 KB of the internal RAM Wi-Fi, Bluetooth and
  // secure connections need.
  static constexpr int LINES = 12;
  const size_t bufSize = W * LINES * 2;
  void *b1 = heap_caps_aligned_alloc(4, bufSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!b1) {
    Serial.println("[board] low internal RAM: draw buffer in PSRAM");
    b1 = heap_caps_aligned_alloc(4, bufSize, MALLOC_CAP_SPIRAM);
  }
  lv_display_t *d = lv_display_create(W, H);
  lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(d, flush);
  lv_display_set_buffers(d, b1, nullptr, bufSize, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_indev_t *in = lv_indev_create();
  lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(in, readTouch);
  return true;
}

void backlight(int pct) {
  // V1.3+: 0 = brightest ... 244 = dimmest, 245 = off
  if (pct <= 0) mcu(245);
  else mcu((uint8_t)(244 - (std::min(pct, 100) * 244) / 100));
}

static uint32_t beepUntil = 0;
void beep(int ms) {
  mcu(246);
  beepUntil = millis() + ms;
}
void loop() {
  if (beepUntil && (int32_t)(millis() - beepUntil) >= 0) {
    beepUntil = 0;
    mcu(247);
  }
}

bool touchHeld() {
  lgfx::touch_point_t p;
  return touch.getTouchRaw(&p, 1) > 0;
}

}  // namespace board
