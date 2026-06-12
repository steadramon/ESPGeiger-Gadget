/*
  Discovery.cpp - mDNS browser

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

#include "Discovery.h"

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mdns.h>
#include <string.h>

#include "../Stations/Stations.h"
#include "../Util/FastMillis.h"

namespace Discovery {

uint32_t g_last_poll_ms = 0;

namespace {

volatile bool s_started     = false;
TaskHandle_t  s_task_handle = nullptr;

uint32_t first_ipv4(const mdns_result_t * r) {
  for (const mdns_ip_addr_t * a = r->addr; a != nullptr; a = a->next) {
    if (a->addr.type == ESP_IPADDR_TYPE_V4) return a->addr.u_addr.ip4.addr;
  }
  return 0;
}

const char * txt_lookup(const mdns_result_t * r, const char * key) {
  if (!r->txt) return nullptr;
  for (size_t i = 0; i < r->txt_count; i++) {
    if (r->txt[i].key && strcmp(r->txt[i].key, key) == 0) {
      return r->txt[i].value;
    }
  }
  return nullptr;
}

// Fallback when _osc._udp doesn't publish an id TXT; parse the 6-hex
// suffix from ESPGeiger-<chipid> hostnames.
bool chipid_from_hostname(const char * host, char out[8]) {
  if (!host) return false;
  const size_t n = strlen(host);
  if (n < 7 || host[n - 7] != '-') return false;
  for (size_t i = n - 6; i < n; i++) {
    const char c = host[i];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  memcpy(out, host + n - 6, 6);
  out[6] = '\0';
  out[7] = '\0';
  return true;
}

bool process_geiger(mdns_result_t * results) {
  if (!results) return false;
  bool changed = false;
  for (mdns_result_t * r = results; r; r = r->next) {
    if (!r->hostname) continue;
    const uint32_t ipv4 = first_ipv4(r);
    if (ipv4 == 0) continue;

    // Reads and Serial.printf outside the mutex; per-entry grab/release.
    const char * txt_fname = txt_lookup(r, "fname");
    char derived[8] = {0};
    const bool have_derived = chipid_from_hostname(r->hostname, derived);

    bool        log_chipid = false;
    char        log_chipid_buf[8] = {0};

    if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(500)) != pdTRUE) continue;
    const int before = Stations::g_count;
    const int idx = Stations::upsert(r->hostname, IPAddress(ipv4), r->port);
    if (idx >= 0) {
      if ((int)Stations::g_count != before) changed = true;
      Stations::Station & s = Stations::g_stations[idx];

      if (txt_fname && txt_fname[0] &&
          strncmp(s.fname, txt_fname, sizeof(s.fname)) != 0) {
        strncpy(s.fname, txt_fname, sizeof(s.fname) - 1);
        s.fname[sizeof(s.fname) - 1] = '\0';
        Stations::g_list_dirty = true;
        changed = true;
      }
      // _osc._udp's id TXT wins over this fallback.
      if (!s.chipid[0] && have_derived) {
        memcpy(s.chipid, derived, sizeof(s.chipid));
        memcpy(log_chipid_buf, derived, sizeof(log_chipid_buf));
        log_chipid = true;
        changed = true;
      }
    }
    xSemaphoreGive(Stations::g_mux);

    if (log_chipid) {
      Serial.printf("[mdns] %s chipid<-hostname %s\n",
                    r->hostname, log_chipid_buf);
    }
  }
  return changed;
}

bool process_osc(mdns_result_t * results) {
  if (!results) return false;
  bool changed = false;
  for (mdns_result_t * r = results; r; r = r->next) {
    if (!r->hostname) continue;

    // Reads outside the mutex; per-entry grab/release inside.
    const char * id = txt_lookup(r, "id");
    const char * f  = txt_lookup(r, "fname");

    bool log_chipid = false;
    char log_chipid_buf[8] = {0};
    uint16_t log_port = 0;

    if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(500)) != pdTRUE) continue;
    const int idx = Stations::find_by_host(r->hostname);
    if (idx >= 0) {
      Stations::Station & s = Stations::g_stations[idx];
      if (!(s.flags & Stations::F_UDP_ANNOUNCED) || s.osc_port != r->port) {
        s.flags    |= Stations::F_UDP_ANNOUNCED;
        s.osc_port  = r->port;
        changed = true;
      }
      if (id && id[0] && strncmp(s.chipid, id, sizeof(s.chipid)) != 0) {
        strncpy(s.chipid, id, sizeof(s.chipid) - 1);
        s.chipid[sizeof(s.chipid) - 1] = '\0';
        memcpy(log_chipid_buf, s.chipid, sizeof(log_chipid_buf));
        log_chipid = true;
        log_port   = r->port;
        changed = true;
      }
      if (f && f[0] && !s.fname[0]) {
        strncpy(s.fname, f, sizeof(s.fname) - 1);
        s.fname[sizeof(s.fname) - 1] = '\0';
        Stations::g_list_dirty = true;
        changed = true;
      }
    }
    xSemaphoreGive(Stations::g_mux);

    if (log_chipid) {
      Serial.printf("[mdns] %s chipid=%s port=%u\n",
                    r->hostname, log_chipid_buf, (unsigned)log_port);
    }
  }
  return changed;
}

void task_body(void * /*arg*/) {
  while (!s_started || WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  uint32_t interval_ms = kIntervalMinMs;
  for (;;) {
    bool pass_changed = false;
    g_last_poll_ms = fast_millis();

    // Sync API; async doesn't deliver until search end so polling buys
    // nothing. Stragglers reappear on the next pass.
    mdns_result_t * results = nullptr;
    esp_err_t err = mdns_query_ptr("_geiger", "_tcp", 800, Stations::g_max, &results);
    int n = 0;
    for (mdns_result_t * r = results; r; r = r->next) n++;
    Serial.printf("[mdns] _geiger._tcp -> err=%d n=%d\n", (int)err, n);
    if (err == ESP_OK && process_geiger(results)) pass_changed = true;
    if (results) mdns_query_results_free(results);

    // Enrichment only; the station still lists from _geiger alone.
    mdns_result_t * osc_results = nullptr;
    esp_err_t oerr = mdns_query_ptr("_osc", "_udp", 600, Stations::g_max, &osc_results);
    int osc_n = 0;
    for (mdns_result_t * r = osc_results; r; r = r->next) osc_n++;
    Serial.printf("[mdns] _osc._udp -> err=%d n=%d\n", (int)oerr, osc_n);
    if (oerr == ESP_OK && process_osc(osc_results)) pass_changed = true;
    if (osc_results) mdns_query_results_free(osc_results);

    if (pass_changed) {
      interval_ms = kIntervalMinMs;
    } else {
      uint32_t next = interval_ms + (interval_ms / 2);
      if (next > kIntervalMaxMs) next = kIntervalMaxMs;
      interval_ms = next;
    }
    vTaskDelay(pdMS_TO_TICKS(interval_ms));
  }
}

}  // anonymous namespace

void set_started(bool yes) { s_started = yes; }

void start_task() {
  xTaskCreatePinnedToCore(task_body, "mdns_poll", 4096, nullptr, 1, &s_task_handle, 0);
}

}  // namespace Discovery
