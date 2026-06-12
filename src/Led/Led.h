/*
  Led.h - RGB status LED

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

// CYD on-board RGB LED, GPIO 4/16/17, common anode (active-low).
// GPIO 4 is a strapping pin; without init() red half-lights at boot.

#pragma once

#include <stdint.h>

namespace Led {

constexpr int kPinR = 4;
constexpr int kPinG = 16;
constexpr int kPinB = 17;

void init();
void off();
void set(bool r, bool g, bool b);

void tick();

}  // namespace Led
