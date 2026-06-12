/*
  Poller.cpp - HTTP /json round-robin poller

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

#include "Poller.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>

#include "../Settings/Settings.h"
#include "../Stations/Stations.h"
#include "../Util/FastMillis.h"

namespace Poller {

volatile uint32_t g_poll_count = 0;
volatile bool     g_radmon_alarm_pending = false;
volatile int      g_radmon_alarm_idx     = -1;
volatile uint8_t  g_radmon_alarm_down    = 0;
volatile uint8_t  g_radmon_alarm_known   = 0;

namespace {

volatile bool s_started        = false;

// Concurrent HTTP transmits couple into the backlight as visible flashes.
constexpr size_t kWorkerCount = 1;
TaskHandle_t s_task_handles[kWorkerCount] = { nullptr };

static char s_body[kWorkerCount][768];
static char s_cbody[kWorkerCount][1024];

volatile int  s_priority_idx       = -1;
uint32_t      s_last_priority_ms   = 0;
uint32_t      s_last_rr_ms         = 0;
uint32_t      s_last_majority_alarm_ms = 0;

float parse_float(const char * body, const char * key, float dflt = 0.0f) {
  char needle[16];
  int kn = snprintf(needle, sizeof(needle), "\"%s\":", key);
  if (kn <= 0 || kn >= (int)sizeof(needle)) return dflt;
  const char * p = strstr(body, needle);
  if (!p) return dflt;
  return strtof(p + kn, nullptr);
}

int parse_int(const char * body, const char * key, int dflt = 0) {
  char needle[16];
  int kn = snprintf(needle, sizeof(needle), "\"%s\":", key);
  if (kn <= 0 || kn >= (int)sizeof(needle)) return dflt;
  const char * p = strstr(body, needle);
  if (!p) return dflt;
  return (int)strtol(p + kn, nullptr, 10);
}

uint32_t parse_uint(const char * body, const char * key, uint32_t dflt = 0) {
  char needle[16];
  int kn = snprintf(needle, sizeof(needle), "\"%s\":", key);
  if (kn <= 0 || kn >= (int)sizeof(needle)) return dflt;
  const char * p = strstr(body, needle);
  if (!p) return dflt;
  return (uint32_t)strtoul(p + kn, nullptr, 10);
}

// /clicks emits one chunk per last_day entry; strip the hex framing or
// the array parser trips on it.
static void dechunk_inplace(char * buf) {
  if (!buf[0] || buf[0] == '{') return;
  size_t in = 0, out = 0;
  for (;;) {
    size_t sz = 0;
    bool   got = false;
    while (buf[in]) {
      const char c = buf[in];
      if (c == '\r' || c == '\n') {
        while (buf[in] == '\r' || buf[in] == '\n') in++;
        break;
      }
      if      (c >= '0' && c <= '9') { sz = sz * 16 + (c - '0');      got = true; in++; }
      else if (c >= 'a' && c <= 'f') { sz = sz * 16 + 10 + (c - 'a'); got = true; in++; }
      else if (c >= 'A' && c <= 'F') { sz = sz * 16 + 10 + (c - 'A'); got = true; in++; }
      else                           { in++; }
    }
    if (!got || sz == 0) break;
    for (size_t j = 0; j < sz && buf[in]; j++) buf[out++] = buf[in++];
    while (buf[in] == '\r' || buf[in] == '\n') in++;
  }
  buf[out] = '\0';
}

// configured = "age" key non-null = station has Radmon enabled.
static bool parse_radmon(const char * body, bool * ok_out, bool * configured_out) {
  const char * p = strstr(body, "\"radmon\":{");
  if (!p) return false;
  const char * obj = p + 10;
  const char * end = strchr(obj, '}');
  if (!end) return false;
  const size_t span = (size_t)(end - obj);

  const char * okp = strstr(obj, "\"ok\":");
  if (!okp || okp >= end) return false;
  *ok_out = (strncmp(okp + 5, "true", 4) == 0);

  const char * agep = strstr(obj, "\"age\":");
  if (agep && agep < end) {
    *configured_out = (strncmp(agep + 6, "null", 4) != 0);
  } else {
    *configured_out = false;
  }
  (void)span;
  return true;
}

// Exits as soon as the outer braces balance; parent doesn't send
// Content-Length, so readBytes would block the full timeout.
static bool read_json_body(WiFiClient & stream, char * body, size_t cap) {
  size_t n = 0;
  uint32_t read_start = fast_millis();
  int  brace_depth = 0;
  bool saw_open    = false;
  while (n < cap - 1) {
    if (fast_millis() - read_start > 2500) break;
    int avail = stream.available();
    if (avail > 0) {
      int want = (int)(cap - 1 - n);
      if (want > avail) want = avail;
      int got = stream.read((uint8_t *)body + n, want);
      if (got > 0) {
        for (int i = 0; i < got; ++i) {
          char c = body[n + i];
          if (c == '{') { brace_depth++; saw_open = true; }
          else if (c == '}') brace_depth--;
        }
        n += got;
        if (saw_open && brace_depth == 0) break;
      }
    } else if (!stream.connected()) {
      break;
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  body[n] = '\0';
  return saw_open && brace_depth == 0;
}

void task_body(void * arg) {
  const size_t worker_idx = (size_t)(uintptr_t)arg;
  char * const my_body  = s_body[worker_idx];
  char * const my_cbody = s_cbody[worker_idx];

  // Reused for the task lifetime; setReuse keeps the TCP socket alive
  // across same-host GETs if the parent honours keep-alive.
  WiFiClient client;
  HTTPClient http;
  http.setReuse(true);
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);

  while (!s_started || WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    const uint32_t now      = fast_millis();
    const int      prio     = s_priority_idx;
    const bool prio_due = (prio >= 0) && (now - s_last_priority_ms >= kPriorityIntervalMs);
    const bool rr_due   = (now - s_last_rr_ms >= Settings::g_poll_gap_ms);

    if (!prio_due && !rr_due) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // Snapshot under the mutex, then drop it before the network call.
    char      host[33] = {0};
    IPAddress ip(0, 0, 0, 0);
    uint16_t  port = 0;
    bool      have_target = false;
    bool      is_priority = false;
    size_t    target_idx = 0;

    if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
      if (Stations::g_count > 0) {
        if (prio_due && prio < (int)Stations::g_count) {
          target_idx  = (size_t)prio;
          is_priority = true;
        } else {
          // LRU pick, skipping the priority slot.
          target_idx = 0;
          uint32_t oldest = UINT32_MAX;
          for (size_t i = 0; i < Stations::g_count; i++) {
            if (prio >= 0 && (int)i == prio && Stations::g_count > 1) continue;
            const uint32_t t = Stations::g_stations[i].last_poll_at_ms;
            if (t < oldest) { oldest = t; target_idx = i; }
          }
          // Stamp now so a failed poll still advances the rotation.
          Stations::g_stations[target_idx].last_poll_at_ms = now;
        }
        const Stations::Station & s = Stations::g_stations[target_idx];
        ip   = s.ip;
        port = s.http_port;
        strncpy(host, s.host, sizeof(host) - 1);
        have_target = true;
      }
      xSemaphoreGive(Stations::g_mux);
    }

    if (have_target) {
      if (is_priority) s_last_priority_ms = now;
      else             s_last_rr_ms       = now;
    }

    if (!have_target || ip == IPAddress(0,0,0,0) || port == 0) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    char url[64];
    snprintf(url, sizeof(url), "http://%u.%u.%u.%u:%u/json",
             ip[0], ip[1], ip[2], ip[3], port);

    int code = -1;
    uint32_t t0 = fast_millis();
    if (http.begin(client, url)) {
      code = http.GET();
      g_poll_count++;
    }
    uint32_t dt = fast_millis() - t0;

    if (code == 200) {
      char * const body = my_body;
      WiFiClient & stream = http.getStream();
      const bool ok = read_json_body(stream, body, sizeof(s_body[0]));
      if (ok) {
        const float    cpm        = parse_float(body, "c");
        const float    usv        = parse_float(body, "s");
        const uint32_t uptime     = parse_uint (body, "ut");
        const int      rssi       = parse_int  (body, "rssi");
        // Default tube_alive=true so legacy builds don't show as faulted.
        const bool     tube_alive = (parse_int(body, "tube", 1) != 0);
        const bool     saturated  = (parse_int(body, "sat",  0) != 0);
        const float    env_t      = parse_float(body, "t",  NAN);
        const int      env_tu     = parse_int  (body, "tu", 0);
        const float    env_h      = parse_float(body, "h",  NAN);
        const float    env_p      = parse_float(body, "p",  NAN);
        const float    hv_v       = parse_float(body, "hv", NAN);
        const bool     has_env    = !isnanf(env_t) || !isnanf(env_h) || !isnanf(env_p);
        const bool     has_hv     = !isnanf(hv_v);
        const uint32_t now_ms     = fast_millis();
        if (!isnanf(cpm) &&
            xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
          // Slot may have been reshuffled by mDNS during the GET.
          if (target_idx < Stations::g_count &&
              strncmp(Stations::g_stations[target_idx].host, host,
                      sizeof(Stations::g_stations[target_idx].host)) == 0) {
            Stations::Station & st = Stations::g_stations[target_idx];
            Stations::apply_json(st, cpm, usv, (int8_t)rssi, uptime,
                                 tube_alive, saturated, now_ms);
            if (has_env) {
              st.env_temp      = env_t;
              st.env_humid     = env_h;
              st.env_pressure  = env_p;
              st.env_temp_unit = (uint8_t)env_tu;
              st.flags        |= Stations::F_HAS_ENV;
            }
            if (has_hv) {
              st.hv_voltage = hv_v;
              st.flags     |= Stations::F_HAS_HV;
            }
          }
          xSemaphoreGive(Stations::g_mux);
          Serial.printf("[poll] %-20s c=%.1f s=%.3f rssi=%d tube=%d sat=%d%s%s (%lums)\n",
                        host, cpm, usv, rssi, (int)tube_alive, (int)saturated,
                        has_env ? " env" : "", has_hv ? " hv" : "",
                        (unsigned long)dt);
        }
      } else {
        Serial.printf("[poll] %s body parse short (%lums)\n",
                      host, (unsigned long)dt);
      }
    } else if (code == 401) {
      if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (target_idx < Stations::g_count &&
            strncmp(Stations::g_stations[target_idx].host, host,
                    sizeof(Stations::g_stations[target_idx].host)) == 0) {
          Stations::g_stations[target_idx].flags |= Stations::F_NEEDS_AUTH;
        }
        xSemaphoreGive(Stations::g_mux);
      }
      Serial.printf("[poll] %s -> 401 (%lums)\n", host, (unsigned long)dt);
    } else {
      Serial.printf("[poll] %s -> %d (%lums)\n", host, code, (unsigned long)dt);
    }
    http.end();

    // Seed cph_24 once per station; only Detail uses the 24h chart.
    bool need_clicks_fetch = false;
    if (is_priority && code == 200) {
      if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (target_idx < Stations::g_count &&
            strncmp(Stations::g_stations[target_idx].host, host,
                    sizeof(Stations::g_stations[target_idx].host)) == 0 &&
            Stations::g_stations[target_idx].last_clicks_fetch_at_ms == 0) {
          need_clicks_fetch = true;
        }
        xSemaphoreGive(Stations::g_mux);
      }
    }

    // Probe /screen.bin once so the Mirror button hides on builds without it.
    bool need_oled_probe = false;
    if (code == 200) {
      if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (target_idx < Stations::g_count &&
            strncmp(Stations::g_stations[target_idx].host, host,
                    sizeof(Stations::g_stations[target_idx].host)) == 0 &&
            !(Stations::g_stations[target_idx].flags2 & Stations::F2_OLED_PROBED)) {
          Stations::g_stations[target_idx].flags2 |= Stations::F2_OLED_PROBED;
          need_oled_probe = true;
        }
        xSemaphoreGive(Stations::g_mux);
      }
    }
    if (need_oled_probe) {
      vTaskDelay(pdMS_TO_TICKS(kInterRequestDelayMs));
      char ourl[80];
      snprintf(ourl, sizeof(ourl), "http://%u.%u.%u.%u:%u/screen.bin",
               ip[0], ip[1], ip[2], ip[3], port);
      // Shorter timeout: older firmwares 404 fast; missing route can hang.
      http.setConnectTimeout(1000);
      http.setTimeout(1000);
      int ocode = -1;
      if (http.begin(client, ourl)) ocode = http.GET();
      http.end();
      http.setConnectTimeout(kHttpTimeoutMs);
      http.setTimeout(kHttpTimeoutMs);
      Serial.printf("[probe] %s /screen.bin -> %d\n", host, ocode);
      if (ocode == 200 &&
          xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (target_idx < Stations::g_count &&
            strncmp(Stations::g_stations[target_idx].host, host,
                    sizeof(Stations::g_stations[target_idx].host)) == 0) {
          Stations::g_stations[target_idx].flags2 |= Stations::F2_HAS_OLED;
          Stations::g_list_dirty = true;
        }
        xSemaphoreGive(Stations::g_mux);
      }
    }

    if (need_clicks_fetch) {
      vTaskDelay(pdMS_TO_TICKS(kInterRequestDelayMs));
      char curl[64];
      snprintf(curl, sizeof(curl), "http://%u.%u.%u.%u:%u/clicks",
               ip[0], ip[1], ip[2], ip[3], port);
      int ccode = -1;
      uint32_t ct0 = fast_millis();
      if (http.begin(client, curl)) ccode = http.GET();
      uint32_t cdt = fast_millis() - ct0;

      if (ccode == 200) {
        char * const cbody = my_cbody;
        WiFiClient & cstream = http.getStream();
        const bool cok = read_json_body(cstream, cbody, sizeof(s_cbody[0]));
        if (cok) {
          dechunk_inplace(cbody);
          // last_day = [partial_current_hour, completed_newest_first].
          uint32_t hours[Stations::kCph24];
          size_t   hcount = 0;
          const char * p = strstr(cbody, "\"last_day\":[");
          if (p) {
            p += sizeof("\"last_day\":[") - 1;
            while (hcount < Stations::kCph24 && *p && *p != ']') {
              while (*p == ' ' || *p == ',') p++;
              if (*p == ']' || !*p) break;
              char * end = nullptr;
              const unsigned long v = strtoul(p, &end, 10);
              if (end == p) break;
              hours[hcount++] = (uint32_t)v;
              p = end;
            }
          }
          if (hcount > 0 &&
              xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (target_idx < Stations::g_count &&
                strncmp(Stations::g_stations[target_idx].host, host,
                        sizeof(Stations::g_stations[target_idx].host)) == 0) {
              Stations::apply_clicks_last_day(
                Stations::g_stations[target_idx], hours, hcount);
              Stations::g_stations[target_idx].last_clicks_fetch_at_ms = fast_millis();
            }
            xSemaphoreGive(Stations::g_mux);
            Serial.printf("[clicks] %s seeded %u hours (%lums)\n",
                          host, (unsigned)hcount, (unsigned long)cdt);
          } else {
            Serial.printf("[clicks] %s parse empty (%lums)\n",
                          host, (unsigned long)cdt);
          }
        } else {
          Serial.printf("[clicks] %s body short (%lums)\n",
                        host, (unsigned long)cdt);
        }
      } else {
        Serial.printf("[clicks] %s -> %d (%lums)\n",
                      host, ccode, (unsigned long)cdt);
      }
      http.end();
    }

    // Slow tick for the Radmon watchdog. Skip the network round-trip if
     // the watchdog is off.
    bool need_outputs_fetch = false;
    if (code == 200 && Settings::g_radmon_watchdog) {
      if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (target_idx < Stations::g_count &&
            strncmp(Stations::g_stations[target_idx].host, host,
                    sizeof(Stations::g_stations[target_idx].host)) == 0) {
          const Stations::Station & s = Stations::g_stations[target_idx];
          if (fast_millis() - s.last_outputs_fetch_at_ms >= kOutputsIntervalMs ||
              s.last_outputs_fetch_at_ms == 0) {
            need_outputs_fetch = true;
          }
        }
        xSemaphoreGive(Stations::g_mux);
      }
    }
    if (need_outputs_fetch) {
      vTaskDelay(pdMS_TO_TICKS(kInterRequestDelayMs));
      char ourl[64];
      snprintf(ourl, sizeof(ourl), "http://%u.%u.%u.%u:%u/outputs",
               ip[0], ip[1], ip[2], ip[3], port);
      int ocode = -1;
      if (http.begin(client, ourl)) ocode = http.GET();
      if (ocode == 200) {
        char * const obody = my_body;
        WiFiClient & ostream = http.getStream();
        if (read_json_body(ostream, obody, sizeof(s_body[0]))) {
          bool radmon_ok = false;
          bool radmon_configured = false;
          const bool found = parse_radmon(obody, &radmon_ok, &radmon_configured);
          if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
            const uint32_t now_ms = fast_millis();
            if (target_idx < Stations::g_count &&
                strncmp(Stations::g_stations[target_idx].host, host,
                        sizeof(Stations::g_stations[target_idx].host)) == 0) {
              Stations::Station & st = Stations::g_stations[target_idx];
              st.last_outputs_fetch_at_ms = now_ms;
              if (!found || !radmon_configured) {
                st.flags2 &= ~(Stations::F2_RADMON_KNOWN | Stations::F2_RADMON_DOWN);
              } else {
                st.flags2 |= Stations::F2_RADMON_KNOWN;
                if (radmon_ok) st.flags2 &= ~Stations::F2_RADMON_DOWN;
                else           st.flags2 |=  Stations::F2_RADMON_DOWN;
                Stations::g_list_dirty = true;
              }
            }

            // One alarm per cooldown, majority down, 2-station minimum.
            uint8_t known = 0, down = 0;
            for (size_t i = 0; i < Stations::g_count; i++) {
              const auto & sk = Stations::g_stations[i];
              if (sk.flags2 & Stations::F2_RADMON_KNOWN) {
                known++;
                if (sk.flags2 & Stations::F2_RADMON_DOWN) down++;
              }
            }
            if (Settings::g_radmon_watchdog && down >= 2 && down * 2 > known &&
                (s_last_majority_alarm_ms == 0 ||
                 now_ms - s_last_majority_alarm_ms >= kAlarmCooldownMs)) {
              s_last_majority_alarm_ms = now_ms;
              g_radmon_alarm_idx     = (int)target_idx;
              g_radmon_alarm_down    = down;
              g_radmon_alarm_known   = known;
              g_radmon_alarm_pending = true;
              Serial.printf("[radmon] majority down %u/%u -> alarm\n",
                            (unsigned)down, (unsigned)known);
            }
            // Raise the count while the cooldown is alive so the modal
            // shows the peak, not the trigger value.
            if (s_last_majority_alarm_ms != 0 &&
                now_ms - s_last_majority_alarm_ms < kAlarmCooldownMs &&
                down > g_radmon_alarm_down) {
              g_radmon_alarm_down  = down;
              g_radmon_alarm_known = known;
            }
            xSemaphoreGive(Stations::g_mux);
          }
        }
      } else {
        Serial.printf("[outputs] %s -> %d\n", host, ocode);
      }
      http.end();
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

}  // anonymous namespace

void set_started(bool yes) { s_started = yes; }

void set_priority_index(int idx) {
  s_priority_idx = idx;
  // Reset so a newly-pinned station polls on the next iteration.
  s_last_priority_ms = (idx >= 0) ? 0 : fast_millis();
}

void start_task() {
  for (size_t i = 0; i < kWorkerCount; i++) {
    char name[12];
    snprintf(name, sizeof(name), "poller%u", (unsigned)i);
    xTaskCreatePinnedToCore(task_body, name, 4096,
                            (void *)(uintptr_t)i, 1,
                            &s_task_handles[i], 0);
  }
}

}  // namespace Poller
