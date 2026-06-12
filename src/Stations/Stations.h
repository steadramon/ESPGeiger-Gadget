/*
  Stations.h - station table

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

// Discovered station table. Discovery, UdpRx, Poller, Ui all touch via
// g_mux. cps_60 = 60 per-second slots, cpm_60 = 60 per-minute, cph_24 =
// 24 per-hour.

#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stddef.h>
#include <stdint.h>

#include "../Util/EGSmoothed.h"

namespace Stations {

// kMax is the compile-time upper cap; fixed-size arrays in other modules
// (screensaver tracker, diff cache, etc.) size against it. g_max is the
// runtime cap actually applied at boot from Settings::g_max_stations.
constexpr size_t kMax = 32;
extern size_t    g_max;

constexpr uint16_t kCps60 = 60;
constexpr uint16_t kCpm60 = 60;
constexpr uint16_t kCph24 = 24;

constexpr uint8_t F_UDP_ANNOUNCED = 1 << 0;
constexpr uint8_t F_NEEDS_AUTH    = 1 << 1;
constexpr uint8_t F_STALE         = 1 << 2;
constexpr uint8_t F_TUBE_ALIVE    = 1 << 3;
constexpr uint8_t F_SATURATED     = 1 << 4;
constexpr uint8_t F_HAS_ENV       = 1 << 5;
constexpr uint8_t F_HAS_HV        = 1 << 6;
constexpr uint8_t F2_OLED_PROBED  = 1 << 0;
constexpr uint8_t F2_HAS_OLED     = 1 << 1;
constexpr uint8_t F2_RADMON_KNOWN = 1 << 2;
constexpr uint8_t F2_RADMON_DOWN  = 1 << 3;
constexpr uint8_t F2_CPM_ALERT    = 1 << 4;

struct Station {
  char       host[33];
  char       fname[33];
  char       chipid[8];
  IPAddress  ip;
  uint16_t   http_port;
  uint16_t   osc_port;

  uint32_t   first_seen_at_ms;
  uint32_t   last_seen_at_ms;
  uint8_t    flags;
  uint8_t    flags2;

  float      cpm_now;
  float      usv_per_hour;
  uint32_t   uptime_s;
  int8_t     rssi;
  uint32_t   total_clicks;
  uint32_t   last_click_at_ms;
  uint32_t   last_poll_at_ms;
  uint32_t   last_poll_ok_at_ms;       // gates the FAULT badge
  uint32_t   last_click_counter;
  uint32_t   last_click_ts_ms;
  uint32_t   gap_filled;
  uint32_t   resync_count;
  uint32_t   last_clicks_fetch_at_ms;
  uint32_t   last_outputs_fetch_at_ms;

  float      env_temp;
  float      env_humid;
  float      env_pressure;
  uint8_t    env_temp_unit;            // 0=C, 1=F, 2=K
  float      hv_voltage;

  // Smoothes the 1024..65535 counter-jump tier over dt_ms.
  float      drain_pending;
  float      drain_rate;
  float      drain_partial;

  EGRingAvg<float, kCps60> cps_60;
  EGRingAvg<float, kCpm60> cpm_60;
  EGRingAvg<float, kCph24> cph_24;

  uint32_t   cps_last_tick_ms;
  uint32_t   cpm_last_tick_ms;
  uint32_t   cph_last_tick_ms;

  uint16_t   pending_clicks;
};

extern Station *          g_stations;       // allocated at init() to g_max slots
extern size_t             g_count;
extern SemaphoreHandle_t  g_mux;
extern std::atomic<bool>  g_list_dirty;

// Caller holds g_mux for all of these.

int find_by_host(const char * host);
int find_by_chipid(const char * chipid);
int upsert(const char * host, IPAddress ip, uint16_t http_port);

void add_click(Station & s, uint32_t now_ms, uint32_t count = 1);

void apply_json(Station & s, float cpm, float usv, int8_t rssi,
                uint32_t uptime_s, bool tube_alive, bool saturated,
                uint32_t at_ms);

// last_day = [partial_current_hour, completed_hours_newest_first].
void apply_clicks_last_day(Station & s, const uint32_t * last_day, size_t n);

void tick(uint32_t now_ms);

const char * label(const Station & s);

void init();

inline bool is_faulted(const Station & s) {
  return s.last_poll_ok_at_ms > 0 && !(s.flags & F_TUBE_ALIVE);
}

inline bool is_saturated(const Station & s) {
  return (s.flags & F_SATURATED) != 0;
}

// Requires at least one credited UDP click so the badge doesn't show
// for stations that just advertise.
inline bool is_udp_active(const Station & s) {
  return (s.flags & F_UDP_ANNOUNCED) && s.total_clicks > 0;
}

inline bool is_radmon_known(const Station & s) {
  return (s.flags2 & F2_RADMON_KNOWN) != 0;
}

inline bool is_radmon_down(const Station & s) {
  return (s.flags2 & F2_RADMON_DOWN) != 0;
}

inline bool is_cpm_alert(const Station & s) {
  return (s.flags2 & F2_CPM_ALERT) != 0;
}

}  // namespace Stations
