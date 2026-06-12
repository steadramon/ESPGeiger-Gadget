/*
  Display.cpp - LovyanGFX panel + touch

  Copyright (C) 2026 @steadramon

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#define LGFX_USE_V1

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lvgl.h>

#include "Display.h"
#include "../Util/FastMillis.h"
#include "../Settings/Settings.h"
#include "../Theme/Theme.h"
#include "../board.h"

namespace Display {

namespace {

constexpr int PIN_TFT_CS    = 15;
constexpr int PIN_TFT_DC    = 2;
constexpr int PIN_TFT_RST   = -1;   // tied to EN
constexpr int PIN_TFT_MOSI  = 13;
constexpr int PIN_TFT_MISO  = 12;
constexpr int PIN_TFT_SCLK  = 14;
constexpr int PIN_TFT_BCKL  = 21;

constexpr int PIN_TP_CS     = 33;
constexpr int PIN_TP_MOSI   = 32;
constexpr int PIN_TP_MISO   = 39;
constexpr int PIN_TP_SCLK   = 25;
constexpr int PIN_TP_IRQ    = 36;

// 30 * 240 * 2 = 14.4 KB. 40 broke DRAM once Stations + Poller landed.
constexpr int32_t LVGL_BUF_LINES = 30;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_SPI        bus_;
#if defined(PANEL_ST7789)
  lgfx::Panel_ST7789   panel_;
#else
  lgfx::Panel_ILI9341  panel_;
#endif
  lgfx::Light_PWM      light_;
  lgfx::Touch_XPT2046  touch_;
public:
  LGFX() {
    {
      auto cfg = bus_.config();
      cfg.spi_host    = HSPI_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 27000000;   // 40 MHz marginal on the CYD flex
      cfg.freq_read   = 6000000;    // MISO flaky
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = PIN_TFT_SCLK;
      cfg.pin_mosi    = PIN_TFT_MOSI;
      cfg.pin_miso    = PIN_TFT_MISO;
      cfg.pin_dc      = PIN_TFT_DC;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs           = PIN_TFT_CS;
      cfg.pin_rst          = PIN_TFT_RST;
      cfg.pin_busy         = -1;
      cfg.panel_width      = Board::SCREEN_W;
      cfg.panel_height     = Board::SCREEN_H;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;
      // 16-bit dummy read renders solid red; 8 reads shifted but usable.
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = true;
      cfg.invert           = false;
      cfg.rgb_order        = false;       // BGR on Sunton CYD
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;
      panel_.config(cfg);
    }
    {
      auto cfg = light_.config();
      cfg.pin_bl      = PIN_TFT_BCKL;
      cfg.invert      = false;
      // 25 kHz beat against WiFi RX bursts on the shared rail; 40 kHz
      // killed the visible flicker at 30% duty.
      cfg.freq        = 40000;
      cfg.pwm_channel = 7;
      light_.config(cfg);
      panel_.setLight(&light_);
    }
    {
      auto cfg = touch_.config();
      // x_min > x_max inverts X to match the panel mounting on this CYD.
      cfg.x_min            = 3900;
      cfg.x_max            = 200;
      cfg.y_min            = 200;
      cfg.y_max            = 3900;
      cfg.pin_int          = PIN_TP_IRQ;
      cfg.bus_shared       = false;
      cfg.offset_rotation  = 0;
      cfg.spi_host         = VSPI_HOST;
      cfg.freq             = 1000000;
      cfg.pin_sclk         = PIN_TP_SCLK;
      cfg.pin_mosi         = PIN_TP_MOSI;
      cfg.pin_miso         = PIN_TP_MISO;
      cfg.pin_cs           = PIN_TP_CS;
      touch_.config(cfg);
      panel_.setTouch(&touch_);
    }
    setPanel(&panel_);
  }
};

LGFX s_lgfx;

DMA_ATTR uint8_t s_lv_buf[Board::SCREEN_W * LVGL_BUF_LINES * 2];

lv_display_t * s_lv_disp  = nullptr;
lv_indev_t   * s_lv_indev = nullptr;

// Tile-by-tile hook for /shot.
FlushObserver s_flush_observer = nullptr;

void lv_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map) {
  const int32_t w = area->x2 - area->x1 + 1;
  const int32_t h = area->y2 - area->y1 + 1;
  s_lgfx.startWrite();
  s_lgfx.setAddrWindow(area->x1, area->y1, w, h);
  // swap=true: LE -> BE for the panel.
  s_lgfx.writePixels(reinterpret_cast<uint16_t *>(px_map),
                     static_cast<int32_t>(w) * h, true);
  s_lgfx.endWrite();
  if (s_flush_observer) {
    s_flush_observer(area->x1, area->y1, area->x2, area->y2, px_map);
  }
  lv_display_flush_ready(disp);
}

int8_t s_touch_offset_x = 0;
int8_t s_touch_offset_y = 0;

void lv_touch_cb(lv_indev_t * indev, lv_indev_data_t * data) {
  uint16_t x = 0, y = 0;
  const bool pressed = s_lgfx.getTouch(&x, &y);
  data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  if (pressed) {
    int32_t cx = (int32_t)x + s_touch_offset_x;
    int32_t cy = (int32_t)y + s_touch_offset_y;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx > Board::SCREEN_W - 1) cx = Board::SCREEN_W - 1;
    if (cy > Board::SCREEN_H - 1) cy = Board::SCREEN_H - 1;
    data->point.x = (int32_t)cx;
    data->point.y = (int32_t)cy;
  }
}

}  // anonymous namespace

void init() {
  Serial.println("[disp] LovyanGFX init() begin");
  bool ok = s_lgfx.init();
  Serial.printf("[disp] init() returned %d\n", (int)ok);
  s_lgfx.setRotation(0);
  s_lgfx.fillScreen(0x0000);
  Serial.printf("[disp] panel=%s reported w=%d h=%d\n",
                panel_name(),
                (int)s_lgfx.width(), (int)s_lgfx.height());

  // Settings::load() must have run.
  bool any_nonzero = false;
  for (int i = 0; i < 8; i++) if (Settings::g_touch_cal[i]) { any_nonzero = true; break; }
  if (any_nonzero) {
    set_touch_calibration(Settings::g_touch_cal);
    set_touch_offset(Settings::g_touch_offset_x, Settings::g_touch_offset_y);
    Serial.printf("[disp] applied saved touch calibration  off %d,%d\n",
                  (int)Settings::g_touch_offset_x,
                  (int)Settings::g_touch_offset_y);
  }

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return fast_millis(); });

  s_lv_disp = lv_display_create(Board::SCREEN_W, Board::SCREEN_H);
  lv_display_set_buffers(s_lv_disp, s_lv_buf, nullptr, sizeof(s_lv_buf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(s_lv_disp, lv_flush_cb);

  s_lv_indev = lv_indev_create();
  lv_indev_set_type(s_lv_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(s_lv_indev, lv_touch_cb);
  // 10 (LVGL default) ghost-clicks during scroll on the small panel.
  lv_indev_set_scroll_limit(s_lv_indev, 6);

  Serial.printf("[disp] %s init done (%ldx%ld)\n",
                panel_name(),
                (long)Board::SCREEN_W, (long)Board::SCREEN_H);
}

void set_backlight(float pct) {
  if (pct < 0.0f) pct = 0.0f;
  if (pct > 1.0f) pct = 1.0f;
  s_lgfx.setBrightness(static_cast<uint8_t>(pct * 255.0f));
}

Panel panel() {
#if defined(PANEL_ST7789)
  return Panel::ST7789;
#else
  return Panel::ILI9341;
#endif
}

const char * panel_name() {
#if defined(PANEL_ST7789)
  return "ST7789";
#else
  return "ILI9341";
#endif
}

void calibrate_touch(uint16_t out[8], int8_t * offset_x, int8_t * offset_y) {
  s_lgfx.fillScreen(0x0000);
  s_lgfx.setTextColor(0xFFFFu, 0x0000u);
  s_lgfx.setTextSize(1);
  s_lgfx.setCursor(8, Board::SCREEN_H / 2 - 12);
  s_lgfx.print("Tap each corner dot");
  s_lgfx.calibrateTouch(out, 0xFFFFu, 0x0000u, 12);
  s_lgfx.setTouchCalibrate(out);

  // Reset running offset so the centre tap measures the new corner fit.
  s_touch_offset_x = 0;
  s_touch_offset_y = 0;

  s_lgfx.fillScreen(0x0000);
  s_lgfx.setCursor(8, Board::SCREEN_H / 2 + 24);
  s_lgfx.print("Tap the centre dot");
  const int cx = Board::SCREEN_W / 2;
  const int cy = Board::SCREEN_H / 2;
  s_lgfx.fillCircle(cx, cy, 12, 0xFFFFu);

  uint16_t tx = 0, ty = 0;
  uint32_t start = millis();
  while (!s_lgfx.getTouch(&tx, &ty)) {
    if (millis() - start > 20000) break;
    delay(10);
  }
  int32_t sum_x = 0, sum_y = 0;
  uint32_t n = 0;
  const uint32_t sample_until = millis() + 120;
  while (millis() < sample_until) {
    if (s_lgfx.getTouch(&tx, &ty)) { sum_x += tx; sum_y += ty; n++; }
    delay(5);
  }
  if (n > 0) {
    const int32_t avg_x = sum_x / (int32_t)n;
    const int32_t avg_y = sum_y / (int32_t)n;
    int32_t dx = cx - avg_x;
    int32_t dy = cy - avg_y;
    if (dx >  127) dx =  127;
    if (dx < -128) dx = -128;
    if (dy >  127) dy =  127;
    if (dy < -128) dy = -128;
    *offset_x = (int8_t)dx;
    *offset_y = (int8_t)dy;
  } else {
    *offset_x = 0;
    *offset_y = 0;
  }
  s_lgfx.fillScreen(0x0000);
}

void set_touch_calibration(const uint16_t params[8]) {
  s_lgfx.setTouchCalibrate(const_cast<uint16_t *>(params));
}

void set_touch_offset(int8_t offset_x, int8_t offset_y) {
  s_touch_offset_x = offset_x;
  s_touch_offset_y = offset_y;
}

void install_flush_observer(FlushObserver cb) { s_flush_observer = cb; }
void clear_flush_observer()                   { s_flush_observer = nullptr; }

void force_full_refresh() {
  if (!s_lv_disp) return;
  lv_obj_invalidate(lv_screen_active());
  lv_refr_now(s_lv_disp);
}

}  // namespace Display
