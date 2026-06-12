/*
  Settings.cpp - NVS-backed config

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

#include "Settings.h"

#include <Preferences.h>
#include <string.h>

namespace Settings {

char     g_multicast_addr[kAddrLen] = {0};
char     g_hostname[kHostnameLen]   = {0};
uint16_t g_udp_port    = kDefaultUdpPort;
uint32_t g_poll_gap_ms = kDefaultPollGapMs;
float    g_backlight   = kDefaultBacklight;
bool     g_udp_enabled   = kDefaultUdpEnabled;
bool     g_audio_enabled = kDefaultAudioEnabled;
uint8_t  g_audio_volume  = kDefaultAudioVolume;
uint8_t  g_theme_mode    = kDefaultThemeMode;
bool     g_led_enabled   = kDefaultLedEnabled;
uint8_t  g_dice_sides    = kDefaultDiceSides;
uint32_t g_remote_interval_ms = kDefaultRemoteIntervalMs;
uint8_t  g_remote_phosphor    = kDefaultRemotePhosphor;
bool     g_radmon_watchdog    = kDefaultRadmonWatchdog;
bool     g_cpm_alert_watchdog = kDefaultCpmAlertWatchdog;
uint16_t g_screensaver_secs   = kDefaultScreensaverSecs;
uint8_t  g_max_stations       = kDefaultMaxStations;
uint16_t g_touch_cal[kTouchCalLen] = {0};
int8_t   g_touch_offset_x = 0;
int8_t   g_touch_offset_y = 0;

namespace {
Preferences s_prefs;

// (key, char*, size) overload; the (key, def) one returns an Arduino String.
void prefs_get_into(const char * key, const char * dflt, char * dst, size_t cap) {
  if (!dst || cap == 0) return;
  strncpy(dst, dflt, cap - 1);
  dst[cap - 1] = '\0';
  size_t n = s_prefs.getString(key, dst, cap);
  if (n == 0 || dst[0] == '\0') {
    strncpy(dst, dflt, cap - 1);
    dst[cap - 1] = '\0';
  }
}
}  // anonymous namespace

void load() {
  s_prefs.begin("gadget", /*readOnly=*/true);
  prefs_get_into("mcast", kDefaultMulticastAddr, g_multicast_addr, kAddrLen);
  prefs_get_into("host",  kDefaultHostname,      g_hostname,       kHostnameLen);
  g_udp_port    = s_prefs.getUShort("uport",    kDefaultUdpPort);
  g_poll_gap_ms = s_prefs.getUInt  ("pgap",     kDefaultPollGapMs);
  g_backlight   = s_prefs.getFloat ("bl",       kDefaultBacklight);
  g_udp_enabled   = s_prefs.getBool  ("udp",   kDefaultUdpEnabled);
  g_audio_enabled = s_prefs.getBool  ("audio", kDefaultAudioEnabled);
  g_audio_volume  = s_prefs.getUChar ("avol",  kDefaultAudioVolume);
  g_theme_mode    = s_prefs.getUChar ("theme", kDefaultThemeMode);
  g_led_enabled   = s_prefs.getBool  ("led",   kDefaultLedEnabled);
  g_dice_sides    = s_prefs.getUChar ("dice",  kDefaultDiceSides);
  g_remote_interval_ms = s_prefs.getUInt ("rint", kDefaultRemoteIntervalMs);
  g_remote_phosphor    = s_prefs.getUChar("rph",  kDefaultRemotePhosphor);
  g_radmon_watchdog    = s_prefs.getBool ("rmw",  kDefaultRadmonWatchdog);
  g_cpm_alert_watchdog = s_prefs.getBool ("caw",  kDefaultCpmAlertWatchdog);
  g_screensaver_secs   = s_prefs.getUShort("scsv", kDefaultScreensaverSecs);
  g_max_stations       = s_prefs.getUChar ("smax", kDefaultMaxStations);
  if (g_max_stations != 8 && g_max_stations != 16 && g_max_stations != 32)
    g_max_stations = kDefaultMaxStations;
  if (g_remote_phosphor > 3) g_remote_phosphor = 0;
  if (g_remote_interval_ms < 100)   g_remote_interval_ms = 100;
  if (g_remote_interval_ms > 5000)  g_remote_interval_ms = 5000;
  switch (g_dice_sides) {
    case 4: case 6: case 8: case 10: case 12: case 20: break;
    default: g_dice_sides = kDefaultDiceSides;
  }
  if (g_audio_volume > 100) g_audio_volume = 100;
  // Missing / wrong-size tcal leaves zeros; Display::init skips apply.
  size_t n = s_prefs.getBytesLength("tcal");
  if (n == sizeof(g_touch_cal)) {
    s_prefs.getBytes("tcal", g_touch_cal, sizeof(g_touch_cal));
  }
  g_touch_offset_x = (int8_t)s_prefs.getChar("tox", 0);
  g_touch_offset_y = (int8_t)s_prefs.getChar("toy", 0);
  s_prefs.end();

  if (g_poll_gap_ms < kPollGapMinMs) g_poll_gap_ms = kPollGapMinMs;
  if (g_poll_gap_ms > kPollGapMaxMs) g_poll_gap_ms = kPollGapMaxMs;
  if (g_backlight   < kBacklightMin) g_backlight   = kBacklightMin;
  if (g_backlight   > kBacklightMax) g_backlight   = kBacklightMax;
  if (g_udp_port    == 0)            g_udp_port    = kDefaultUdpPort;
}

void save() {
  s_prefs.begin("gadget", /*readOnly=*/false);
  s_prefs.putString("mcast", g_multicast_addr);
  s_prefs.putString("host",  g_hostname);
  s_prefs.putUShort("uport", g_udp_port);
  s_prefs.putUInt  ("pgap",  g_poll_gap_ms);
  s_prefs.putFloat ("bl",    g_backlight);
  s_prefs.putBool  ("udp",   g_udp_enabled);
  s_prefs.putBool  ("audio", g_audio_enabled);
  s_prefs.putUChar ("avol",  g_audio_volume);
  s_prefs.putUChar ("theme", g_theme_mode);
  s_prefs.putBool  ("led",   g_led_enabled);
  s_prefs.putUChar ("dice",  g_dice_sides);
  s_prefs.putUInt  ("rint",  g_remote_interval_ms);
  s_prefs.putUChar ("rph",   g_remote_phosphor);
  s_prefs.putBool  ("rmw",   g_radmon_watchdog);
  s_prefs.putBool  ("caw",   g_cpm_alert_watchdog);
  s_prefs.putUShort("scsv",  g_screensaver_secs);
  s_prefs.putUChar ("smax",  g_max_stations);
  s_prefs.putBytes ("tcal",  g_touch_cal, sizeof(g_touch_cal));
  s_prefs.putChar  ("tox",   (char)g_touch_offset_x);
  s_prefs.putChar  ("toy",   (char)g_touch_offset_y);
  s_prefs.end();
}

void factory_reset() {
  // Wipes config + WiFi creds; caller reboots.
  s_prefs.begin("gadget", /*readOnly=*/false);
  s_prefs.clear();
  s_prefs.end();
  Preferences wifi;
  wifi.begin("wifi", /*readOnly=*/false);
  wifi.clear();
  wifi.end();
}

}  // namespace Settings
