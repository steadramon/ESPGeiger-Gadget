/*
  Theme.cpp - brand colours + fonts

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

#include "Theme.h"
#include "../board.h"

namespace Theme {

const lv_font_t * f_title = nullptr;
const lv_font_t * f_sub   = nullptr;
const lv_font_t * f_body  = nullptr;
const lv_font_t * f_muted = nullptr;

namespace {
// UNSCII is ASCII-only; LV_SYMBOL_* PUA glyphs come from Montserrat via
// .fallback. Bundled UNSCII is const so keep a writable copy.
lv_font_t s_unscii16_with_fallback;
bool      s_unscii16_inited = false;

void ensure_retro_font() {
  if (s_unscii16_inited) return;
  s_unscii16_with_fallback          = lv_font_unscii_16;
  s_unscii16_with_fallback.fallback = &lv_font_montserrat_16;
  s_unscii16_inited                 = true;
}
}  // anonymous namespace

uint32_t kBg           = 0x1a1d20;
uint32_t kAccent       = 0x0066cc;
uint32_t kFg           = 0xdee2e6;
uint32_t kMuted        = 0xadb5bd;
uint32_t kOk           = 0x55aa55;
uint32_t kAccentYellow = 0xFFBB22;
uint32_t kRowBg            = 0x2b3035;
uint32_t kRowBgPressed     = 0x373b3e;
uint32_t kTitleColor       = 0xdee2e6;
bool     kUppercaseTitles  = false;
int      kRadiusBtn        = 6;

void init() {
  apply(Mode::Default);
}

void apply(Mode m) {
  switch (m) {
    case Mode::Retro:
      kBg              = 0x001000;
      kFg              = 0x33ff66;
      kAccent          = 0x66ff99;
      kMuted           = 0x117733;
      kOk              = 0x88ff88;
      kAccentYellow    = 0xffaa00;
      kRowBg           = 0x003311;
      kRowBgPressed    = 0x004422;
      kTitleColor      = 0x33ff66;
      kUppercaseTitles = false;
      kRadiusBtn       = 10;
      f_title = &lv_font_montserrat_28;
      f_sub   = &lv_font_montserrat_24;
      f_body  = &lv_font_montserrat_16;
      f_muted = &lv_font_montserrat_14;
      break;
    case Mode::Gadget:
      kBg              = 0x1a1612;
      kFg              = 0xe8e2d4;
      kAccent          = 0xFBD000;   // atomic yellow
      kMuted           = 0x8b7d6b;   // aged khaki
      kOk              = 0x9aa055;   // olive drab
      kAccentYellow    = 0xFBD000;
      kRowBg           = 0x2a2520;
      kRowBgPressed    = 0x3a342d;
      kTitleColor      = 0xFBD000;
      kUppercaseTitles = true;
      kRadiusBtn       = 0;
      f_title = &lv_font_montserrat_28;
      f_sub   = &lv_font_montserrat_24;
      f_body  = &lv_font_montserrat_16;
      f_muted = &lv_font_montserrat_14;
      break;
    case Mode::Default:
    default:
      kBg              = 0x1a1d20;
      kAccent          = 0x0066cc;
      kFg              = 0xdee2e6;
      kMuted           = 0xadb5bd;
      kOk              = 0x55aa55;
      kAccentYellow    = 0xFFBB22;
      kRowBg           = 0x2b3035;
      kRowBgPressed    = 0x373b3e;
      kTitleColor      = 0xdee2e6;
      kUppercaseTitles = false;
      kRadiusBtn       = 6;
      f_title = &lv_font_montserrat_28;
      f_sub   = &lv_font_montserrat_24;
      f_body  = &lv_font_montserrat_16;
      f_muted = &lv_font_montserrat_14;
      break;
  }
}

const char * mode_name(Mode m) {
  switch (m) {
    case Mode::Retro:  return "retro";
    case Mode::Gadget: return "gadget";
    default:           return "default";
  }
}

}
