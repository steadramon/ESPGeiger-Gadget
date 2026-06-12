/*
  Discovery.h - mDNS browser

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

// mDNS browser for _geiger._tcp + _osc._udp. Core-0 task; queries are
// blocking. Writes to Stations::. Backoff 3s -> 45s.

#pragma once

#include <stdint.h>

namespace Discovery {

constexpr uint32_t kIntervalMinMs   = 3000;
constexpr uint32_t kIntervalMaxMs   = 45000;
constexpr uint32_t kDeviceStaleMs   = 45000;

extern uint32_t g_last_poll_ms;

void start_task();

// Gate the task: arduino-esp32 mDNS init isn't thread-safe with the browser.
void set_started(bool yes);

}  // namespace Discovery
