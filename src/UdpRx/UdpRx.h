/*
  UdpRx.h - OSC-over-multicast subscriber

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

// OSC-over-multicast subscriber for the parent's UdpBlip stream:
//   /click  ,ii    counter, ts_ms
//   /rad    ,ffsi  cpm, usv, state, total_clicks
//   /sys    ,iiii  uptime_s, rssi, lps, tick_max_us

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace UdpRx {

constexpr size_t kRxBufSize = 256;

void start_task();
void set_started(bool yes);

extern volatile uint32_t g_pkt_count;

// Set on the rising healthy->alert edge from /rad. Main loop fires the
// klaxon + modal and clears; subject to kAlertCooldownMs.
extern volatile bool g_cpm_alarm_pending;
extern volatile int  g_cpm_alarm_idx;

constexpr uint32_t kAlertCooldownMs = 5UL * 60UL * 1000UL;

}  // namespace UdpRx
