/*
  Settings.h - NVS-backed config

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

// NVS-backed runtime config. Plain globals; the Settings UI is the sole
// writer. Defaults mirror the parent firmware.

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace Settings {

// Match parent's UDPBLIP_DEFAULT_GROUP / _PORT.
constexpr const char * kDefaultMulticastAddr = "239.255.86.86";
constexpr uint16_t     kDefaultUdpPort       = 57340;
constexpr uint32_t     kDefaultPollGapMs     = 3000;
constexpr float        kDefaultBacklight     = 0.7f;
constexpr bool         kDefaultUdpEnabled    = true;
constexpr bool         kDefaultAudioEnabled  = true;
constexpr uint8_t      kDefaultAudioVolume   = 40;
constexpr uint8_t      kDefaultThemeMode     = 0;
constexpr bool         kDefaultLedEnabled    = true;
constexpr uint8_t      kDefaultDiceSides     = 6;
constexpr uint32_t     kDefaultRemoteIntervalMs = 1000;
constexpr uint8_t      kDefaultRemotePhosphor   = 0;
constexpr bool         kDefaultRadmonWatchdog   = false;
constexpr bool         kDefaultCpmAlertWatchdog = false;
constexpr uint16_t     kDefaultScreensaverSecs  = 60;
constexpr uint8_t      kDefaultMaxStations      = 16;

constexpr uint32_t kPollGapMinMs   = 1000;
constexpr uint32_t kPollGapMaxMs   = 30000;
constexpr float    kBacklightMin   = 0.10f;
constexpr float    kBacklightMax   = 1.0f;

constexpr size_t kAddrLen      = 16;
constexpr size_t kHostnameLen  = 32;
constexpr const char * kDefaultHostname = "espgeiger-gadget";
constexpr size_t kTouchCalLen  = 8;

extern char     g_multicast_addr[kAddrLen];
extern char     g_hostname[kHostnameLen];
extern uint16_t g_udp_port;
extern uint32_t g_poll_gap_ms;
extern float    g_backlight;
extern bool     g_udp_enabled;
extern bool     g_audio_enabled;
extern uint8_t  g_audio_volume;
extern uint8_t  g_theme_mode;
extern bool     g_led_enabled;
extern uint8_t  g_dice_sides;
extern uint32_t g_remote_interval_ms;
extern uint8_t  g_remote_phosphor;
extern bool     g_radmon_watchdog;
extern bool     g_cpm_alert_watchdog;
extern uint16_t g_screensaver_secs;
extern uint8_t  g_max_stations;       // 8, 16 or 32; applied at boot
extern uint16_t g_touch_cal[kTouchCalLen];
extern int8_t   g_touch_offset_x;
extern int8_t   g_touch_offset_y;

void load();
void save();
void factory_reset();

}  // namespace Settings
