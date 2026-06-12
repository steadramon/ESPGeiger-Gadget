/*
  Poller.h - HTTP /json round-robin poller

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

// HTTP /json round-robin poller. Dedicated core-0 task keeps the blocking
// HTTPClient off LVGL. Cadence comes from Settings::g_poll_gap_ms.

#pragma once

#include <stdint.h>

namespace Poller {

constexpr uint32_t kHttpTimeoutMs        = 3000;
constexpr uint32_t kPriorityIntervalMs   = 3000;
// Spacer between back-to-back GETs to the same station; backlight couples
// into concurrent transmits and the parent sends Connection: close anyway.
constexpr uint32_t kInterRequestDelayMs  = 300;

void start_task();
void set_started(bool yes);

// Pin a station to refresh every kPriorityIntervalMs on top of round-robin.
// -1 clears.
void set_priority_index(int idx);

extern volatile uint32_t g_poll_count;

// Main loop fires klaxon + modal and clears. -1 = nothing pending.
extern volatile bool    g_radmon_alarm_pending;
extern volatile int     g_radmon_alarm_idx;
extern volatile uint8_t g_radmon_alarm_down;
extern volatile uint8_t g_radmon_alarm_known;

constexpr uint32_t kOutputsIntervalMs = 60000;
constexpr uint32_t kAlarmCooldownMs   = 5UL * 60UL * 1000UL;

}  // namespace Poller
