/*
  Stations.cpp - station table

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

#include "Stations.h"

#include <Arduino.h>
#include <Esp.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../MontePi/MontePi.h"
#include "../Settings/Settings.h"
#include "../Util/FastMillis.h"

namespace Stations {

constexpr float kInvSixty = 1.0f / 60.0f;

Station *          g_stations    = nullptr;
size_t             g_count       = 0;
size_t             g_max         = kMax;
SemaphoreHandle_t  g_mux         = nullptr;
std::atomic<bool>  g_list_dirty{false};

void init() {
  if (!g_mux) g_mux = xSemaphoreCreateMutex();
  g_count = 0;
  g_max   = Settings::g_max_stations;
  if (g_max > kMax) g_max = kMax;
  if (g_max < 4)    g_max = 4;
  if (!g_stations) {
    g_stations = (Station *)calloc(g_max, sizeof(Station));
    if (!g_stations) {
      Serial.printf("[stations] calloc(%u) failed, rebooting\n", (unsigned)g_max);
      delay(50);
      ESP.restart();
    }
  }
}

int find_by_host(const char * host) {
  for (size_t i = 0; i < g_count; i++) {
    if (strncmp(g_stations[i].host, host, sizeof(g_stations[i].host)) == 0) return (int)i;
  }
  return -1;
}

int find_by_chipid(const char * chipid) {
  for (size_t i = 0; i < g_count; i++) {
    if (g_stations[i].chipid[0] &&
        strncmp(g_stations[i].chipid, chipid, sizeof(g_stations[i].chipid)) == 0) return (int)i;
  }
  return -1;
}

int upsert(const char * host, IPAddress ip, uint16_t http_port) {
  const int existing = find_by_host(host);
  const uint32_t now = fast_millis();
  if (existing >= 0) {
    Station & s = g_stations[existing];
    if (s.ip != ip || s.http_port != http_port) {
      s.ip        = ip;
      s.http_port = http_port;
      g_list_dirty = true;
    }
    s.last_seen_at_ms = now;
    return existing;
  }
  if (g_count >= g_max) return -1;

  Station & s = g_stations[g_count];
  memset(&s, 0, sizeof(Station));
  // memset zeroed EGRingAvg::_window; begin() restores it.
  s.cps_60.begin(kCps60);
  s.cpm_60.begin(kCpm60);
  s.cph_24.begin(kCph24);
  // NAN -> the renderer hides the field rather than showing "0".
  s.env_temp     = NAN;
  s.env_humid    = NAN;
  s.env_pressure = NAN;
  s.hv_voltage   = NAN;
  strncpy(s.host, host, sizeof(s.host) - 1);
  s.ip               = ip;
  s.http_port        = http_port;
  s.first_seen_at_ms = now;
  s.last_seen_at_ms  = now;
  s.cps_last_tick_ms = now;
  s.cpm_last_tick_ms = now;
  s.cph_last_tick_ms = now;
  g_count++;
  g_list_dirty = true;
  return (int)(g_count - 1);
}

void add_click(Station & s, uint32_t now_ms, uint32_t count) {
  uint32_t v = (uint32_t)s.pending_clicks + count;
  if (v > UINT16_MAX) v = UINT16_MAX;
  s.pending_clicks   = (uint16_t)v;
  s.total_clicks    += count;
  s.last_click_at_ms = now_ms;
  MontePi::throw_darts(now_ms ^ s.total_clicks, count);
}

void apply_clicks_last_day(Station & s, const uint32_t * last_day, size_t n) {
  // last_day[0] is the partial current hour - skip it or the chart shows a
  // dropout at the right edge. last_day is newest-first; push reversed so
  // the ring ends up chronological.
  if (!last_day || n < 2) return;
  s.cph_24.begin(kCph24);
  const size_t available = n - 1;
  const size_t take      = (available > kCph24) ? kCph24 : available;
  for (size_t i = take; i > 0; i--) {
    s.cph_24.add((float)last_day[i] * kInvSixty);
  }
  s.cph_last_tick_ms = fast_millis();
}

void apply_json(Station & s, float cpm, float usv, int8_t rssi,
                uint32_t uptime_s, bool tube_alive, bool saturated,
                uint32_t at_ms) {
  s.cpm_now            = cpm;
  s.usv_per_hour       = usv;
  s.rssi               = rssi;
  s.uptime_s           = uptime_s;
  s.last_seen_at_ms    = at_ms;
  s.last_poll_ok_at_ms = at_ms;
  if (tube_alive) s.flags |=  F_TUBE_ALIVE; else s.flags &= ~F_TUBE_ALIVE;
  if (saturated)  s.flags |=  F_SATURATED;  else s.flags &= ~F_SATURATED;
  g_list_dirty = true;
}

void tick(uint32_t now_ms) {
  for (size_t i = 0; i < g_count; i++) {
    Station & s = g_stations[i];

    // 1 Hz: flush pending_clicks into cps_60.
    while (now_ms - s.cps_last_tick_ms >= 1000) {
      // Drain any gap-fill credit over its dt window before flushing
      // (matches parent GeigerUdpRx::secondTicker).
      if (s.drain_rate > 0.0f) {
        float step = s.drain_rate;
        if (step > s.drain_pending) step = s.drain_pending;
        s.drain_pending -= step;
        s.drain_partial += step;
        if (s.drain_pending <= 0.0f) {
          s.drain_pending = 0.0f;
          s.drain_rate    = 0.0f;
        }
        if (s.drain_partial >= 1.0f) {
          const uint32_t emit = (uint32_t)s.drain_partial;
          s.drain_partial -= (float)emit;
          uint32_t v = (uint32_t)s.pending_clicks + emit;
          if (v > UINT16_MAX) v = UINT16_MAX;
          s.pending_clicks = (uint16_t)v;
          s.total_clicks  += emit;
          s.gap_filled    += emit;
        }
      }

      // Pre-poll: raw CPS into cps_60 and bootstrap cpm_now from its
      // smoothed mean (raw pending_clicks * 60 drops to 0 on a quiet
      // second). Post-poll: cps_60 mirrors the parent's cpmf so the
      // Detail 60s chart matches the headline.
      if (is_udp_active(s) && s.last_poll_ok_at_ms == 0) {
        s.cps_60.add((float)s.pending_clicks);
        s.cpm_now = s.cps_60.get() * 60.0f;
      } else {
        s.cps_60.add(s.cpm_now * kInvSixty);
      }
      s.pending_clicks   = 0;
      s.cps_last_tick_ms += 1000;
    }

    while (now_ms - s.cpm_last_tick_ms >= 60000) {
      s.cpm_60.add(s.cpm_now);
      s.cpm_last_tick_ms += 60000;
    }

    while (now_ms - s.cph_last_tick_ms >= 3600000UL) {
      s.cph_24.add(s.cpm_60.get());
      s.cph_last_tick_ms += 3600000UL;
    }
  }
}

const char * label(const Station & s) {
  return s.fname[0] ? s.fname : s.host;
}

}  // namespace Stations
