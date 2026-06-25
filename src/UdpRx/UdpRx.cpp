/*
  UdpRx.cpp - OSC-over-multicast subscriber

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

#include "UdpRx.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>

#include "../Audio/Audio.h"
#include "../Settings/Settings.h"
#include "../State/State.h"
#include "../Stations/Stations.h"
#include "../Util/FastMillis.h"
#include "../Util/IgmpRefresh.h"

namespace UdpRx {

namespace {

// OSC is big-endian on the wire.
uint32_t read_u32_be(const uint8_t * p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

float read_f32_be(const uint8_t * p) {
  union { uint32_t u; float f; } u;
  u.u = read_u32_be(p);
  return u.f;
}

// Offset past the 4-byte-padded OSC string, -1 on overrun.
int osc_skip_string(const uint8_t * buf, int len, int off) {
  int p = off;
  while (p < len && buf[p] != 0) p++;
  if (p >= len) return -1;
  int next = (p + 4) & ~3;
  return next > len ? -1 : next;
}

}  // anonymous namespace

volatile uint32_t g_pkt_count = 0;

volatile bool g_cpm_alarm_pending = false;
volatile int  g_cpm_alarm_idx     = -1;

// Force a rebind after this long of silence on a connected socket.
constexpr uint32_t kHeartbeatMs = 120000;
volatile uint32_t s_last_pkt_at_ms = 0;

// Re-emit IGMP membership reports this often, even while packets flow, so an
// aggressive router never ages out our multicast subscription.
constexpr uint32_t kIgmpRefreshMs = 300000;

namespace {

// Per-10s log; zeroed after each summary.
volatile uint32_t s_pkt_window  = 0;
volatile uint32_t s_click_count = 0;
volatile uint32_t s_rad_count   = 0;
volatile uint32_t s_sys_count   = 0;
volatile uint32_t s_drop_chipid = 0;
volatile uint32_t s_drop_format = 0;
uint32_t          s_last_summary_ms = 0;

uint32_t          s_last_alert_alarm_ms = 0;

// Poisson-spread audio for credited clicks, mirroring parent GeigerUdpRx.
// All access from the UdpRx task, so no sync.
constexpr uint8_t kGapfillQueue   = 16;
uint32_t          s_gapfill_due_ms[kGapfillQueue] = {0};
uint8_t           s_gapfill_count   = 0;
volatile uint32_t s_gapfill_dropped = 0;

bool extract_chipid_and_msg(const uint8_t * path, int path_len,
                            char chipid_out[8], const char *& msg_out) {
  // /espg/<6-hex chipid>/<msg>\0
  static constexpr int kPrefix = 6;
  static constexpr int kChipid = 6;
  if (path_len < kPrefix + kChipid + 1) return false;
  if (memcmp(path, "/espg/", kPrefix) != 0) return false;
  for (int i = 0; i < kChipid; i++) chipid_out[i] = (char)path[kPrefix + i];
  chipid_out[kChipid] = '\0';
  if (path[kPrefix + kChipid] != '/') return false;
  msg_out = (const char *)(path + kPrefix + kChipid + 1);
  return true;
}

void process_click(int station_idx, uint32_t counter, uint32_t ts_ms,
                   uint32_t now_ms) {
  Stations::Station & s = Stations::g_stations[station_idx];

  // First click after boot: credit 1, seed anchors, no backfill.
  if (s.last_click_counter == 0) {
    Stations::add_click(s, now_ms, 1);
    s.last_click_at_ms   = now_ms;
    s.last_click_counter = counter;
    s.last_click_ts_ms   = ts_ms;
    if (g_state == State::Detail && g_detail_idx == station_idx) Audio::click();
    return;
  }

  // 4-tier gap-fill mirroring parent GeigerUdpRx::processClick.
  uint32_t credit       = 1;
  uint32_t gap_credit   = 0;
  bool     advance      = true;
  bool     queue_drain  = false;
  uint32_t drain_count  = 0;
  uint32_t drain_dt_ms  = 0;

  if (counter == s.last_click_counter) return;
  if (counter > s.last_click_counter) {
    const uint32_t gap = counter - s.last_click_counter - 1;
    if (gap == 0) {
      // sequential
    } else if (gap <= 1024) {
      credit     += gap;
      gap_credit  = gap;
    } else if (gap <= 65535) {
      const uint32_t dt = ts_ms - s.last_click_ts_ms;
      if (dt > 100 && dt < 60000) {
        // Stations::tick drains second-by-second over dt.
        queue_drain = true;
        drain_count = gap;
        drain_dt_ms = dt;
      } else {
        // dt nonsense -> skip gap-fill.
        s.resync_count++;
        Serial.printf("[udp] %s drain rejected gap=%lu dt=%lu\n",
                      s.chipid, (unsigned long)gap, (unsigned long)dt);
      }
    } else {
      s.resync_count++;
      Serial.printf("[udp] %s resync gap=%lu (counter %lu -> %lu)\n",
                    s.chipid, (unsigned long)gap,
                    (unsigned long)s.last_click_counter, (unsigned long)counter);
    }
  } else {
    // Counter went down: small + sane ts is UDP reorder, big is reboot.
    const uint32_t back = s.last_click_counter - counter;
    const bool ts_reset = (int32_t)(ts_ms - s.last_click_ts_ms) < -60000;
    if (back > 1024 || ts_reset) {
      s.resync_count++;
      Serial.printf("[udp] %s reset (counter %lu -> %lu)\n",
                    s.chipid,
                    (unsigned long)s.last_click_counter, (unsigned long)counter);
    } else {
      credit  = 0;
      advance = false;
    }
  }

  if (credit > 0) {
    Stations::add_click(s, now_ms, credit);
    s.last_click_at_ms = now_ms;
    s.gap_filled      += gap_credit;
  }
  if (queue_drain) {
    s.drain_pending += (float)drain_count;
    const float r = (float)drain_count * 1000.0f / (float)drain_dt_ms;
    if (r > s.drain_rate) s.drain_rate = r;
  }
  if (advance) {
    s.last_click_counter = counter;
    s.last_click_ts_ms   = ts_ms;
  }

  // One immediate blip; gap-fill credits get random offsets in (0, window_ms]
  // so they don't lump into a single audible bang. Window capped at 50 ms =
  // sender's UDPBLIP_CLICK_MIN_INTERVAL_MS.
  if (credit > 0 && g_state == State::Detail && g_detail_idx == station_idx) {
    Audio::click();
    if (gap_credit > 0) {
      uint32_t window_ms = ts_ms - s.last_click_ts_ms;
      if (window_ms == 0 || window_ms > 50) window_ms = 50;
      uint32_t r = (uint32_t)micros();
      for (uint32_t i = 0; i < gap_credit; i++) {
        if (s_gapfill_count >= kGapfillQueue) {
          s_gapfill_dropped += (gap_credit - i);
          break;
        }
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        s_gapfill_due_ms[s_gapfill_count++] = now_ms + (r % window_ms) + 1;
      }
    }
  }
}

void process_rad(int station_idx, float cpm, float usv, const char * state,
                 uint32_t total_clicks, uint32_t now_ms) {
  Stations::Station & s = Stations::g_stations[station_idx];
  if (!isnanf(cpm))  s.cpm_now      = cpm;
  if (!isnanf(usv))  s.usv_per_hour = usv;
  s.total_clicks    = total_clicks;
  s.last_seen_at_ms = now_ms;

  // "alert" is the parent's snooze-aware over-threshold state.
  const bool now_alert  = (state && strcmp(state, "alert") == 0);
  const bool was_alert  = (s.flags2 & Stations::F2_CPM_ALERT) != 0;
  if (now_alert) s.flags2 |=  Stations::F2_CPM_ALERT;
  else           s.flags2 &= ~Stations::F2_CPM_ALERT;

  if (now_alert && !was_alert && Settings::g_cpm_alert_watchdog &&
      (s_last_alert_alarm_ms == 0 ||
       (now_ms - s_last_alert_alarm_ms) >= kAlertCooldownMs)) {
    s_last_alert_alarm_ms = now_ms;
    g_cpm_alarm_idx       = station_idx;
    g_cpm_alarm_pending   = true;
  }

  Stations::g_list_dirty = true;
}

void process_sys(int station_idx, uint32_t uptime_s, int32_t rssi,
                 uint32_t now_ms) {
  Stations::Station & s = Stations::g_stations[station_idx];
  s.uptime_s        = uptime_s;
  s.rssi            = (int8_t)rssi;
  s.last_seen_at_ms = now_ms;
  Stations::g_list_dirty = true;
}

void process_packet(const uint8_t * buf, int len) {
  if (len <= 0 || buf[0] != '/') { s_drop_format++; return; }
  int path_end = osc_skip_string(buf, len, 0);
  if (path_end < 0) { s_drop_format++; return; }

  char chipid[8];
  const char * msg = nullptr;
  if (!extract_chipid_and_msg(buf, path_end, chipid, msg)) {
    s_drop_format++; return;
  }

  if (path_end >= len || buf[path_end] != ',') { s_drop_format++; return; }
  int tag_end = osc_skip_string(buf, len, path_end);
  if (tag_end < 0) { s_drop_format++; return; }
  const char * tag = (const char *)(buf + path_end + 1);

  if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(100)) != pdTRUE) return;
  const int idx = Stations::find_by_chipid(chipid);
  if (idx < 0) {
    s_drop_chipid++;
    xSemaphoreGive(Stations::g_mux);
    return;
  }

  const uint32_t now = fast_millis();
  g_pkt_count++;
  s_pkt_window++;
  s_last_pkt_at_ms = now;

  if (strcmp(msg, "click") == 0 && strcmp(tag, "ii") == 0) {
    if (tag_end + 8 > len) { s_drop_format++; }
    else {
      const uint32_t counter = read_u32_be(buf + tag_end);
      const uint32_t ts_ms   = read_u32_be(buf + tag_end + 4);
      process_click(idx, counter, ts_ms, now);
      s_click_count++;
    }
  } else if (strcmp(msg, "rad") == 0 && strcmp(tag, "ffsi") == 0) {
    if (tag_end + 8 > len) { s_drop_format++; }
    else {
      const float cpm = read_f32_be(buf + tag_end);
      const float usv = read_f32_be(buf + tag_end + 4);
      int state_off = tag_end + 8;
      int after_state = osc_skip_string(buf, len, state_off);
      if (after_state < 0 || after_state + 4 > len) { s_drop_format++; }
      else {
        const char * state = (const char *)(buf + state_off);
        const uint32_t total = read_u32_be(buf + after_state);
        process_rad(idx, cpm, usv, state, total, now);
        s_rad_count++;
      }
    }
  } else if (strcmp(msg, "sys") == 0 && strcmp(tag, "iiii") == 0) {
    if (tag_end + 16 > len) { s_drop_format++; }
    else {
      const uint32_t uptime = read_u32_be(buf + tag_end);
      const int32_t  rssi   = (int32_t)read_u32_be(buf + tag_end + 4);
      // lps + tick_max_us on the wire are ignored.
      process_sys(idx, uptime, rssi, now);
      s_sys_count++;
    }
  }

  xSemaphoreGive(Stations::g_mux);
}

WiFiUDP       s_udp;
bool          s_socket_open = false;
uint32_t      s_last_igmp_refresh_ms = 0;   // fast_millis() of last bind/IGMP refresh (task-only)
char          s_open_addr[Settings::kAddrLen] = {0};
uint16_t      s_open_port = 0;
bool          s_open_enabled = false;
volatile bool s_started = false;
TaskHandle_t  s_task_handle = nullptr;

void close_socket() {
  if (s_socket_open) {
    s_udp.stop();
    s_socket_open = false;
  }
  // Force settings_changed() true so the next loop iteration reopens.
  // Otherwise a WiFi-reconnect-triggered close would never recover.
  s_open_addr[0] = '\0';
  s_open_port    = 0;
  s_open_enabled = false;
}

bool open_socket() {
  close_socket();
  IPAddress group;
  if (!group.fromString(Settings::g_multicast_addr)) {
    Serial.printf("[udp] bad multicast addr '%s'\n", Settings::g_multicast_addr);
    return false;
  }
  if (!s_udp.beginMulticast(group, Settings::g_udp_port)) {
    Serial.printf("[udp] beginMulticast %s:%u failed\n",
                  Settings::g_multicast_addr, Settings::g_udp_port);
    return false;
  }
  Serial.printf("[udp] listening on %s:%u\n",
                Settings::g_multicast_addr, Settings::g_udp_port);
  strncpy(s_open_addr, Settings::g_multicast_addr, sizeof(s_open_addr) - 1);
  s_open_addr[sizeof(s_open_addr) - 1] = '\0';
  s_open_port    = Settings::g_udp_port;
  s_open_enabled = true;
  s_socket_open  = true;
  // beginMulticast() just sent a fresh join, so reset the refresh timer.
  s_last_igmp_refresh_ms = fast_millis();
  return true;
}

bool settings_changed() {
  return strncmp(s_open_addr, Settings::g_multicast_addr, sizeof(s_open_addr)) != 0 ||
         s_open_port    != Settings::g_udp_port ||
         s_open_enabled != Settings::g_udp_enabled;
}

void summarise_if_due() {
  const uint32_t now = fast_millis();
  if (now - s_last_summary_ms < 10000) return;
  s_last_summary_ms = now;
  Serial.printf("[udp] 10s totals: %lu pkt (click=%lu rad=%lu sys=%lu) "
                "dropped chipid=%lu format=%lu\n",
                (unsigned long)s_pkt_window,
                (unsigned long)s_click_count,
                (unsigned long)s_rad_count,
                (unsigned long)s_sys_count,
                (unsigned long)s_drop_chipid,
                (unsigned long)s_drop_format);
  s_pkt_window = s_click_count = s_rad_count = s_sys_count = 0;
  s_drop_chipid = s_drop_format = 0;
}

void task_body(void * /*arg*/) {
  while (!s_started || WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  s_last_summary_ms = fast_millis();

  // SDK default sleep drops ~50% of multicast.
  WiFi.setSleep(WIFI_PS_MIN_MODEM);
  Serial.println("[udp] WiFi.setSleep(WIFI_PS_MIN_MODEM)");

  bool     was_connected = true;
  uint8_t  buf[kRxBufSize];

  for (;;) {
    // IGMP membership drops on WiFi reconnect; close so the next loop
    // iteration reopens.
    const bool now_connected = (WiFi.status() == WL_CONNECTED);
    if (now_connected && !was_connected) {
      Serial.println("[udp] WiFi reconnect - rebinding multicast");
      close_socket();
    }
    was_connected = now_connected;

    // Silent socket after packets had been arriving and stations exist:
    // probably an unobserved IGMP drop, force a rebind.
    if (now_connected && s_socket_open && Settings::g_udp_enabled &&
        s_last_pkt_at_ms != 0 &&
        Stations::g_count > 0 &&
        (fast_millis() - s_last_pkt_at_ms) > kHeartbeatMs) {
      Serial.printf("[udp] heartbeat: no packets for %lums, rebinding\n",
                    (unsigned long)(fast_millis() - s_last_pkt_at_ms));
      close_socket();
      s_last_pkt_at_ms = fast_millis();
    }

    // Re-emit IGMP membership reports via lwIP directly - no socket teardown,
    // covers every group joined on the STA netif (this socket + mDNS).
    if (now_connected && s_socket_open && Settings::g_udp_enabled &&
        s_last_igmp_refresh_ms != 0 &&
        (fast_millis() - s_last_igmp_refresh_ms) > kIgmpRefreshMs) {
      IgmpRefresh::refresh();
      s_last_igmp_refresh_ms = fast_millis();
    }

    if (settings_changed()) {
      close_socket();
      if (Settings::g_udp_enabled) {
        if (!open_socket()) {
          vTaskDelay(pdMS_TO_TICKS(1000));
          continue;
        }
      } else {
        s_open_enabled = false;
      }
    }

    if (!Settings::g_udp_enabled || !s_socket_open) {
      vTaskDelay(pdMS_TO_TICKS(500));
      summarise_if_due();
      continue;
    }

    // Cap so a flood doesn't starve the task's yield.
    int drained = 0;
    while (drained < 8) {
      int len = s_udp.parsePacket();
      if (len <= 0) break;
      if (len > (int)sizeof(buf)) len = (int)sizeof(buf);
      int n = s_udp.read(buf, len);
      if (n > 0) process_packet(buf, n);
      drained++;
    }

    // Drain due gap-fill blips, swap-with-last to keep packed. Wholesale
    // flush off-Detail so stale entries don't burst on re-entry.
    if (s_gapfill_count > 0) {
      if (g_state != State::Detail) {
        s_gapfill_count = 0;
      } else {
        const uint32_t now_ms = fast_millis();
        uint8_t i = 0;
        while (i < s_gapfill_count) {
          if ((int32_t)(now_ms - s_gapfill_due_ms[i]) >= 0) {
            Audio::click();
            s_gapfill_count--;
            s_gapfill_due_ms[i] = s_gapfill_due_ms[s_gapfill_count];
          } else {
            i++;
          }
        }
      }
    }

    summarise_if_due();
    vTaskDelay(pdMS_TO_TICKS(drained ? 5 : 30));
  }
}

}  // anonymous namespace

void set_started(bool yes) { s_started = yes; }

void start_task() {
  xTaskCreatePinnedToCore(task_body, "udprx", 4096, nullptr, 1,
                          &s_task_handle, 0);
}

}  // namespace UdpRx
