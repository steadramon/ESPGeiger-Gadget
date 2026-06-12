/*
  FastMillis.cpp - 1 kHz Ticker millis counter

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

#include "FastMillis.h"

#include <Arduino.h>
#include <Ticker.h>

namespace Util {

volatile uint32_t _fast_ms_counter = 0;

namespace {
Ticker   s_ticker;
uint32_t s_last_us  = 0;
uint32_t s_accum_us = 0;

// us delta absorbs Ticker slip under FreeRTOS scheduling.
void tick_cb() {
  const uint32_t now_us = micros();
  s_accum_us += (now_us - s_last_us);
  while (s_accum_us >= 1000) {
    _fast_ms_counter++;
    s_accum_us -= 1000;
  }
  s_last_us = now_us;
}
}  // anonymous namespace

void fast_millis_begin() {
  s_last_us = micros();
  _fast_ms_counter = s_last_us / 1000;
  s_ticker.attach_ms(1, tick_cb);
}

}  // namespace Util
