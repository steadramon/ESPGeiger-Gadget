/*
  Display.h - LovyanGFX panel + touch

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

// LovyanGFX panel + XPT2046 touch for the Sunton 2432S028R. ILI9341 by
// default, ST7789 with -DPANEL_ST7789 (Rv3).

#pragma once

#include <stdint.h>

namespace Display {

enum class Panel { ILI9341, ST7789 };

void          init();
void          set_backlight(float pct);   // 0.0 .. 1.0
Panel         panel();
const char *  panel_name();

// 4 corners + a centre tap, the centre captures uniform bias from the
// fit. Caller must restore the active LVGL screen afterwards.
void          calibrate_touch(uint16_t out[8], int8_t * offset_x, int8_t * offset_y);

void          set_touch_calibration(const uint16_t params[8]);
void          set_touch_offset(int8_t offset_x, int8_t offset_y);

// /shot path. Tiles arrive RGB565, row-major, top-down.
using FlushObserver = void (*)(int x1, int y1, int x2, int y2, const uint8_t * rgb565);
void          install_flush_observer(FlushObserver cb);
void          clear_flush_observer();

void          force_full_refresh();

}  // namespace Display
