/*
  Wifi.cpp - WiFi connect + credential persistence

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

#include "Wifi.h"

#include <Preferences.h>
#include <WiFi.h>
#include <string.h>

namespace Wifi {

ScanEntry g_scan[kScanMax];
size_t    g_scan_count = 0;

namespace {
Preferences s_prefs;

size_t prefs_get_into(const char * key, char * dst, size_t dst_size) {
  if (!dst || dst_size == 0) return 0;
  return s_prefs.getString(key, dst, dst_size);
}
}

bool load_creds(char * ssid_out, char * pass_out) {
  if (!ssid_out || !pass_out) return false;
  ssid_out[0] = '\0';
  pass_out[0] = '\0';
  s_prefs.begin("wifi", /*readOnly=*/true);
  prefs_get_into("ssid", ssid_out, kSsidLen);
  prefs_get_into("pass", pass_out, kPassLen);
  s_prefs.end();
  return ssid_out[0] != '\0';
}

void save_creds(const char * ssid, const char * pass) {
  if (!ssid || !pass) return;
  s_prefs.begin("wifi", /*readOnly=*/false);
  s_prefs.putString("ssid", ssid);
  s_prefs.putString("pass", pass);
  s_prefs.end();
}

size_t process_scan(int raw_count) {
  g_scan_count = 0;
  if (raw_count <= 0) return 0;
  for (int i = 0; i < raw_count; ++i) {
    int8_t rssi = (int8_t)WiFi.RSSI(i);
    if (rssi < kRssiFloor) continue;
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    int existing = -1;
    for (size_t j = 0; j < g_scan_count; ++j) {
      if (strcmp(g_scan[j].ssid, s.c_str()) == 0) { existing = (int)j; break; }
    }
    if (existing >= 0) {
      if (rssi > g_scan[existing].rssi) g_scan[existing].rssi = rssi;
      continue;
    }
    if (g_scan_count >= kScanMax) continue;
    ScanEntry & e = g_scan[g_scan_count++];
    strncpy(e.ssid, s.c_str(), kSsidLen - 1);
    e.ssid[kSsidLen - 1] = '\0';
    e.rssi = rssi;
    e.enc  = (uint8_t)WiFi.encryptionType(i);
  }
  for (size_t i = 1; i < g_scan_count; ++i) {
    ScanEntry cur = g_scan[i];
    int j = (int)i;
    while (j > 0 && g_scan[j - 1].rssi < cur.rssi) {
      g_scan[j] = g_scan[j - 1];
      j--;
    }
    g_scan[j] = cur;
  }
  return g_scan_count;
}

}
