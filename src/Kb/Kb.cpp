/*
  Kb.cpp - lightweight on-screen keyboard

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

#include "Kb.h"

#include <string.h>

namespace Kb {

namespace {

struct KbCtx {
  lv_obj_t * textarea;
  Layout    layout;
  uint8_t   mode;
};

// Mode-switch keys. Multi-char ASCII so on_press can strcmp them apart
// from typed glyphs (which are always single-char).
constexpr const char * kModeLower = "abc";
constexpr const char * kModeUpper = "ABC";
constexpr const char * kModeNum   = "123";

const char * kIpMap[] = {
  "1", "2", "3", "\n",
  "4", "5", "6", "\n",
  "7", "8", "9", "\n",
  ".", "0", ":", "\n",
  LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};

const char * kHostMapLower[] = {
  "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
  "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
  "z", "x", "c", "v", "b", "n", "m", "-", LV_SYMBOL_BACKSPACE, "\n",
  kModeNum, LV_SYMBOL_OK, ""
};

const char * kHostMapNum[] = {
  "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
  "-", LV_SYMBOL_BACKSPACE, "\n",
  kModeLower, LV_SYMBOL_OK, ""
};

const char * kPwMapLower[] = {
  "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
  "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
  kModeUpper, "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
  kModeNum, " ", LV_SYMBOL_OK, ""
};

const char * kPwMapUpper[] = {
  "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
  "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
  kModeLower, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
  kModeNum, " ", LV_SYMBOL_OK, ""
};

const char * kPwMapNum[] = {
  "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
  "-", "/", ":", ";", "(", ")", "$", "&", "@", "\"", "\n",
  ".", ",", "?", "!", "'", "_", "*", "#", LV_SYMBOL_BACKSPACE, "\n",
  kModeLower, " ", LV_SYMBOL_OK, ""
};

void apply_map(lv_obj_t * btnm, Layout layout, uint8_t mode) {
  const char ** map = nullptr;
  switch (layout) {
    case Layout::Ip:
      map = kIpMap;
      break;
    case Layout::Hostname:
      map = (mode == 0) ? kHostMapLower : kHostMapNum;
      break;
    case Layout::Password:
      map = (mode == 0) ? kPwMapLower
          : (mode == 1) ? kPwMapUpper
                        : kPwMapNum;
      break;
  }
  if (map) lv_buttonmatrix_set_map(btnm, map);
}

void on_press(lv_event_t * e) {
  lv_obj_t * btnm = (lv_obj_t *)lv_event_get_target(e);
  KbCtx * ctx = (KbCtx *)lv_obj_get_user_data(btnm);
  if (!ctx) return;

  const uint32_t id = lv_buttonmatrix_get_selected_button(btnm);
  const char * txt = lv_buttonmatrix_get_button_text(btnm, id);
  if (!txt) return;

  if (strcmp(txt, kModeLower) == 0) { ctx->mode = 0; apply_map(btnm, ctx->layout, 0); return; }
  if (strcmp(txt, kModeUpper) == 0) { ctx->mode = 1; apply_map(btnm, ctx->layout, 1); return; }
  if (strcmp(txt, kModeNum)   == 0) { ctx->mode = 2; apply_map(btnm, ctx->layout, 2); return; }

  if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
    if (ctx->textarea) lv_textarea_delete_char(ctx->textarea);
    return;
  }
  if (strcmp(txt, LV_SYMBOL_OK) == 0) {
    lv_obj_send_event(btnm, LV_EVENT_READY, nullptr);
    return;
  }

  if (ctx->textarea) lv_textarea_add_text(ctx->textarea, txt);
}

void on_delete(lv_event_t * e) {
  lv_obj_t * btnm = (lv_obj_t *)lv_event_get_target(e);
  KbCtx * ctx = (KbCtx *)lv_obj_get_user_data(btnm);
  if (ctx) lv_free(ctx);
}

}  // anonymous namespace

lv_obj_t * create(lv_obj_t * parent, lv_obj_t * textarea, Layout layout) {
  lv_obj_t * btnm = lv_buttonmatrix_create(parent);
  KbCtx * ctx = (KbCtx *)lv_malloc(sizeof(KbCtx));
  ctx->textarea = textarea;
  ctx->layout   = layout;
  ctx->mode     = 0;
  lv_obj_set_user_data(btnm, ctx);
  apply_map(btnm, layout, 0);
  lv_obj_add_event_cb(btnm, on_press,  LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(btnm, on_delete, LV_EVENT_DELETE,        nullptr);
  return btnm;
}

}  // namespace Kb
