/*
  Wifi.h - WiFi connect + credential persistence

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

#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace Wifi {

constexpr int8_t kRssiFloor = -85;
constexpr size_t kScanMax   = 12;
constexpr size_t kSsidLen   = 33;
constexpr size_t kPassLen   = 65;

struct ScanEntry {
  char    ssid[kSsidLen];
  int8_t  rssi;
  uint8_t enc;
};

extern ScanEntry g_scan[kScanMax];
extern size_t    g_scan_count;

// True if an SSID was loaded. Output buffers must be at least kSsidLen
// / kPassLen.
bool load_creds(char * ssid_out, char * pass_out);
void save_creds(const char * ssid, const char * pass);

// Filter, dedupe by strongest, sort RSSI desc. Returns kept count.
size_t process_scan(int raw_count);

}
