/*
  FastMillis.h - 1 kHz Ticker millis counter

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

// 1 kHz Ticker-driven millis() replacement; same wrap as millis().
// Lifted from the ESPGeiger parent firmware.

#pragma once

#include <stdint.h>

namespace Util {

extern volatile uint32_t _fast_ms_counter;

static inline uint32_t fast_millis() { return _fast_ms_counter; }

void fast_millis_begin();

}  // namespace Util

static inline uint32_t fast_millis() { return Util::_fast_ms_counter; }
