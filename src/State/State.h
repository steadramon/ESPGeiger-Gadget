/*
  State.h - top-level state enum

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

#include <Arduino.h>

enum class State {
  Splash,
  ConnectingSaved,
  Picker,
  Password,
  Connecting,
  Main,
  Settings,
  SettingsEdit,
  TouchCal,
  Detail,
  Dice,
  RemoteScreen,
  Screensaver,
  Info,
  SpecCard,
  Post,
};

extern volatile int   g_detail_idx;
extern volatile State g_state;

constexpr size_t kSsidMax = 33;
extern char g_pending_ssid[kSsidMax];

// Deferred. Calling enter_state from an LVGL event handler tears down
// widgets the input layer still holds.
void request_state_change(State s);
