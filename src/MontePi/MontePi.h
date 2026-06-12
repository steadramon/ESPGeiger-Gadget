/*
  MontePi.h - Monte Carlo Pi estimator driven by UDP click timing

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

// Each credited UDP click is one Monte Carlo dart throw. Dart (x, y) is
// xorshift'd from the click's ts_ms ^ counter so the position depends on
// real radiation timing entropy. Aggregated across the whole fleet.

#pragma once

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace MontePi {

constexpr size_t kRingLen = 256;

struct Snapshot {
  uint64_t decays;   // real credited UDP clicks
  uint64_t darts;    // statistical samples = decays * kDartsPerClick
  uint64_t inside;
  int16_t  recent_x[kRingLen];
  int16_t  recent_y[kRingLen];
  size_t   recent_head;
  size_t   recent_count;
};

void init();

// One dart per credited click. seed must vary per call (callers pass the
// station's now_ms ^ total_clicks); each dart in the batch xorshifts
// (seed + i * golden_ratio) so a single big gap-fill still produces
// statistically independent darts.
void throw_darts(uint32_t seed, uint32_t count);

void snapshot(Snapshot & out);

extern volatile uint64_t g_decays;
extern volatile uint64_t g_darts;
extern volatile uint64_t g_inside;

}  // namespace MontePi
