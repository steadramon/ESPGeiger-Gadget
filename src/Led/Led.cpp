/*
  Led.cpp - RGB status LED

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

#include "Led.h"

#include <Arduino.h>

#include "../Settings/Settings.h"
#include "../Stations/Stations.h"
#include "../Util/FastMillis.h"

namespace Led {

namespace {
constexpr uint32_t kStaleMs = 60000;
// CYD has no series resistor so 100% duty is blinding. Active-low: 255 off.
constexpr uint8_t  kOnDuty  = 240;
constexpr uint8_t  kOffDuty = 255;
bool s_pwm_engaged[3] = { false, false, false };
}  // anonymous namespace

void init() {
  // digitalWrite only at boot. First analogWrite attaches LEDC, which
  // flashes the LED and clicks the speaker through the shared rail.
  pinMode(kPinR, OUTPUT); digitalWrite(kPinR, HIGH);
  pinMode(kPinG, OUTPUT); digitalWrite(kPinG, HIGH);
  pinMode(kPinB, OUTPUT); digitalWrite(kPinB, HIGH);
}

void off() {
  // arduino-esp32 v3: digitalWrite can't override an LEDC-attached pin.
  if (s_pwm_engaged[0]) analogWrite(kPinR, kOffDuty); else digitalWrite(kPinR, HIGH);
  if (s_pwm_engaged[1]) analogWrite(kPinG, kOffDuty); else digitalWrite(kPinG, HIGH);
  if (s_pwm_engaged[2]) analogWrite(kPinB, kOffDuty); else digitalWrite(kPinB, HIGH);
}

void set(bool r, bool g, bool b) {
  if (r) { analogWrite(kPinR, kOnDuty); s_pwm_engaged[0] = true; }
  else   { if (s_pwm_engaged[0]) analogWrite(kPinR, kOffDuty); else digitalWrite(kPinR, HIGH); }
  if (g) { analogWrite(kPinG, kOnDuty); s_pwm_engaged[1] = true; }
  else   { if (s_pwm_engaged[1]) analogWrite(kPinG, kOffDuty); else digitalWrite(kPinG, HIGH); }
  if (b) { analogWrite(kPinB, kOnDuty); s_pwm_engaged[2] = true; }
  else   { if (s_pwm_engaged[2]) analogWrite(kPinB, kOffDuty); else digitalWrite(kPinB, HIGH); }
}

void tick() {
  if (!Settings::g_led_enabled) { off(); return; }
  if (!Stations::g_mux) return;
  if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(50)) != pdTRUE) return;

  // Priority: red > amber > green.
  bool any_red   = false;
  bool any_amber = false;
  const uint32_t now = fast_millis();
  const size_t n = Stations::g_count;
  if (n == 0) {
    any_amber = true;
  } else {
    for (size_t i = 0; i < n; i++) {
      const Stations::Station & s = Stations::g_stations[i];
      const bool faulted = (s.last_poll_ok_at_ms > 0) &&
                           !(s.flags & Stations::F_TUBE_ALIVE);
      const bool saturated = (s.flags & Stations::F_SATURATED) != 0;
      if (faulted || saturated) { any_red = true; break; }
      const bool never_heard = (s.last_poll_ok_at_ms == 0) &&
                               !(s.flags & Stations::F_UDP_ANNOUNCED);
      const bool stale = (now - s.last_seen_at_ms) > kStaleMs;
      if (never_heard || stale) any_amber = true;
    }
  }
  xSemaphoreGive(Stations::g_mux);

  if      (any_red)   set(true,  false, false);
  else if (any_amber) set(true,  true,  false);   // R+G = amber
  else                set(false, true,  false);
}

}  // namespace Led
