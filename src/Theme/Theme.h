/*
  Theme.h - brand colours + fonts

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

#pragma once

#include <lvgl.h>
#include <stdint.h>

// Default palette mirrors the parent firmware's WebPortal STYLE_CSS.

namespace Theme {

enum class Mode : uint8_t {
  Default = 0,
  Retro   = 1,
  Gadget  = 2,   // atomic-warning palette, sharp radii
};

extern uint32_t kBg;
extern uint32_t kAccent;
extern uint32_t kFg;
extern uint32_t kMuted;
extern uint32_t kOk;
extern uint32_t kAccentYellow;
extern uint32_t kRowBg;
extern uint32_t kRowBgPressed;
extern uint32_t kTitleColor;
extern bool     kUppercaseTitles;
extern int      kRadiusBtn;

extern const lv_font_t * f_title;
extern const lv_font_t * f_sub;
extern const lv_font_t * f_body;
extern const lv_font_t * f_muted;

void init();
void apply(Mode m);

const char * mode_name(Mode m);

}
