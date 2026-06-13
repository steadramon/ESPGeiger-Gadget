/*
  Ui.cpp - LVGL screens

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

#include "Ui.h"

#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <Version.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>

#include "../Audio/Audio.h"
#include "../Display/Display.h"
#include "../Kb/Kb.h"
#include "../Led/Led.h"
#include "../MontePi/MontePi.h"
#include "../Poller/Poller.h"
#include "../Settings/Settings.h"
#include "../Theme/Theme.h"
#include "../Stations/Stations.h"
#include "../State/State.h"
#include "../Util/FastMillis.h"
#include "../Theme/Theme.h"
#include "../Wifi/Wifi.h"
#include "../board.h"

namespace Ui {

SettingsEditTarget g_pending_settings_edit = SettingsEditTarget::Multicast;

lv_obj_t * scr_splash        = nullptr;
lv_obj_t * scr_picker        = nullptr;
lv_obj_t * scr_password      = nullptr;
lv_obj_t * scr_main          = nullptr;
lv_obj_t * scr_settings      = nullptr;
lv_obj_t * scr_settings_edit = nullptr;
lv_obj_t * scr_detail        = nullptr;

// Diff cache. Allocated to g_max on first build_main, reused across rebuilds.
static char (*last_name)[40] = nullptr;
static char (*last_cpm) [16] = nullptr;
static char (*last_meta)[48] = nullptr;

namespace {

lv_obj_t * splash_status = nullptr;

lv_obj_t * picker_status = nullptr;
lv_obj_t * picker_list   = nullptr;

lv_obj_t * password_label = nullptr;
lv_obj_t * password_input = nullptr;
lv_obj_t * password_kb    = nullptr;

void style_dark_screen(lv_obj_t * scr) {
  lv_obj_set_style_bg_color(scr, lv_color_hex(Theme::kBg), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(scr, 0, 0);
}

// Default theme blue doesn't fit the palette; restyle each part.
void style_slider(lv_obj_t * sl) {
  lv_obj_set_style_bg_color(sl, lv_color_hex(Theme::kRowBg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl, lv_color_hex(Theme::kAccent), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, lv_color_hex(Theme::kFg), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_pad_all(sl, 4, LV_PART_KNOB);
  lv_obj_set_style_border_width(sl, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(sl, 0, LV_PART_INDICATOR);
  lv_obj_set_style_border_width(sl, 0, LV_PART_KNOB);
}

void style_switch(lv_obj_t * sw) {
  lv_obj_set_style_bg_color(sw, lv_color_hex(Theme::kRowBg), LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw, lv_color_hex(Theme::kAccent),
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(sw, lv_color_hex(Theme::kFg), LV_PART_KNOB);
}

void on_picker_clicked(lv_event_t * e) {
  lv_obj_t * btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
  size_t idx = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(btn));
  if (idx >= Wifi::g_scan_count) return;
  strncpy(g_pending_ssid, Wifi::g_scan[idx].ssid, kSsidMax - 1);
  g_pending_ssid[kSsidMax - 1] = '\0';
  if (Wifi::g_scan[idx].enc == WIFI_AUTH_OPEN) {
    request_state_change(State::Connecting);
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_pending_ssid);
  } else {
    request_state_change(State::Password);
  }
}

void on_password_event(lv_event_t * e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    const char * pass = lv_textarea_get_text(password_input);
    Wifi::save_creds(g_pending_ssid, pass);
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_pending_ssid, pass);
    request_state_change(State::Connecting);
  } else if (code == LV_EVENT_CANCEL) {
    request_state_change(State::Picker);
  }
}

void on_password_back(lv_event_t *) {
  request_state_change(State::Picker);
}

void on_settings_open(lv_event_t *)  { request_state_change(State::Settings); }
// Snapshot on entry so the back-as-cancel path can revert.
struct SettingsSnap {
  uint32_t poll_gap_ms;
  float    backlight;
  bool     udp_enabled;
  bool     audio_enabled;
  uint8_t  audio_volume;
  uint8_t  theme_mode;
  bool     led_enabled;
  bool     radmon_watchdog;
  bool     cpm_alert_watchdog;
  uint16_t screensaver_secs;
  uint8_t  max_stations;
};
SettingsSnap s_settings_snap;
bool         s_settings_dirty = false;

void settings_take_snapshot() {
  s_settings_snap.poll_gap_ms      = Settings::g_poll_gap_ms;
  s_settings_snap.backlight        = Settings::g_backlight;
  s_settings_snap.udp_enabled      = Settings::g_udp_enabled;
  s_settings_snap.audio_enabled    = Settings::g_audio_enabled;
  s_settings_snap.audio_volume     = Settings::g_audio_volume;
  s_settings_snap.theme_mode       = Settings::g_theme_mode;
  s_settings_snap.led_enabled      = Settings::g_led_enabled;
  s_settings_snap.radmon_watchdog  = Settings::g_radmon_watchdog;
  s_settings_snap.cpm_alert_watchdog = Settings::g_cpm_alert_watchdog;
  s_settings_snap.screensaver_secs = Settings::g_screensaver_secs;
  s_settings_snap.max_stations     = Settings::g_max_stations;
}

void settings_revert_snapshot() {
  const uint8_t old_theme = s_settings_snap.theme_mode;
  Settings::g_poll_gap_ms      = s_settings_snap.poll_gap_ms;
  Settings::g_backlight        = s_settings_snap.backlight;
  Settings::g_udp_enabled      = s_settings_snap.udp_enabled;
  Settings::g_audio_enabled    = s_settings_snap.audio_enabled;
  Settings::g_audio_volume     = s_settings_snap.audio_volume;
  Settings::g_theme_mode       = old_theme;
  Settings::g_led_enabled      = s_settings_snap.led_enabled;
  Settings::g_radmon_watchdog  = s_settings_snap.radmon_watchdog;
  Settings::g_cpm_alert_watchdog = s_settings_snap.cpm_alert_watchdog;
  Settings::g_screensaver_secs = s_settings_snap.screensaver_secs;
  Settings::g_max_stations     = s_settings_snap.max_stations;
  Display::set_backlight(Settings::g_backlight);
  Theme::apply((Theme::Mode)old_theme);
  Led::tick();
}

void on_settings_save(lv_event_t *) {
  if (s_settings_dirty) {
    Settings::save();
    s_settings_dirty = false;
  }
  // Max-stations only takes effect at next boot.
  if (Settings::g_max_stations != Stations::g_max) {
    Serial.println("[settings] max_stations changed, rebooting");
    delay(150);
    ESP.restart();
  }
  request_state_change(State::Main);
}

// Back = cancel: drop dirty edits, revert from snapshot.
void on_settings_close(lv_event_t *) {
  if (s_settings_dirty) {
    settings_revert_snapshot();
    s_settings_dirty = false;
  }
  request_state_change(State::Main);
}
void on_multicast_tap(lv_event_t *) {
  g_pending_settings_edit = SettingsEditTarget::Multicast;
  request_state_change(State::SettingsEdit);
}
void on_hostname_tap(lv_event_t *) {
  g_pending_settings_edit = SettingsEditTarget::Hostname;
  request_state_change(State::SettingsEdit);
}
void on_detail_back(lv_event_t *)    { request_state_change(State::Main); }
void on_open_mirror(lv_event_t *)    { request_state_change(State::RemoteScreen); }

void fire_remote_action(const char * path) {
  const int idx = g_detail_idx;
  if (idx < 0) return;
  IPAddress ip(0, 0, 0, 0);
  uint16_t  port = 0;
  if (Stations::g_mux &&
      xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (idx < (int)Stations::g_count) {
      ip   = Stations::g_stations[idx].ip;
      port = Stations::g_stations[idx].http_port;
    }
    xSemaphoreGive(Stations::g_mux);
  }
  if (ip == IPAddress(0,0,0,0) || port == 0) return;
  char url[80];
  snprintf(url, sizeof(url), "http://%u.%u.%u.%u:%u%s",
           ip[0], ip[1], ip[2], ip[3], port, path);
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(1000);
  http.setTimeout(1000);
  int code = -1;
  if (http.begin(client, url)) code = http.GET();
  Serial.printf("[action] %s -> %d\n", url, code);
  http.end();
}

void on_remote_tap(lv_event_t *)     { fire_remote_action("/screen/tap"); }
void on_remote_back(lv_event_t *)    { request_state_change(State::Detail); }
void on_calibrate_tap(lv_event_t *)  { request_state_change(State::TouchCal); }

void on_theme_cycle(lv_event_t *) {
  Settings::g_theme_mode = (Settings::g_theme_mode + 1) % 3;
  Theme::apply((Theme::Mode)Settings::g_theme_mode);
  s_settings_dirty = true;
  destroy_settings();
  destroy_main();
  build_settings();
  settings_refresh();
  lv_screen_load(scr_settings);
}

void on_main_row_clicked(lv_event_t * e) {
  lv_obj_t * btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
  const size_t idx = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(btn));
  if (idx >= Stations::g_max) return;
  g_detail_idx = (int)idx;
  request_state_change(State::Detail);
}

void on_poll_slider(lv_event_t * e);
void on_bl_slider(lv_event_t * e);
void on_udp_toggle(lv_event_t * e);
void on_audio_slider(lv_event_t * e);
void on_wifi_reset(lv_event_t * e);
void on_rmw_toggle(lv_event_t * e);
void on_screensaver_cycle(lv_event_t * e);
void on_factory_reset(lv_event_t * e);
void on_hostname_tap(lv_event_t * e);
void on_max_stations_cycle(lv_event_t * e);

}  // anonymous namespace

// Reserved for the Retro theme; not implemented yet.
void update_crt_corners() {}

void build_splash() {
  scr_splash = lv_obj_create(nullptr);
  style_dark_screen(scr_splash);

  // Geometry mirrors WebPortal::FAVICON_PATHS.
  constexpr int32_t disc_size  = 96;
  constexpr int32_t rim_w      = 7;
  constexpr int32_t arc_diam   = 68;
  constexpr int32_t arc_stroke = 23;
  constexpr int32_t dot_size   = 12;

  lv_obj_t * disc = lv_obj_create(scr_splash);
  lv_obj_remove_style_all(disc);
  lv_obj_set_size(disc, disc_size, disc_size);
  lv_obj_align(disc, LV_ALIGN_CENTER, 0, -52);
  lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(disc, lv_color_hex(Theme::kAccentYellow), 0);
  lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(disc, lv_color_hex(0x666666), 0);
  lv_obj_set_style_border_width(disc, rim_w, 0);

  // Three lobes at 12 / 4 / 8 o'clock, 60deg wedges.
  struct LeafSpec { int32_t start; int32_t end; };
  const LeafSpec leaves[3] = {
    { 240, 300 },   // top
    {   0,  60 },   // bottom-right
    { 120, 180 },   // bottom-left
  };
  for (const auto & l : leaves) {
    lv_obj_t * arc = lv_arc_create(disc);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, arc_diam, arc_diam);
    lv_obj_center(arc);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_angles(arc, l.start, l.end);
    lv_obj_set_style_arc_width(arc, arc_stroke, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x555555), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
  }

  lv_obj_t * dot = lv_obj_create(disc);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, dot_size, dot_size);
  lv_obj_center(dot);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(0x555555), 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

  lv_obj_t * title = lv_label_create(scr_splash);
  lv_label_set_text(title, "ESPGeiger");
  lv_obj_set_style_text_font(title, Theme::f_title, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, 30);

  lv_obj_t * sub = lv_label_create(scr_splash);
  lv_label_set_text(sub, "Gadget");
  lv_obj_set_style_text_font(sub, Theme::f_sub, 0);
  lv_obj_set_style_text_color(sub, lv_color_hex(Theme::kAccent), 0);
  lv_obj_align(sub, LV_ALIGN_CENTER, 0, 65);

  splash_status = lv_label_create(scr_splash);
  lv_label_set_text(splash_status, "starting...");
  lv_obj_set_style_text_font(splash_status, Theme::f_muted, 0);
  lv_obj_set_style_text_color(splash_status, lv_color_hex(Theme::kMuted), 0);
  lv_obj_set_width(splash_status, Board::SCREEN_W - 8);
  lv_obj_set_style_text_align(splash_status, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(splash_status, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// Forward decl; the real definition lives near build_post / destroy_post.
namespace { extern lv_obj_t * post_status_lbl; }

void splash_set_status(const char * msg) {
  // Route to whichever boot screen is alive; scr_post (Gadget/Retro)
  // wins because it's the one actually displayed.
  if (post_status_lbl) {
    lv_label_set_text(post_status_lbl, msg);
    return;
  }
  if (splash_status) lv_label_set_text(splash_status, msg);
}

namespace {
const char * const s_funny_status[] = {
  "Geiger...ing",
  "Scanning for\nlocal superpowers...",
  "Calibrating\nspicy-rock detector...",
  "Searching for mutant turtles.",
  "Locating nearest\nlead-lined refrigerator...",
  "Quantum state:\n[X] Alive [X] Dead",
  "Self-test failed:\nUser is radiating charisma.",
  "Asking the neutrinos\nnicely...",
  "Convincing photons\nto behave...",
  "Calling Dr Oppenheimer\nfor backup...",
  "Running numbers\npast Feynman...",
  "Checking critical mass...",
  "Checking if Einstein\nis looking...",
  "Dividing by zero...\nPlease hold...",
  "Is that a banana\nor a fuel rod?",
  "Charging the flux capacitor...",
  "Verifying tinfoil hat\nalignment...",
  "Banana sensor triggered...",
  "Counting to 3.14159265... ish.",
  "Improvising.\nDon't tell anyone.",
};
constexpr size_t   kFunnyCount      = sizeof(s_funny_status) / sizeof(s_funny_status[0]);
constexpr uint32_t kFunnyIntervalMs = 1800;
uint32_t s_funny_last_ms = 0;
uint8_t  s_funny_order[kFunnyCount];
uint8_t  s_funny_pos     = kFunnyCount;   // forces a shuffle on first call

void shuffle_funny_order() {
  for (size_t i = 0; i < kFunnyCount; i++) s_funny_order[i] = (uint8_t)i;
  for (size_t i = kFunnyCount - 1; i > 0; i--) {
    const uint32_t j = esp_random() % (i + 1);
    const uint8_t  t = s_funny_order[i];
    s_funny_order[i] = s_funny_order[j];
    s_funny_order[j] = t;
  }
  s_funny_pos = 0;
}
}

void splash_tick_funny() {
  // Only the SVG trefoil splash (Default mode) rotates the cheeky lines;
  // the POST screen keeps its boot log clean.
  if (Settings::g_theme_mode != (uint8_t)Theme::Mode::Default) return;
  if (!splash_status) return;
  const uint32_t now = fast_millis();
  if (now - s_funny_last_ms < kFunnyIntervalMs) return;
  if (s_funny_pos >= kFunnyCount) shuffle_funny_order();
  s_funny_last_ms = now;
  lv_label_set_text(splash_status, s_funny_status[s_funny_order[s_funny_pos]]);
  s_funny_pos++;
}

// picker

void build_picker() {
  scr_picker = lv_obj_create(nullptr);
  style_dark_screen(scr_picker);

  lv_obj_t * title = lv_label_create(scr_picker);
  lv_label_set_text(title, "Pick a network");
  lv_obj_set_style_text_font(title, Theme::f_sub, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 8);

  picker_status = lv_label_create(scr_picker);
  lv_label_set_text(picker_status, "scanning...");
  lv_obj_set_style_text_font(picker_status, Theme::f_muted, 0);
  lv_obj_set_style_text_color(picker_status, lv_color_hex(Theme::kMuted), 0);
  lv_obj_align(picker_status, LV_ALIGN_TOP_RIGHT, -10, 12);

  picker_list = lv_list_create(scr_picker);
  lv_obj_set_size(picker_list, Board::SCREEN_W - 16, Board::SCREEN_H - 50);
  lv_obj_align(picker_list, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_obj_set_style_bg_color(picker_list, lv_color_hex(0x1f2326), 0);
  lv_obj_set_style_border_width(picker_list, 0, 0);
}

void picker_set_status(const char * msg) {
  if (picker_status) lv_label_set_text(picker_status, msg);
}

void picker_clear_list() {
  if (picker_list) lv_obj_clean(picker_list);
}

void picker_populate() {
  if (!picker_list) return;
  lv_obj_clean(picker_list);
  for (size_t i = 0; i < Wifi::g_scan_count; i++) {
    const auto & e = Wifi::g_scan[i];
    char row[64];
    if (e.enc == WIFI_AUTH_OPEN) {
      snprintf(row, sizeof(row), "%-20.20s  %d", e.ssid, (int)e.rssi);
    } else {
      snprintf(row, sizeof(row), LV_SYMBOL_WIFI " %-18.18s  %d", e.ssid, (int)e.rssi);
    }
    lv_obj_t * btn = lv_list_add_button(picker_list, nullptr, row);
    lv_obj_set_user_data(btn, reinterpret_cast<void *>((uintptr_t)i));
    lv_obj_set_style_text_font(btn, Theme::f_body, 0);
    lv_obj_add_event_cb(btn, on_picker_clicked, LV_EVENT_CLICKED, nullptr);
  }
}

// password

void build_password() {
  if (scr_password) return;

  scr_password = lv_obj_create(nullptr);
  style_dark_screen(scr_password);

  // Back to picker.
  lv_obj_t * back = lv_button_create(scr_password);
  lv_obj_set_size(back, 56, 32);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 4);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x2b3035), 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x373b3e), LV_STATE_PRESSED);
  lv_obj_set_style_radius(back, 6, 0);
  lv_obj_set_style_border_width(back, 0, 0);
  lv_obj_add_event_cb(back, on_password_back, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * back_lbl = lv_label_create(back);
  lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " back");
  lv_obj_set_style_text_font(back_lbl, Theme::f_muted, 0);
  lv_obj_set_style_text_color(back_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_center(back_lbl);

  password_label = lv_label_create(scr_password);
  lv_obj_set_style_text_font(password_label, Theme::f_muted, 0);
  lv_obj_set_style_text_color(password_label, lv_color_hex(Theme::kMuted), 0);
  lv_obj_set_width(password_label, Board::SCREEN_W - 72);
  lv_obj_set_style_text_align(password_label, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_align(password_label, LV_ALIGN_TOP_LEFT, 68, 12);

  password_input = lv_textarea_create(scr_password);
  lv_obj_set_width(password_input, Board::SCREEN_W - 16);
  lv_textarea_set_one_line(password_input, true);
  // Touch typing into a masked field is too punishing to recover from.
  lv_textarea_set_password_mode(password_input, false);
  lv_textarea_set_placeholder_text(password_input, "password");
  lv_obj_set_style_text_font(password_input, Theme::f_body, 0);
  lv_obj_align(password_input, LV_ALIGN_TOP_MID, 0, 44);

  password_kb = Kb::create(scr_password, password_input, Kb::Layout::Password);
  lv_obj_set_size(password_kb, Board::SCREEN_W, 200);
  lv_obj_align(password_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_event_cb(password_kb, on_password_event, LV_EVENT_ALL, nullptr);
}

void destroy_password() {
  if (!scr_password) return;
  lv_obj_delete(scr_password);
  scr_password   = nullptr;
  password_label = nullptr;
  password_input = nullptr;
  password_kb    = nullptr;
}

void password_show_for(const char * ssid) {
  if (!password_label || !password_input) return;
  char msg[64];
  snprintf(msg, sizeof(msg), "Password for %s", ssid ? ssid : "?");
  lv_label_set_text(password_label, msg);
  lv_textarea_set_text(password_input, "");
}

// main (station list)

namespace {
lv_obj_t * main_status = nullptr;
lv_obj_t * main_list   = nullptr;

// Reused in-place; avoid lv_obj_clean churn on every refresh.
struct Row {
  lv_obj_t * btn;
  lv_obj_t * name_lbl;
  lv_obj_t * cpm_lbl;
  lv_obj_t * meta_lbl;
};
Row main_rows[Stations::kMax];

}  // namespace

void build_main() {
  if (scr_main) return;

  // All-or-nothing alloc so a partial failure can't leave a non-null
  // last_name alongside null cpm/meta (next refresh would segfault).
  if (!last_name || !last_cpm || !last_meta) {
    free(last_name); free(last_cpm); free(last_meta);
    last_name = nullptr; last_cpm = nullptr; last_meta = nullptr;
    auto * n = (char (*)[40])calloc(Stations::g_max, sizeof(*last_name));
    auto * c = (char (*)[16])calloc(Stations::g_max, sizeof(*last_cpm));
    auto * m = (char (*)[48])calloc(Stations::g_max, sizeof(*last_meta));
    if (n && c && m) {
      last_name = n; last_cpm = c; last_meta = m;
    } else {
      free(n); free(c); free(m);
      Serial.println("[ui] diff-cache alloc failed; skipping build_main");
      return;
    }
  }

  scr_main = lv_obj_create(nullptr);
  style_dark_screen(scr_main);

  lv_obj_t * title = lv_label_create(scr_main);
  lv_label_set_text(title, Theme::kUppercaseTitles ? "STATIONS" : "Stations");
  lv_obj_set_style_text_font(title, Theme::f_sub, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(Theme::kTitleColor), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 8);

  main_status = lv_label_create(scr_main);
  lv_label_set_text(main_status, "looking...");
  lv_obj_set_style_text_font(main_status, Theme::f_muted, 0);
  lv_obj_set_style_text_color(main_status, lv_color_hex(Theme::kMuted), 0);
  lv_obj_align(main_status, LV_ALIGN_TOP_RIGHT, -50, 12);

  lv_obj_t * gear = lv_button_create(scr_main);
  lv_obj_set_size(gear, 36, 32);
  lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -4, 4);
  lv_obj_set_style_bg_color(gear, lv_color_hex(0x2b3035), 0);
  lv_obj_set_style_bg_color(gear, lv_color_hex(0x373b3e), LV_STATE_PRESSED);
  lv_obj_set_style_radius(gear, 6, 0);
  lv_obj_set_style_border_width(gear, 0, 0);
  lv_obj_add_event_cb(gear, on_settings_open, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * gear_lbl = lv_label_create(gear);
  lv_label_set_text(gear_lbl, LV_SYMBOL_SETTINGS);
  lv_obj_set_style_text_font(gear_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(gear_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_center(gear_lbl);

  // Manual container, not lv_list: pre-allocate kMax row widgets.
  main_list = lv_obj_create(scr_main);
  lv_obj_set_size(main_list, Board::SCREEN_W - 8, Board::SCREEN_H - 40);
  lv_obj_align(main_list, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_obj_set_style_bg_color(main_list, lv_color_hex(0x1f2326), 0);
  lv_obj_set_style_border_width(main_list, 0, 0);
  lv_obj_set_style_pad_all(main_list, 4, 0);
  lv_obj_set_style_pad_gap(main_list, 6, 0);
  lv_obj_set_flex_flow(main_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(main_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(main_list, LV_SCROLLBAR_MODE_AUTO);

  // Hidden until populated; LVGL flex skips them so empty rows collapse.
  // Only allocate up to the runtime cap so the Main screen widget count
  // shrinks for users who dialled down g_max_stations.
  for (size_t i = 0; i < Stations::g_max; i++) {
    Row & r = main_rows[i];
    r.btn = lv_button_create(main_list);
    lv_obj_set_size(r.btn, lv_pct(100), 52);
    lv_obj_set_style_bg_color(r.btn, lv_color_hex(0x2b3035), 0);
    lv_obj_set_style_bg_color(r.btn, lv_color_hex(0x373b3e), LV_STATE_PRESSED);
    lv_obj_set_style_radius(r.btn, 6, 0);
    lv_obj_set_style_border_width(r.btn, 0, 0);
    lv_obj_set_style_pad_all(r.btn, 6, 0);
    lv_obj_add_flag(r.btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(r.btn, reinterpret_cast<void *>((uintptr_t)i));
    lv_obj_add_event_cb(r.btn, on_main_row_clicked, LV_EVENT_CLICKED, nullptr);

    r.name_lbl = lv_label_create(r.btn);
    // Refresh switches long mode to SCROLL_CIRCULAR only when the hostname
    // overflows; default DOTS avoids an idle anim per row.
    lv_obj_set_style_text_font(r.name_lbl, Theme::f_muted, 0);
    lv_obj_set_style_text_color(r.name_lbl, lv_color_hex(Theme::kFg), 0);
    lv_obj_set_width(r.name_lbl, Board::SCREEN_W - 6 - 6 - 80);  // leave 80px for CPM
    lv_label_set_long_mode(r.name_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(r.name_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    r.cpm_lbl = lv_label_create(r.btn);
    lv_obj_set_style_text_font(r.cpm_lbl, Theme::f_body, 0);
    lv_obj_set_style_text_color(r.cpm_lbl, lv_color_hex(Theme::kAccent), 0);
    lv_obj_align(r.cpm_lbl, LV_ALIGN_TOP_RIGHT, 0, 0);

    r.meta_lbl = lv_label_create(r.btn);
    lv_obj_set_style_text_font(r.meta_lbl, Theme::f_muted, 0);
    lv_obj_set_style_text_color(r.meta_lbl, lv_color_hex(Theme::kMuted), 0);
    lv_obj_align(r.meta_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  }
}

void destroy_main() {
  if (!scr_main) return;
  lv_obj_delete(scr_main);
  scr_main    = nullptr;
  main_status = nullptr;
  main_list   = nullptr;
  // main_rows[] pointers now dangling.
  memset(main_rows, 0, sizeof(main_rows));
  if (last_name) memset(last_name, 0, Stations::g_max * sizeof(*last_name));
  if (last_cpm)  memset(last_cpm,  0, Stations::g_max * sizeof(*last_cpm));
  if (last_meta) memset(last_meta, 0, Stations::g_max * sizeof(*last_meta));
}

void main_set_status(const char * msg) {
  if (main_status) lv_label_set_text(main_status, msg);
}

void main_refresh_list() {
  if (!main_list) return;
  if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(200)) != pdTRUE) return;
  const size_t n = Stations::g_count;
  const uint32_t now = fast_millis();

  if (main_status) {
    char st[24];
    snprintf(st, sizeof(st), "%u / %u", (unsigned)n, (unsigned)Stations::g_max);
    lv_label_set_text(main_status, st);
  }

  // Alphabetical by host, insertion sort.
  uint8_t order[Stations::kMax];
  for (uint8_t k = 0; k < Stations::kMax; k++) order[k] = k;
  for (size_t i = 1; i < n; i++) {
    const uint8_t cur = order[i];
    size_t j = i;
    while (j > 0 &&
           strcasecmp(Stations::g_stations[order[j - 1]].host,
                      Stations::g_stations[cur].host) > 0) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = cur;
  }

  for (size_t i = 0; i < Stations::g_max; i++) {
    Row & r = main_rows[i];
    if (i >= n) {
      lv_obj_add_flag(r.btn, LV_OBJ_FLAG_HIDDEN);
      last_name[i][0] = last_cpm[i][0] = last_meta[i][0] = '\0';
      continue;
    }
    const size_t              station_idx = order[i];
    const Stations::Station & s           = Stations::g_stations[station_idx];
    // user_data routes the tap to the right station.
    lv_obj_set_user_data(r.btn, reinterpret_cast<void *>((uintptr_t)station_idx));

    char name[40];
    snprintf(name, sizeof(name), "%s", Stations::label(s));
    if (strcmp(name, last_name[i]) != 0) {
      strncpy(last_name[i], name, sizeof(last_name[i]) - 1);
      lv_label_set_text(r.name_lbl, name);
      lv_point_t sz;
      lv_text_get_size(&sz, name, Theme::f_muted, 0, 0, LV_COORD_MAX,
                       LV_TEXT_FLAG_NONE);
      const int32_t avail = Board::SCREEN_W - 6 - 6 - 80;
      lv_label_set_long_mode(r.name_lbl,
        sz.x > avail ? LV_LABEL_LONG_MODE_SCROLL_CIRCULAR
                     : LV_LABEL_LONG_MODE_DOTS);
    }

    char cpm[16];
    if (Stations::is_saturated(s)) {
      snprintf(cpm, sizeof(cpm), "%d CPM SAT", (int)(s.cpm_now + 0.5f));
    } else if (Stations::is_faulted(s)) {
      snprintf(cpm, sizeof(cpm), "FAULT");
    } else if (s.cpm_now > 0 || Stations::is_udp_active(s)) {
      snprintf(cpm, sizeof(cpm), "%d CPM", (int)(s.cpm_now + 0.5f));
    } else {
      snprintf(cpm, sizeof(cpm), "--");
    }
    if (strcmp(cpm, last_cpm[i]) != 0) {
      strncpy(last_cpm[i], cpm, sizeof(last_cpm[i]) - 1);
      lv_label_set_text(r.cpm_lbl, cpm);
      uint32_t col = Theme::kAccent;
      if      (Stations::is_faulted(s))   col = 0xff5555;
      else if (Stations::is_saturated(s)) col = Theme::kAccentYellow;
      lv_obj_set_style_text_color(r.cpm_lbl, lv_color_hex(col), 0);
    }

    char meta[48];
    // newer of {poll_ok, click}; mDNS last_seen doesn't mean the CPM is fresh.
    const char * udp_tag = Stations::is_udp_active(s) ? " " LV_SYMBOL_BELL : "";
    const uint32_t fresh = (s.last_click_at_ms > s.last_poll_ok_at_ms)
                            ? s.last_click_at_ms : s.last_poll_ok_at_ms;
    // Manual format; IPAddress::toString() allocates.
    if (fresh == 0) {
      snprintf(meta, sizeof(meta), "%u.%u.%u.%u  never%s",
               s.ip[0], s.ip[1], s.ip[2], s.ip[3], udp_tag);
    } else {
      const uint32_t age_s = (now - fresh) / 1000;
      snprintf(meta, sizeof(meta), "%u.%u.%u.%u  %lus ago%s",
               s.ip[0], s.ip[1], s.ip[2], s.ip[3],
               (unsigned long)age_s, udp_tag);
    }
    if (strcmp(meta, last_meta[i]) != 0) {
      strncpy(last_meta[i], meta, sizeof(last_meta[i]) - 1);
      lv_label_set_text(r.meta_lbl, meta);
    }
    lv_obj_clear_flag(r.btn, LV_OBJ_FLAG_HIDDEN);
  }
  xSemaphoreGive(Stations::g_mux);
}

// settings

namespace {
lv_obj_t * settings_poll_lbl   = nullptr;
lv_obj_t * settings_poll_slide = nullptr;
lv_obj_t * settings_bl_lbl     = nullptr;
lv_obj_t * settings_bl_slide   = nullptr;
lv_obj_t * settings_udp_switch   = nullptr;
lv_obj_t * settings_udp_lbl      = nullptr;
lv_obj_t * settings_audio_lbl    = nullptr;
lv_obj_t * settings_audio_slide  = nullptr;
lv_obj_t * settings_theme_lbl    = nullptr;
lv_obj_t * settings_top_bar      = nullptr;
lv_obj_t * settings_host_lbl     = nullptr;
lv_obj_t * settings_factory_lbl  = nullptr;
lv_obj_t * settings_scsv_lbl     = nullptr;
lv_obj_t * settings_smax_lbl     = nullptr;
lv_obj_t * settings_led_switch   = nullptr;
lv_obj_t * settings_rmw_switch   = nullptr;
lv_obj_t * settings_caw_switch   = nullptr;

// LVGL 9.5 fires VALUE_CHANGED on programmatic state changes too; gates
// handlers during settings_refresh().
bool s_settings_refreshing = false;

void update_poll_label() {
  if (!settings_poll_lbl) return;
  char buf[32];
  snprintf(buf, sizeof(buf), "Poll every %lus",
           (unsigned long)(Settings::g_poll_gap_ms / 1000));
  lv_label_set_text(settings_poll_lbl, buf);
}

void update_bl_label() {
  if (!settings_bl_lbl) return;
  char buf[24];
  snprintf(buf, sizeof(buf), "Backlight %d%%",
           (int)(Settings::g_backlight * 100.0f + 0.5f));
  lv_label_set_text(settings_bl_lbl, buf);
}
void on_poll_slider(lv_event_t * e) {
  if (s_settings_refreshing) return;
  lv_obj_t * s = static_cast<lv_obj_t *>(lv_event_get_target(e));
  uint32_t secs = (uint32_t)lv_slider_get_value(s);
  if (secs < 1)  secs = 1;
  if (secs > 30) secs = 30;
  Settings::g_poll_gap_ms = secs * 1000;
  update_poll_label();
  s_settings_dirty = true;
}

void on_bl_slider(lv_event_t * e) {
  if (s_settings_refreshing) return;
  lv_obj_t * s = static_cast<lv_obj_t *>(lv_event_get_target(e));
  int pct = lv_slider_get_value(s);
  if (pct < 10)  pct = 10;
  if (pct > 100) pct = 100;
  Settings::g_backlight = (float)pct / 100.0f;
  Display::set_backlight(Settings::g_backlight);
  update_bl_label();
  s_settings_dirty = true;
}

void on_udp_toggle(lv_event_t * e) {
  if (s_settings_refreshing) return;
  lv_obj_t * sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
  Settings::g_udp_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
  s_settings_dirty = true;
}

void on_audio_slider(lv_event_t * e) {
  if (s_settings_refreshing) return;
  lv_obj_t * s = static_cast<lv_obj_t *>(lv_event_get_target(e));
  int v = lv_slider_get_value(s);
  if (v < 0)   v = 0;
  if (v > 100) v = 100;
  Settings::g_audio_volume  = (uint8_t)v;
  Settings::g_audio_enabled = (v > 0);
  if (settings_audio_lbl) {
    char buf[24];
    snprintf(buf, sizeof(buf), v == 0 ? "Audio off" : "Audio %d%%", v);
    lv_label_set_text(settings_audio_lbl, buf);
  }
  s_settings_dirty = true;
  if (v > 0) Audio::click();
}

void on_led_toggle(lv_event_t * e) {
  if (s_settings_refreshing) return;
  lv_obj_t * sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
  Settings::g_led_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
  s_settings_dirty = true;
  Led::tick();
}

void on_rmw_toggle(lv_event_t * e) {
  if (s_settings_refreshing) return;
  lv_obj_t * sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
  Settings::g_radmon_watchdog = lv_obj_has_state(sw, LV_STATE_CHECKED);
  s_settings_dirty = true;
}

void on_caw_toggle(lv_event_t * e) {
  if (s_settings_refreshing) return;
  lv_obj_t * sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
  Settings::g_cpm_alert_watchdog = lv_obj_has_state(sw, LV_STATE_CHECKED);
  s_settings_dirty = true;
}

constexpr uint16_t kScsvPresets[] = { 0, 30, 60, 300, 900, 1800 };

void format_scsv(char * buf, size_t cap) {
  const uint16_t v = Settings::g_screensaver_secs;
  if      (v == 0)        snprintf(buf, cap, "Off");
  else if (v < 60)        snprintf(buf, cap, "%us", (unsigned)v);
  else if (v % 60 == 0 && v < 3600)
                          snprintf(buf, cap, "%um", (unsigned)(v / 60));
  else                    snprintf(buf, cap, "%um%us",
                                   (unsigned)(v / 60), (unsigned)(v % 60));
}

void update_scsv_label() {
  if (!settings_scsv_lbl) return;
  char b[12];
  format_scsv(b, sizeof(b));
  lv_label_set_text(settings_scsv_lbl, b);
}

void on_screensaver_cycle(lv_event_t *) {
  const size_t n = sizeof(kScsvPresets) / sizeof(kScsvPresets[0]);
  size_t idx = 0;
  for (size_t i = 0; i < n; i++) {
    if (kScsvPresets[i] == Settings::g_screensaver_secs) { idx = i; break; }
  }
  Settings::g_screensaver_secs = kScsvPresets[(idx + 1) % n];
  Settings::save();
  update_scsv_label();
}

void update_smax_label() {
  if (!settings_smax_lbl) return;
  char buf[16];
  const bool pending = (Settings::g_max_stations != Stations::g_max);
  snprintf(buf, sizeof(buf), "%u%s",
           (unsigned)Settings::g_max_stations,
           pending ? " *" : "");
  lv_label_set_text(settings_smax_lbl, buf);
}

void on_max_stations_cycle(lv_event_t *) {
  if (s_settings_refreshing) return;
  uint8_t v = Settings::g_max_stations;
  v = (v == 8) ? 16 : (v == 16 ? 32 : 8);
  Settings::g_max_stations = v;
  s_settings_dirty = true;
  update_smax_label();
}

void on_open_dice(lv_event_t *) {
  request_state_change(State::Dice);
}

void on_open_info(lv_event_t *) {
  request_state_change(State::Info);
}

void do_wifi_reset() {
  Wifi::save_creds("", "");
  WiFi.disconnect(/*eraseap=*/true, /*wifioff=*/false);
  delay(50);
  ESP.restart();
}

void do_factory_reset() {
  Settings::factory_reset();
  delay(50);
  ESP.restart();
}

void on_wifi_reset(lv_event_t *) {
  show_confirm("Reset Wi-Fi credentials?",
               "The gadget will reboot and ask you to pick a network again.",
               do_wifi_reset);
}

void on_factory_reset(lv_event_t *) {
  show_confirm("Factory reset?",
               "Wipes Wi-Fi, calibration, theme and every other setting. "
               "The gadget will reboot.",
               do_factory_reset);
}

}  // anonymous namespace

void build_settings() {
  if (scr_settings) return;
  scr_settings = lv_obj_create(nullptr);
  style_dark_screen(scr_settings);
  lv_obj_set_scroll_dir(scr_settings, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(scr_settings, LV_SCROLLBAR_MODE_AUTO);

  // FLOATING: pinned regardless of scroll, opaque bg.
  settings_top_bar = lv_obj_create(scr_settings);
  lv_obj_remove_style_all(settings_top_bar);
  lv_obj_set_size(settings_top_bar, Board::SCREEN_W, 40);
  lv_obj_align(settings_top_bar, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_color(settings_top_bar, lv_color_hex(Theme::kBg), 0);
  lv_obj_set_style_bg_opa(settings_top_bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(settings_top_bar, 1, 0);
  lv_obj_set_style_border_color(settings_top_bar, lv_color_hex(0x373b3e), 0);
  lv_obj_set_style_border_side(settings_top_bar, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_pad_all(settings_top_bar, 0, 0);
  lv_obj_add_flag(settings_top_bar, LV_OBJ_FLAG_FLOATING);
  lv_obj_remove_flag(settings_top_bar, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t * back = lv_button_create(settings_top_bar);
  lv_obj_set_size(back, 56, 32);
  lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x2b3035), 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x373b3e), LV_STATE_PRESSED);
  lv_obj_set_style_radius(back, 6, 0);
  lv_obj_set_style_border_width(back, 0, 0);
  lv_obj_remove_flag(back, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(back, on_settings_close, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * back_lbl = lv_label_create(back);
  lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_font(back_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(back_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_center(back_lbl);

  lv_obj_t * title = lv_label_create(settings_top_bar);
  lv_label_set_text(title, "Settings");
  lv_obj_set_style_text_font(title, Theme::f_sub, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(title, LV_ALIGN_LEFT_MID, 70, 0);

  // Save button. Back-button is the cancel path.
  lv_obj_t * save = lv_button_create(settings_top_bar);
  lv_obj_set_size(save, 40, 32);
  lv_obj_align(save, LV_ALIGN_RIGHT_MID, -6, 0);
  lv_obj_set_style_bg_color(save, lv_color_hex(Theme::kAccent), 0);
  lv_obj_set_style_bg_color(save, lv_color_hex(0x3380dd), LV_STATE_PRESSED);
  lv_obj_set_style_radius(save, 6, 0);
  lv_obj_set_style_border_width(save, 0, 0);
  // Don't let drift get promoted to a scroll on this small button.
  lv_obj_remove_flag(save, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(save, on_settings_save, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * save_lbl = lv_label_create(save);
  lv_label_set_text(save_lbl, LV_SYMBOL_OK);
  lv_obj_set_style_text_font(save_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(save_lbl, lv_color_hex(0xffffff), 0);
  lv_obj_center(save_lbl);

  auto section_header = [](lv_obj_t * parent, int y, const char * text) {
    lv_obj_t * lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, Theme::f_muted, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::kAccent), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 12, y);
  };

  section_header(scr_settings, 50,  "DISPLAY");

  settings_bl_lbl = lv_label_create(scr_settings);
  lv_obj_set_style_text_font(settings_bl_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(settings_bl_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(settings_bl_lbl, LV_ALIGN_TOP_LEFT, 12, 72);

  settings_bl_slide = lv_slider_create(scr_settings);
  lv_obj_set_width(settings_bl_slide, Board::SCREEN_W - 24);
  lv_obj_align(settings_bl_slide, LV_ALIGN_TOP_LEFT, 12, 100);
  lv_slider_set_range(settings_bl_slide, 10, 100);
  style_slider(settings_bl_slide);
  lv_obj_add_event_cb(settings_bl_slide, on_bl_slider, LV_EVENT_RELEASED, nullptr);

  // Cycle Theme:: + rebuild the screen.
  lv_obj_t * theme_btn = lv_button_create(scr_settings);
  lv_obj_set_size(theme_btn, Board::SCREEN_W - 24, 28);
  lv_obj_align(theme_btn, LV_ALIGN_TOP_LEFT, 12, 132);
  lv_obj_set_style_bg_color(theme_btn, lv_color_hex(Theme::kRowBg), 0);
  lv_obj_set_style_bg_color(theme_btn, lv_color_hex(Theme::kRowBgPressed), LV_STATE_PRESSED);
  lv_obj_set_style_radius(theme_btn, Theme::kRadiusBtn, 0);
  lv_obj_set_style_border_width(theme_btn, 0, 0);
  lv_obj_set_style_pad_left(theme_btn, 8, 0);
  lv_obj_set_style_pad_right(theme_btn, 8, 0);
  lv_obj_add_event_cb(theme_btn, on_theme_cycle, LV_EVENT_CLICKED, nullptr);

  lv_obj_t * theme_caption = lv_label_create(theme_btn);
  lv_label_set_text(theme_caption, "Theme");
  lv_obj_set_style_text_font(theme_caption, Theme::f_body, 0);
  lv_obj_set_style_text_color(theme_caption, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(theme_caption, LV_ALIGN_LEFT_MID, 0, 0);

  settings_theme_lbl = lv_label_create(theme_btn);
  lv_obj_set_style_text_font(settings_theme_lbl, Theme::f_muted, 0);
  lv_obj_set_style_text_color(settings_theme_lbl, lv_color_hex(Theme::kMuted), 0);
  lv_obj_align(settings_theme_lbl, LV_ALIGN_RIGHT_MID, -18, 0);

  lv_obj_t * theme_chev = lv_label_create(theme_btn);
  lv_label_set_text(theme_chev, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_font(theme_chev, Theme::f_muted, 0);
  lv_obj_set_style_text_color(theme_chev, lv_color_hex(Theme::kMuted), 0);
  lv_obj_align(theme_chev, LV_ALIGN_RIGHT_MID, 0, 0);

  lv_obj_t * scsv_btn = lv_button_create(scr_settings);
  lv_obj_set_size(scsv_btn, Board::SCREEN_W - 24, 28);
  lv_obj_align(scsv_btn, LV_ALIGN_TOP_LEFT, 12, 170);
  lv_obj_set_style_bg_color(scsv_btn, lv_color_hex(Theme::kRowBg), 0);
  lv_obj_set_style_bg_color(scsv_btn, lv_color_hex(Theme::kRowBgPressed), LV_STATE_PRESSED);
  lv_obj_set_style_radius(scsv_btn, Theme::kRadiusBtn, 0);
  lv_obj_set_style_border_width(scsv_btn, 0, 0);
  lv_obj_set_style_pad_left(scsv_btn, 8, 0);
  lv_obj_set_style_pad_right(scsv_btn, 8, 0);
  lv_obj_add_event_cb(scsv_btn, on_screensaver_cycle, LV_EVENT_CLICKED, nullptr);

  lv_obj_t * scsv_caption = lv_label_create(scsv_btn);
  lv_label_set_text(scsv_caption, "Screensaver");
  lv_obj_set_style_text_font(scsv_caption, Theme::f_body, 0);
  lv_obj_set_style_text_color(scsv_caption, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(scsv_caption, LV_ALIGN_LEFT_MID, 0, 0);

  settings_scsv_lbl = lv_label_create(scsv_btn);
  lv_obj_set_style_text_font(settings_scsv_lbl, Theme::f_muted, 0);
  lv_obj_set_style_text_color(settings_scsv_lbl, lv_color_hex(Theme::kMuted), 0);
  lv_obj_align(settings_scsv_lbl, LV_ALIGN_RIGHT_MID, -18, 0);

  lv_obj_t * scsv_chev = lv_label_create(scsv_btn);
  lv_label_set_text(scsv_chev, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_font(scsv_chev, Theme::f_muted, 0);
  lv_obj_set_style_text_color(scsv_chev, lv_color_hex(Theme::kMuted), 0);
  lv_obj_align(scsv_chev, LV_ALIGN_RIGHT_MID, 0, 0);

  section_header(scr_settings, 210, "SOUND & ALERTS");

  settings_audio_lbl = lv_label_create(scr_settings);
  lv_obj_set_style_text_font(settings_audio_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(settings_audio_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(settings_audio_lbl, LV_ALIGN_TOP_LEFT, 12, 232);

  settings_audio_slide = lv_slider_create(scr_settings);
  lv_obj_set_width(settings_audio_slide, Board::SCREEN_W - 24);
  lv_obj_align(settings_audio_slide, LV_ALIGN_TOP_LEFT, 12, 260);
  lv_slider_set_range(settings_audio_slide, 0, 100);
  style_slider(settings_audio_slide);
  lv_obj_add_event_cb(settings_audio_slide, on_audio_slider, LV_EVENT_RELEASED, nullptr);

  lv_obj_t * led_label = lv_label_create(scr_settings);
  lv_label_set_text(led_label, "Health LED");
  lv_obj_set_style_text_font(led_label, Theme::f_body, 0);
  lv_obj_set_style_text_color(led_label, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(led_label, LV_ALIGN_TOP_LEFT, 12, 292);

  settings_led_switch = lv_switch_create(scr_settings);
  lv_obj_align(settings_led_switch, LV_ALIGN_TOP_RIGHT, -12, 288);
  style_switch(settings_led_switch);
  lv_obj_add_event_cb(settings_led_switch, on_led_toggle,
                      LV_EVENT_VALUE_CHANGED, nullptr);

  // Klaxon on majority Radmon down.
  lv_obj_t * rmw_label = lv_label_create(scr_settings);
  lv_label_set_text(rmw_label, "Radmon watchdog");
  lv_obj_set_style_text_font(rmw_label, Theme::f_body, 0);
  lv_obj_set_style_text_color(rmw_label, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(rmw_label, LV_ALIGN_TOP_LEFT, 12, 326);

  settings_rmw_switch = lv_switch_create(scr_settings);
  lv_obj_align(settings_rmw_switch, LV_ALIGN_TOP_RIGHT, -12, 322);
  style_switch(settings_rmw_switch);
  lv_obj_add_event_cb(settings_rmw_switch, on_rmw_toggle,
                      LV_EVENT_VALUE_CHANGED, nullptr);

  // Triggers on /rad state="alert" (station's own cpm_alert, snooze-aware).
  lv_obj_t * caw_label = lv_label_create(scr_settings);
  lv_label_set_text(caw_label, "High-CPM klaxon");
  lv_obj_set_style_text_font(caw_label, Theme::f_body, 0);
  lv_obj_set_style_text_color(caw_label, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(caw_label, LV_ALIGN_TOP_LEFT, 12, 360);

  settings_caw_switch = lv_switch_create(scr_settings);
  lv_obj_align(settings_caw_switch, LV_ALIGN_TOP_RIGHT, -12, 356);
  style_switch(settings_caw_switch);
  lv_obj_add_event_cb(settings_caw_switch, on_caw_toggle,
                      LV_EVENT_VALUE_CHANGED, nullptr);

  section_header(scr_settings, 402, "NETWORK");

  settings_poll_lbl = lv_label_create(scr_settings);
  lv_obj_set_style_text_font(settings_poll_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(settings_poll_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(settings_poll_lbl, LV_ALIGN_TOP_LEFT, 12, 424);

  settings_poll_slide = lv_slider_create(scr_settings);
  lv_obj_set_width(settings_poll_slide, Board::SCREEN_W - 24);
  lv_obj_align(settings_poll_slide, LV_ALIGN_TOP_LEFT, 12, 452);
  lv_slider_set_range(settings_poll_slide, 1, 30);
  style_slider(settings_poll_slide);
  lv_obj_add_event_cb(settings_poll_slide, on_poll_slider, LV_EVENT_RELEASED, nullptr);

  lv_obj_t * udp_label = lv_label_create(scr_settings);
  lv_label_set_text(udp_label, "UDP click stream");
  lv_obj_set_style_text_font(udp_label, Theme::f_body, 0);
  lv_obj_set_style_text_color(udp_label, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(udp_label, LV_ALIGN_TOP_LEFT, 12, 484);

  settings_udp_switch = lv_switch_create(scr_settings);
  lv_obj_align(settings_udp_switch, LV_ALIGN_TOP_RIGHT, -12, 480);
  style_switch(settings_udp_switch);
  lv_obj_add_event_cb(settings_udp_switch, on_udp_toggle, LV_EVENT_VALUE_CHANGED, nullptr);

  // Tap opens the textarea+keyboard edit screen.
  lv_obj_t * mcast_btn = lv_button_create(scr_settings);
  lv_obj_set_size(mcast_btn, Board::SCREEN_W - 24, 26);
  lv_obj_align(mcast_btn, LV_ALIGN_TOP_LEFT, 12, 518);
  lv_obj_set_style_bg_color(mcast_btn, lv_color_hex(Theme::kRowBg), 0);
  lv_obj_set_style_bg_color(mcast_btn, lv_color_hex(Theme::kRowBgPressed), LV_STATE_PRESSED);
  lv_obj_set_style_radius(mcast_btn, Theme::kRadiusBtn, 0);
  lv_obj_set_style_border_width(mcast_btn, 0, 0);
  lv_obj_set_style_pad_left(mcast_btn, 8, 0);
  lv_obj_set_style_pad_right(mcast_btn, 8, 0);
  lv_obj_add_event_cb(mcast_btn, on_multicast_tap, LV_EVENT_CLICKED, nullptr);

  settings_udp_lbl = lv_label_create(mcast_btn);
  lv_obj_set_style_text_font(settings_udp_lbl, Theme::f_muted, 0);
  lv_obj_set_style_text_color(settings_udp_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(settings_udp_lbl, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t * chev = lv_label_create(mcast_btn);
  lv_label_set_text(chev, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_font(chev, Theme::f_muted, 0);
  lv_obj_set_style_text_color(chev, lv_color_hex(Theme::kMuted), 0);
  lv_obj_align(chev, LV_ALIGN_RIGHT_MID, 0, 0);

  lv_obj_t * host_btn = lv_button_create(scr_settings);
  lv_obj_set_size(host_btn, Board::SCREEN_W - 24, 26);
  lv_obj_align(host_btn, LV_ALIGN_TOP_LEFT, 12, 552);
  lv_obj_set_style_bg_color(host_btn, lv_color_hex(Theme::kRowBg), 0);
  lv_obj_set_style_bg_color(host_btn, lv_color_hex(Theme::kRowBgPressed), LV_STATE_PRESSED);
  lv_obj_set_style_radius(host_btn, Theme::kRadiusBtn, 0);
  lv_obj_set_style_border_width(host_btn, 0, 0);
  lv_obj_set_style_pad_left(host_btn, 8, 0);
  lv_obj_set_style_pad_right(host_btn, 8, 0);
  lv_obj_add_event_cb(host_btn, on_hostname_tap, LV_EVENT_CLICKED, nullptr);

  lv_obj_t * host_caption = lv_label_create(host_btn);
  lv_label_set_text(host_caption, "Hostname");
  lv_obj_set_style_text_font(host_caption, Theme::f_body, 0);
  lv_obj_set_style_text_color(host_caption, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(host_caption, LV_ALIGN_LEFT_MID, 0, 0);

  settings_host_lbl = lv_label_create(host_btn);
  lv_obj_set_style_text_font(settings_host_lbl, Theme::f_muted, 0);
  lv_obj_set_style_text_color(settings_host_lbl, lv_color_hex(Theme::kMuted), 0);
  lv_obj_set_width(settings_host_lbl, 110);
  lv_obj_set_style_text_align(settings_host_lbl, LV_TEXT_ALIGN_RIGHT, 0);
  lv_label_set_long_mode(settings_host_lbl, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_align(settings_host_lbl, LV_ALIGN_RIGHT_MID, -18, 0);

  lv_obj_t * host_chev = lv_label_create(host_btn);
  lv_label_set_text(host_chev, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_font(host_chev, Theme::f_muted, 0);
  lv_obj_set_style_text_color(host_chev, lv_color_hex(Theme::kMuted), 0);
  lv_obj_align(host_chev, LV_ALIGN_RIGHT_MID, 0, 0);

  section_header(scr_settings, 592, "DEVICE");

  lv_obj_t * cal_btn = lv_button_create(scr_settings);
  lv_obj_set_size(cal_btn, Board::SCREEN_W - 24, 26);
  lv_obj_align(cal_btn, LV_ALIGN_TOP_LEFT, 12, 648);
  lv_obj_set_style_bg_color(cal_btn, lv_color_hex(Theme::kRowBg), 0);
  lv_obj_set_style_bg_color(cal_btn, lv_color_hex(Theme::kRowBgPressed), LV_STATE_PRESSED);
  lv_obj_set_style_radius(cal_btn, Theme::kRadiusBtn, 0);
  lv_obj_set_style_border_width(cal_btn, 0, 0);
  lv_obj_set_style_pad_left(cal_btn, 8, 0);
  lv_obj_set_style_pad_right(cal_btn, 8, 0);
  lv_obj_add_event_cb(cal_btn, on_calibrate_tap, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * cal_lbl = lv_label_create(cal_btn);
  lv_label_set_text(cal_lbl, "Calibrate touch");
  lv_obj_set_style_text_font(cal_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(cal_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(cal_lbl, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t * cal_chev = lv_label_create(cal_btn);
  lv_label_set_text(cal_chev, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_font(cal_chev, Theme::f_muted, 0);
  lv_obj_set_style_text_color(cal_chev, lv_color_hex(Theme::kMuted), 0);
  lv_obj_align(cal_chev, LV_ALIGN_RIGHT_MID, 0, 0);

  lv_obj_t * dice_row = lv_button_create(scr_settings);
  lv_obj_set_size(dice_row, Board::SCREEN_W - 24, 26);
  lv_obj_align(dice_row, LV_ALIGN_TOP_LEFT, 12, 682);
  lv_obj_set_style_bg_color(dice_row, lv_color_hex(Theme::kRowBg), 0);
  lv_obj_set_style_bg_color(dice_row, lv_color_hex(Theme::kRowBgPressed), LV_STATE_PRESSED);
  lv_obj_set_style_radius(dice_row, Theme::kRadiusBtn, 0);
  lv_obj_set_style_border_width(dice_row, 0, 0);
  lv_obj_set_style_pad_left(dice_row, 8, 0);
  lv_obj_set_style_pad_right(dice_row, 8, 0);
  lv_obj_add_event_cb(dice_row, on_open_dice, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * dice_caption = lv_label_create(dice_row);
  lv_label_set_text(dice_caption, "Dice roller");
  lv_obj_set_style_text_font(dice_caption, Theme::f_body, 0);
  lv_obj_set_style_text_color(dice_caption, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(dice_caption, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t * dice_chev = lv_label_create(dice_row);
  lv_label_set_text(dice_chev, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_font(dice_chev, Theme::f_muted, 0);
  lv_obj_set_style_text_color(dice_chev, lv_color_hex(Theme::kMuted), 0);
  lv_obj_align(dice_chev, LV_ALIGN_RIGHT_MID, 0, 0);

  lv_obj_t * info_row = lv_button_create(scr_settings);
  lv_obj_set_size(info_row, Board::SCREEN_W - 24, 26);
  lv_obj_align(info_row, LV_ALIGN_TOP_LEFT, 12, 716);
  lv_obj_set_style_bg_color(info_row, lv_color_hex(Theme::kRowBg), 0);
  lv_obj_set_style_bg_color(info_row, lv_color_hex(Theme::kRowBgPressed), LV_STATE_PRESSED);
  lv_obj_set_style_radius(info_row, Theme::kRadiusBtn, 0);
  lv_obj_set_style_border_width(info_row, 0, 0);
  lv_obj_set_style_pad_left(info_row, 8, 0);
  lv_obj_set_style_pad_right(info_row, 8, 0);
  lv_obj_add_event_cb(info_row, on_open_info, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * info_caption = lv_label_create(info_row);
  lv_label_set_text(info_caption, "Info");
  lv_obj_set_style_text_font(info_caption, Theme::f_body, 0);
  lv_obj_set_style_text_color(info_caption, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(info_caption, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t * info_chev = lv_label_create(info_row);
  lv_label_set_text(info_chev, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_font(info_chev, Theme::f_muted, 0);
  lv_obj_set_style_text_color(info_chev, lv_color_hex(Theme::kMuted), 0);
  lv_obj_align(info_chev, LV_ALIGN_RIGHT_MID, 0, 0);

  // Max stations: tap to cycle 8 / 16 / 32, reboot required to take effect.
  lv_obj_t * smax_btn = lv_button_create(scr_settings);
  lv_obj_set_size(smax_btn, Board::SCREEN_W - 24, 26);
  lv_obj_align(smax_btn, LV_ALIGN_TOP_LEFT, 12, 614);
  lv_obj_set_style_bg_color(smax_btn, lv_color_hex(Theme::kRowBg), 0);
  lv_obj_set_style_bg_color(smax_btn, lv_color_hex(Theme::kRowBgPressed), LV_STATE_PRESSED);
  lv_obj_set_style_radius(smax_btn, Theme::kRadiusBtn, 0);
  lv_obj_set_style_border_width(smax_btn, 0, 0);
  lv_obj_set_style_pad_left(smax_btn, 8, 0);
  lv_obj_set_style_pad_right(smax_btn, 8, 0);
  lv_obj_add_event_cb(smax_btn, on_max_stations_cycle, LV_EVENT_CLICKED, nullptr);

  lv_obj_t * smax_caption = lv_label_create(smax_btn);
  lv_label_set_text(smax_caption, "Max stations");
  lv_obj_set_style_text_font(smax_caption, Theme::f_body, 0);
  lv_obj_set_style_text_color(smax_caption, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(smax_caption, LV_ALIGN_LEFT_MID, 0, 0);

  settings_smax_lbl = lv_label_create(smax_btn);
  lv_obj_set_style_text_font(settings_smax_lbl, Theme::f_muted, 0);
  lv_obj_set_style_text_color(settings_smax_lbl, lv_color_hex(Theme::kMuted), 0);
  lv_obj_align(settings_smax_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

  {
    lv_obj_t * danger_lbl = lv_label_create(scr_settings);
    lv_label_set_text(danger_lbl, "DANGER ZONE");
    lv_obj_set_style_text_font(danger_lbl, Theme::f_muted, 0);
    lv_obj_set_style_text_color(danger_lbl, lv_color_hex(0xff5555), 0);
    lv_obj_align(danger_lbl, LV_ALIGN_TOP_LEFT, 12, 756);
  }

  lv_obj_t * reset = lv_button_create(scr_settings);
  lv_obj_set_size(reset, Board::SCREEN_W - 24, 26);
  lv_obj_align(reset, LV_ALIGN_TOP_LEFT, 12, 778);
  lv_obj_set_style_bg_color(reset, lv_color_hex(0x4a3a1a), 0);
  lv_obj_set_style_bg_color(reset, lv_color_hex(0x5a4626), LV_STATE_PRESSED);
  lv_obj_set_style_radius(reset, 6, 0);
  lv_obj_set_style_border_width(reset, 0, 0);
  lv_obj_add_event_cb(reset, on_wifi_reset, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * reset_lbl = lv_label_create(reset);
  lv_label_set_text(reset_lbl, "Reset Wi-Fi credentials");
  lv_obj_set_style_text_font(reset_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(reset_lbl, lv_color_hex(Theme::kAccentYellow), 0);
  lv_obj_center(reset_lbl);

  lv_obj_t * factory = lv_button_create(scr_settings);
  lv_obj_set_size(factory, Board::SCREEN_W - 24, 26);
  lv_obj_align(factory, LV_ALIGN_TOP_LEFT, 12, 812);
  lv_obj_set_style_bg_color(factory, lv_color_hex(0x4a1a1a), 0);
  lv_obj_set_style_bg_color(factory, lv_color_hex(0x5a2626), LV_STATE_PRESSED);
  lv_obj_set_style_radius(factory, 6, 0);
  lv_obj_set_style_border_width(factory, 0, 0);
  lv_obj_add_event_cb(factory, on_factory_reset, LV_EVENT_CLICKED, nullptr);
  settings_factory_lbl = lv_label_create(factory);
  lv_label_set_text(settings_factory_lbl, "Factory reset");
  lv_obj_set_style_text_font(settings_factory_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(settings_factory_lbl, lv_color_hex(0xffaaaa), 0);
  lv_obj_center(settings_factory_lbl);

  lv_obj_move_foreground(settings_top_bar);
}

void destroy_settings() {
  if (!scr_settings) return;
  lv_obj_delete(scr_settings);
  scr_settings         = nullptr;
  settings_poll_lbl    = nullptr;
  settings_poll_slide  = nullptr;
  settings_bl_lbl      = nullptr;
  settings_bl_slide    = nullptr;
  settings_udp_switch  = nullptr;
  settings_udp_lbl     = nullptr;
  settings_audio_lbl   = nullptr;
  settings_audio_slide = nullptr;
  settings_theme_lbl   = nullptr;
  settings_top_bar     = nullptr;
  settings_host_lbl    = nullptr;
  settings_factory_lbl = nullptr;
  settings_scsv_lbl    = nullptr;
  settings_smax_lbl    = nullptr;
  settings_led_switch  = nullptr;
  settings_rmw_switch  = nullptr;
  settings_caw_switch  = nullptr;
}

void settings_refresh() {
  if (!scr_settings) return;
  // Preserve the first-entry snapshot when bouncing through Info/TouchCal.
  if (!s_settings_dirty) settings_take_snapshot();
  s_settings_refreshing = true;
  lv_slider_set_value(settings_poll_slide,
                      (int)(Settings::g_poll_gap_ms / 1000),
                      LV_ANIM_OFF);
  update_poll_label();

  lv_slider_set_value(settings_bl_slide,
                      (int)(Settings::g_backlight * 100.0f + 0.5f),
                      LV_ANIM_OFF);
  update_bl_label();

  if (Settings::g_udp_enabled) lv_obj_add_state(settings_udp_switch, LV_STATE_CHECKED);
  else                         lv_obj_remove_state(settings_udp_switch, LV_STATE_CHECKED);

  if (settings_led_switch) {
    if (Settings::g_led_enabled) lv_obj_add_state(settings_led_switch, LV_STATE_CHECKED);
    else                         lv_obj_remove_state(settings_led_switch, LV_STATE_CHECKED);
  }

  if (settings_rmw_switch) {
    if (Settings::g_radmon_watchdog) lv_obj_add_state(settings_rmw_switch, LV_STATE_CHECKED);
    else                             lv_obj_remove_state(settings_rmw_switch, LV_STATE_CHECKED);
  }

  if (settings_caw_switch) {
    if (Settings::g_cpm_alert_watchdog) lv_obj_add_state(settings_caw_switch, LV_STATE_CHECKED);
    else                                lv_obj_remove_state(settings_caw_switch, LV_STATE_CHECKED);
  }

  update_scsv_label();
  update_smax_label();

  if (settings_audio_slide) {
    lv_slider_set_value(settings_audio_slide,
                        (int)Settings::g_audio_volume, LV_ANIM_OFF);
  }
  if (settings_audio_lbl) {
    char buf[24];
    if (Settings::g_audio_volume == 0) snprintf(buf, sizeof(buf), "Audio off");
    else snprintf(buf, sizeof(buf), "Audio %d%%", (int)Settings::g_audio_volume);
    lv_label_set_text(settings_audio_lbl, buf);
  }

  if (settings_theme_lbl) {
    lv_label_set_text(settings_theme_lbl,
                      Theme::mode_name((Theme::Mode)Settings::g_theme_mode));
  }

  char buf[40];
  snprintf(buf, sizeof(buf), "%s : %u",
           Settings::g_multicast_addr, (unsigned)Settings::g_udp_port);
  lv_label_set_text(settings_udp_lbl, buf);

  if (settings_host_lbl) lv_label_set_text(settings_host_lbl, Settings::g_hostname);

  s_settings_refreshing = false;
}

// settings edit (multicast addr+port)

namespace {
lv_obj_t * settings_edit_title = nullptr;
lv_obj_t * settings_edit_input = nullptr;
lv_obj_t * settings_edit_hint  = nullptr;
lv_obj_t * settings_edit_kb    = nullptr;

enum class EditMode { Multicast, Hostname };
EditMode s_edit_mode = EditMode::Multicast;

// "a.b.c.d:port" -> Settings:: globals.
bool parse_multicast(const char * s) {
  unsigned a, b, c, d, port;
  if (sscanf(s, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) != 5) return false;
  if (a > 255 || b > 255 || c > 255 || d > 255) return false;
  if (port < 1 || port > 65535) return false;
  if (a < 224 || a > 239) {
    Serial.printf("[settings] non-multicast first octet %u - accepting\n", a);
  }
  snprintf(Settings::g_multicast_addr, Settings::kAddrLen, "%u.%u.%u.%u",
           a, b, c, d);
  Settings::g_udp_port = (uint16_t)port;
  return true;
}

// DNS rules: 1..31, [a-z0-9-], no edge dashes. Uppercase is downcased.
bool parse_hostname(const char * s) {
  const size_t n = strlen(s);
  if (n < 1 || n >= Settings::kHostnameLen) return false;
  if (s[0] == '-' || s[n - 1] == '-') return false;
  for (size_t i = 0; i < n; i++) {
    const char c = s[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-';
    if (!ok) return false;
  }
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    Settings::g_hostname[i] = c;
  }
  Settings::g_hostname[n] = '\0';
  return true;
}

void on_settings_edit_event(lv_event_t * e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    const char * v = lv_textarea_get_text(settings_edit_input);
    bool ok = false;
    if (s_edit_mode == EditMode::Multicast) {
      ok = parse_multicast(v);
    } else {
      ok = parse_hostname(v);
    }
    if (ok) {
      Settings::save();
      if (s_edit_mode == EditMode::Multicast) {
        Serial.printf("[settings] multicast saved: %s:%u\n",
                      Settings::g_multicast_addr,
                      (unsigned)Settings::g_udp_port);
      } else {
        Serial.printf("[settings] hostname saved: %s\n", Settings::g_hostname);
        // Re-publish mDNS.
        MDNS.end();
        MDNS.begin(Settings::g_hostname);
        MDNS.addService("http", "tcp", 80);
      }
      request_state_change(State::Settings);
    } else {
      // Hint red for retry.
      const char * msg = (s_edit_mode == EditMode::Multicast)
        ? "format: a.b.c.d:port (e.g. 239.255.42.42:57340)"
        : "a-z 0-9 dash, 1-31 chars, no leading/trailing dash";
      lv_label_set_text(settings_edit_hint, msg);
      lv_obj_set_style_text_color(settings_edit_hint, lv_color_hex(0xff6b6b), 0);
    }
  } else if (code == LV_EVENT_CANCEL) {
    request_state_change(State::Settings);
  }
}

void on_settings_edit_back(lv_event_t *) {
  request_state_change(State::Settings);
}
}  // anonymous namespace

void build_settings_edit() {
  if (scr_settings_edit) return;
  scr_settings_edit = lv_obj_create(nullptr);
  style_dark_screen(scr_settings_edit);

  lv_obj_t * back = lv_button_create(scr_settings_edit);
  lv_obj_set_size(back, 56, 32);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 4);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x2b3035), 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x373b3e), LV_STATE_PRESSED);
  lv_obj_set_style_radius(back, 6, 0);
  lv_obj_set_style_border_width(back, 0, 0);
  lv_obj_add_event_cb(back, on_settings_edit_back, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * back_lbl = lv_label_create(back);
  lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_font(back_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(back_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_center(back_lbl);

  settings_edit_title = lv_label_create(scr_settings_edit);
  lv_label_set_text(settings_edit_title, "");
  lv_obj_set_style_text_font(settings_edit_title, Theme::f_sub, 0);
  lv_obj_set_style_text_color(settings_edit_title, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(settings_edit_title, LV_ALIGN_TOP_LEFT, 70, 10);

  settings_edit_input = lv_textarea_create(scr_settings_edit);
  lv_obj_set_width(settings_edit_input, Board::SCREEN_W - 16);
  lv_textarea_set_one_line(settings_edit_input, true);
  lv_obj_set_style_text_font(settings_edit_input, Theme::f_body, 0);
  lv_obj_align(settings_edit_input, LV_ALIGN_TOP_MID, 0, 50);

  settings_edit_hint = lv_label_create(scr_settings_edit);
  lv_label_set_text(settings_edit_hint, "format: a.b.c.d:port");
  lv_obj_set_style_text_font(settings_edit_hint, Theme::f_muted, 0);
  lv_obj_set_style_text_color(settings_edit_hint, lv_color_hex(Theme::kMuted), 0);
  lv_obj_align(settings_edit_hint, LV_ALIGN_TOP_LEFT, 12, 88);

  // g_pending_settings_edit is set by the row tap.
  const Kb::Layout layout =
      (g_pending_settings_edit == SettingsEditTarget::Hostname)
        ? Kb::Layout::Hostname
        : Kb::Layout::Ip;
  settings_edit_kb = Kb::create(scr_settings_edit, settings_edit_input, layout);
  lv_obj_set_size(settings_edit_kb, Board::SCREEN_W, 200);
  lv_obj_align(settings_edit_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_event_cb(settings_edit_kb, on_settings_edit_event, LV_EVENT_ALL, nullptr);
}

void destroy_settings_edit() {
  if (!scr_settings_edit) return;
  lv_obj_delete(scr_settings_edit);
  scr_settings_edit   = nullptr;
  settings_edit_title = nullptr;
  settings_edit_input = nullptr;
  settings_edit_hint  = nullptr;
  settings_edit_kb    = nullptr;
}

void settings_edit_show_multicast() {
  if (!settings_edit_input) return;
  s_edit_mode = EditMode::Multicast;
  char buf[40];
  snprintf(buf, sizeof(buf), "%s:%u",
           Settings::g_multicast_addr, (unsigned)Settings::g_udp_port);
  lv_textarea_set_text(settings_edit_input, buf);
  lv_label_set_text(settings_edit_title, "Multicast addr:port");
  lv_label_set_text(settings_edit_hint, "format: a.b.c.d:port");
  lv_obj_set_style_text_color(settings_edit_hint, lv_color_hex(Theme::kMuted), 0);
}

void settings_edit_show_hostname() {
  if (!settings_edit_input) return;
  s_edit_mode = EditMode::Hostname;
  lv_textarea_set_text(settings_edit_input, Settings::g_hostname);
  lv_label_set_text(settings_edit_title, "Hostname");
  lv_label_set_text(settings_edit_hint, "a-z 0-9 dash; reached as <name>.local");
  lv_obj_set_style_text_color(settings_edit_hint, lv_color_hex(Theme::kMuted), 0);
}

// detail

namespace {
lv_obj_t * detail_title    = nullptr;
lv_obj_t * detail_cpm_lbl  = nullptr;
lv_obj_t * detail_usv_lbl  = nullptr;
lv_obj_t * detail_env_lbl  = nullptr;
lv_obj_t * detail_mirror_btn = nullptr;
lv_obj_t * detail_footer   = nullptr;

struct Spark {
  lv_obj_t *           chart  = nullptr;
  lv_chart_series_t *  series = nullptr;
  lv_obj_t *           label  = nullptr;
  lv_obj_t *           hi_lbl = nullptr;
  lv_obj_t *           lo_lbl = nullptr;
};
Spark detail_sp_cps;
Spark detail_sp_cpm;
Spark detail_sp_cph;

// Sparkline styling: no axes/grid, tight pad, transparent bg.
void style_sparkline(lv_obj_t * chart) {
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(chart, 0, 0);
  lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_CIRCULAR);
  lv_obj_set_style_bg_color(chart, lv_color_hex(0x1f2326), 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_radius(chart, 4, 0);
  lv_obj_set_style_pad_all(chart, 2, 0);
  lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
  lv_obj_remove_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
}

// unit_scale lets cps_60 plot as CPM (x60) on a shared Y axis.
// smooth_n > 1 plots a trailing N-slot mean (cps_60 needs it; raw 1s
// CPS Poisson noise spikes the line otherwise).
template<uint16_t MaxN>
void fill_sparkline(Spark & sp, const EGRingAvg<float, MaxN> & ring,
                    float unit_scale = 1.0f, uint16_t smooth_n = 1) {
  if (!sp.chart || !sp.series) return;
  const uint16_t count = ring.count();
  lv_chart_set_point_count(sp.chart, MaxN);
  lv_chart_set_all_value(sp.chart, sp.series, LV_CHART_POINT_NONE);

  // Right-anchor while the ring is filling.
  const uint16_t offset = (count < MaxN) ? (uint16_t)(MaxN - count) : 0;

  // Skip the first (smooth_n - 1) slots; their trailing window is truncated.
  const uint16_t plot_from = (smooth_n > 1 && count >= smooth_n)
                               ? (uint16_t)(smooth_n - 1) : 0;

  int32_t data_lo = INT32_MAX, data_hi = 0;
  for (uint16_t i = plot_from; i < count; i++) {
    float val;
    if (smooth_n <= 1) {
      val = ring.at(i);
    } else {
      // Trailing mean over the last smooth_n slots ending at i. Guaranteed
      // full window because i >= smooth_n - 1.
      const uint16_t start = (uint16_t)(i + 1 - smooth_n);
      float sum = 0;
      for (uint16_t j = start; j <= i; j++) sum += ring.at(j);
      val = sum / (float)smooth_n;
    }
    const int32_t v = (int32_t)(val * unit_scale + 0.5f);
    if (v < data_lo) data_lo = v;
    if (v > data_hi) data_hi = v;
    lv_chart_set_value_by_id(sp.chart, sp.series, offset + i, v);
  }
  int32_t lo, hi;
  if (count == 0) {
    lo = 0; hi = 1;
    data_lo = data_hi = 0;
  } else {
    lo = data_lo;
    hi = data_hi;
    if (lo > 0)   lo = lo - (lo / 8 + 1);
    if (lo < 0)   lo = 0;
    if (hi == lo) hi = lo + 1;
    else          hi = hi + (hi - lo) / 8 + 1;
  }
  lv_chart_set_range(sp.chart, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
  lv_chart_refresh(sp.chart);

  // Gutter labels track the raw window min/max, not the padded range.
  // "-" on an empty window to avoid a misleading "0".
  if (sp.hi_lbl && sp.lo_lbl) {
    char b[8];
    if (count == 0) {
      lv_label_set_text(sp.hi_lbl, "-");
      lv_label_set_text(sp.lo_lbl, "-");
    } else {
      snprintf(b, sizeof(b), "%ld", (long)data_hi);
      lv_label_set_text(sp.hi_lbl, b);
      snprintf(b, sizeof(b), "%ld", (long)data_lo);
      lv_label_set_text(sp.lo_lbl, b);
    }
  }
}

// Build one Spark row: chart on the left, right-anchored caption + hi/lo.
void build_sparkline(lv_obj_t * parent, Spark & sp,
                     const char * caption, uint32_t colour, int y_off,
                     uint16_t point_count) {
  const int32_t chart_w = Board::SCREEN_W - 60;
  const int32_t chart_h = 40;
  sp.chart = lv_chart_create(parent);
  lv_obj_set_size(sp.chart, chart_w, chart_h);
  lv_obj_align(sp.chart, LV_ALIGN_TOP_LEFT, 8, y_off);
  style_sparkline(sp.chart);
  lv_chart_set_point_count(sp.chart, point_count);
  sp.series = lv_chart_add_series(sp.chart, lv_color_hex(colour),
                                  LV_CHART_AXIS_PRIMARY_Y);

  // Right gutter: max top, caption middle, min bottom. All f_muted so
  // the column reads as one column; hi/lo in kFg, caption in kMuted.
  sp.hi_lbl = lv_label_create(parent);
  lv_obj_set_style_text_font(sp.hi_lbl, Theme::f_muted, 0);
  lv_obj_set_style_text_color(sp.hi_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(sp.hi_lbl, LV_ALIGN_TOP_RIGHT, -6, y_off + 1);
  lv_label_set_text(sp.hi_lbl, "");

  sp.label = lv_label_create(parent);
  lv_label_set_text(sp.label, caption);
  lv_obj_set_style_text_font(sp.label, Theme::f_muted, 0);
  lv_obj_set_style_text_color(sp.label, lv_color_hex(Theme::kMuted), 0);
  lv_obj_align(sp.label, LV_ALIGN_TOP_RIGHT, -6, y_off + 14);

  sp.lo_lbl = lv_label_create(parent);
  lv_obj_set_style_text_font(sp.lo_lbl, Theme::f_muted, 0);
  lv_obj_set_style_text_color(sp.lo_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(sp.lo_lbl, LV_ALIGN_TOP_RIGHT, -6, y_off + 27);
  lv_label_set_text(sp.lo_lbl, "");
}

// Format uptime as 1d 02h / 2h 15m / 17m / 12s.
void format_uptime(uint32_t s, char * buf, size_t cap) {
  if      (s < 60)    snprintf(buf, cap, "%us", (unsigned)s);
  else if (s < 3600)  snprintf(buf, cap, "%um", (unsigned)(s / 60));
  else if (s < 86400) snprintf(buf, cap, "%uh %02um",
                                (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
  else                snprintf(buf, cap, "%ud %02uh",
                                (unsigned)(s / 86400), (unsigned)((s % 86400) / 3600));
}
}  // anonymous namespace

void build_detail() {
  if (scr_detail) return;
  scr_detail = lv_obj_create(nullptr);
  style_dark_screen(scr_detail);

  // Back button
  lv_obj_t * back = lv_button_create(scr_detail);
  lv_obj_set_size(back, 52, 30);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 4);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x2b3035), 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x373b3e), LV_STATE_PRESSED);
  lv_obj_set_style_radius(back, 6, 0);
  lv_obj_set_style_border_width(back, 0, 0);
  lv_obj_add_event_cb(back, on_detail_back, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * back_lbl = lv_label_create(back);
  lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_font(back_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(back_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_center(back_lbl);

  // f_body (16) instead of f_sub (24); long hostnames ticker-scroll in
  // LVGL 9 because DOTS still wraps when the width is tight.
  detail_title = lv_label_create(scr_detail);
  lv_obj_set_style_text_font(detail_title, Theme::f_body, 0);
  lv_obj_set_style_text_color(detail_title, lv_color_hex(Theme::kFg), 0);
  lv_obj_set_width(detail_title, Board::SCREEN_W - 72);
  lv_label_set_long_mode(detail_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
  lv_obj_align(detail_title, LV_ALIGN_TOP_LEFT, 62, 13);

  // Big CPM + uSv subtitle
  detail_cpm_lbl = lv_label_create(scr_detail);
  lv_obj_set_style_text_font(detail_cpm_lbl, Theme::f_title, 0);
  lv_obj_set_style_text_color(detail_cpm_lbl, lv_color_hex(Theme::kAccent), 0);
  lv_obj_align(detail_cpm_lbl, LV_ALIGN_TOP_MID, 0, 42);

  detail_usv_lbl = lv_label_create(scr_detail);
  lv_obj_set_style_text_font(detail_usv_lbl, Theme::f_muted, 0);
  lv_obj_set_style_text_color(detail_usv_lbl, lv_color_hex(Theme::kMuted), 0);
  lv_obj_align(detail_usv_lbl, LV_ALIGN_TOP_MID, 0, 78);

  // Three sparklines stacked
  build_sparkline(scr_detail, detail_sp_cps, "60s",
                  Theme::kAccentYellow, 104, Stations::kCps60);
  build_sparkline(scr_detail, detail_sp_cpm, "60m",
                  Theme::kAccent,       150, Stations::kCpm60);
  build_sparkline(scr_detail, detail_sp_cph, "24h",
                  0x9090a0,             196, Stations::kCph24);

  // Hidden until the Poller's probe sets F2_HAS_OLED.
  detail_mirror_btn = lv_button_create(scr_detail);
  lv_obj_set_size(detail_mirror_btn, 36, 26);
  lv_obj_align(detail_mirror_btn, LV_ALIGN_TOP_LEFT, 12, 244);
  lv_obj_set_style_bg_color(detail_mirror_btn, lv_color_hex(Theme::kRowBg), 0);
  lv_obj_set_style_bg_color(detail_mirror_btn, lv_color_hex(Theme::kRowBgPressed),
                             LV_STATE_PRESSED);
  lv_obj_set_style_radius(detail_mirror_btn, 6, 0);
  lv_obj_set_style_border_width(detail_mirror_btn, 0, 0);
  lv_obj_add_event_cb(detail_mirror_btn, on_open_mirror, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(detail_mirror_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t * mirror_lbl = lv_label_create(detail_mirror_btn);
  lv_label_set_text(mirror_lbl, LV_SYMBOL_EYE_OPEN);
  lv_obj_set_style_text_font(mirror_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(mirror_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_center(mirror_lbl);

  // Env + HV row above the footer. Hidden by empty-string content when
  // the parent didn't publish any of t/h/p/hv.
  detail_env_lbl = lv_label_create(scr_detail);
  lv_obj_set_style_text_font(detail_env_lbl, Theme::f_muted, 0);
  lv_obj_set_style_text_color(detail_env_lbl, lv_color_hex(Theme::kMuted), 0);
  lv_obj_set_width(detail_env_lbl, Board::SCREEN_W - 16);
  lv_obj_set_style_text_align(detail_env_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(detail_env_lbl, LV_ALIGN_BOTTOM_MID, 0, -26);
  lv_label_set_text(detail_env_lbl, "");


  // Footer - RSSI / uptime / age / UDP marker
  detail_footer = lv_label_create(scr_detail);
  lv_obj_set_style_text_font(detail_footer, Theme::f_muted, 0);
  lv_obj_set_style_text_color(detail_footer, lv_color_hex(Theme::kMuted), 0);
  lv_obj_set_width(detail_footer, Board::SCREEN_W - 16);
  lv_obj_set_style_text_align(detail_footer, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(detail_footer, LV_ALIGN_BOTTOM_MID, 0, -6);
}

void destroy_detail() {
  if (!scr_detail) return;
  lv_obj_delete(scr_detail);
  scr_detail        = nullptr;
  detail_title      = nullptr;
  detail_cpm_lbl    = nullptr;
  detail_usv_lbl    = nullptr;
  detail_env_lbl    = nullptr;
  detail_mirror_btn = nullptr;
  detail_footer     = nullptr;
  detail_sp_cps     = {};
  detail_sp_cpm     = {};
  detail_sp_cph     = {};
}

void detail_refresh() {
  if (!scr_detail) return;
  const int idx = g_detail_idx;
  if (idx < 0) return;
  if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(200)) != pdTRUE) return;
  if (idx >= (int)Stations::g_count) {
    xSemaphoreGive(Stations::g_mux);
    return;
  }
  const Stations::Station & s = Stations::g_stations[idx];
  const uint32_t now = fast_millis();

  lv_label_set_text(detail_title, Stations::label(s));

  // Big CPM. Priority is FAULT > SAT > Poisson z-score vs the last 5
  // minutes of cpm_60.
  char cpm[16];
  uint32_t cpm_col = Theme::kAccent;
  if (Stations::is_faulted(s)) {
    snprintf(cpm, sizeof(cpm), "FAULT");
    cpm_col = 0xff5555;
  } else {
    snprintf(cpm, sizeof(cpm), "%d CPM", (int)(s.cpm_now + 0.5f));
    if (Stations::is_saturated(s)) {
      cpm_col = Theme::kAccentYellow;
    } else {
      const uint16_t cnt = s.cpm_60.count();
      if (cnt >= 5) {
        float baseline = 0.0f;
        for (uint16_t i = (uint16_t)(cnt - 5); i < cnt; i++) baseline += s.cpm_60.at(i);
        baseline *= 0.2f;   // / 5
        if (baseline > 0.5f) {
          const float z = (s.cpm_now - baseline) / sqrtf(baseline);
          if      (z >  3.0f) cpm_col = 0xff5555;             // significant rise
          else if (z >  1.5f) cpm_col = Theme::kAccentYellow; // rising
          else if (z < -1.5f) cpm_col = 0xc060ff;             // dropping
          // else: stable, keep accent
        }
      }
    }
  }
  lv_label_set_text(detail_cpm_lbl, cpm);
  lv_obj_set_style_text_color(detail_cpm_lbl, lv_color_hex(cpm_col), 0);

  // uSv/h - the bundled Montserrat fonts lack U+00B5 (would render as a
  // hollow box otherwise).
  char usv[20];
  snprintf(usv, sizeof(usv), "%.2f uSv/h", (double)s.usv_per_hour);
  lv_label_set_text(detail_usv_lbl, usv);

  // Mirror button visibility - the Poller probes /screen.bin once on the
  // first successful /json for each station; this just reflects the flag.
  if (detail_mirror_btn) {
    if (s.flags2 & Stations::F2_HAS_OLED) {
      lv_obj_remove_flag(detail_mirror_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(detail_mirror_btn, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // cps_60 plots in CPM-equivalent (x60); 10 s trailing window stops
  // Poisson clumping from spiking the line on a low-rate source.
  fill_sparkline<Stations::kCps60>(detail_sp_cps, s.cps_60, 60.0f, 10);
  fill_sparkline<Stations::kCpm60>(detail_sp_cpm, s.cpm_60);
  fill_sparkline<Stations::kCph24>(detail_sp_cph, s.cph_24);

  // Bitmap fonts lack U+00B0 and U+00B5 so all units stay ASCII.
  char env_buf[64];
  size_t en = 0;
  env_buf[0] = '\0';
  if (s.flags & Stations::F_HAS_ENV) {
    if (!isnanf(s.env_temp)) {
      const char u = (s.env_temp_unit == 1) ? 'F' : (s.env_temp_unit == 2) ? 'K' : 'C';
      en += snprintf(env_buf + en, sizeof(env_buf) - en, "%.1f%c", (double)s.env_temp, u);
    }
    if (!isnanf(s.env_humid) && en < sizeof(env_buf) - 1) {
      en += snprintf(env_buf + en, sizeof(env_buf) - en, "%s%.0f%%RH",
                     en ? "  " : "", (double)s.env_humid);
    }
    if (!isnanf(s.env_pressure) && en < sizeof(env_buf) - 1) {
      en += snprintf(env_buf + en, sizeof(env_buf) - en, "%s%.0fhPa",
                     en ? "  " : "", (double)s.env_pressure);
    }
  }
  if ((s.flags & Stations::F_HAS_HV) && !isnanf(s.hv_voltage) &&
      en < sizeof(env_buf) - 1) {
    en += snprintf(env_buf + en, sizeof(env_buf) - en, "%sHV %.0fV",
                   en ? "  " : "", (double)s.hv_voltage);
  }
  lv_label_set_text(detail_env_lbl, env_buf);

  // Age uses the timer that actually reflects the displayed number:
  // last_click_at_ms for UDP stations, last_poll_ok_at_ms for poll-only.
  char up_buf[20];
  format_uptime(s.uptime_s, up_buf, sizeof(up_buf));
  const char * tag = Stations::is_faulted(s)   ? " TUBE" :
                     Stations::is_saturated(s) ? " SAT"  : "";
  char footer[96];
  if (Stations::is_udp_active(s)) {
    const uint32_t age_s = s.last_click_at_ms
                            ? (now - s.last_click_at_ms) / 1000 : 0;
    snprintf(footer, sizeof(footer), "RSSI %d  up %s  UDP %lus%s",
             (int)s.rssi, up_buf, (unsigned long)age_s, tag);
  } else {
    const uint32_t age_s = s.last_poll_ok_at_ms
                            ? (now - s.last_poll_ok_at_ms) / 1000 : 0;
    snprintf(footer, sizeof(footer), "RSSI %d  up %s  poll %lus%s",
             (int)s.rssi, up_buf, (unsigned long)age_s, tag);
  }
  lv_label_set_text(detail_footer, footer);

  xSemaphoreGive(Stations::g_mux);
}

// dice mini-app

namespace {
lv_obj_t   * scr_dice         = nullptr;
lv_obj_t   * dice_value_lbl   = nullptr;
lv_obj_t   * dice_type_lbl    = nullptr;
lv_timer_t * dice_anim_timer  = nullptr;
uint8_t      s_dice_frame     = 0;
uint32_t     s_dice_final_seed = 0;
constexpr uint8_t kDiceAnimFrames    = 12;
constexpr uint8_t kDiceTypes[]       = { 4, 6, 8, 10, 12, 20 };
constexpr size_t  kDiceTypesN        = sizeof(kDiceTypes) / sizeof(kDiceTypes[0]);

// Poisson click timings + micros + esp_random.
uint32_t radiation_seed() {
  uint32_t v = (uint32_t)micros() ^ esp_random();
  if (Stations::g_mux &&
      xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (size_t i = 0; i < Stations::g_count; i++) {
      const Stations::Station & s = Stations::g_stations[i];
      if (s.flags & Stations::F_UDP_ANNOUNCED) {
        v ^= s.last_click_ts_ms ^ (s.total_clicks << 16);
      }
    }
    xSemaphoreGive(Stations::g_mux);
  }
  return v;
}

void dice_show_value(uint8_t v) {
  if (!dice_value_lbl) return;
  char buf[6];
  snprintf(buf, sizeof(buf), "%u", (unsigned)v);
  lv_label_set_text(dice_value_lbl, buf);
}

void dice_update_type_label() {
  if (!dice_type_lbl) return;
  char buf[8];
  snprintf(buf, sizeof(buf), "d%u", (unsigned)Settings::g_dice_sides);
  lv_label_set_text(dice_type_lbl, buf);
}

void dice_anim_cb(lv_timer_t * t) {
  s_dice_frame++;
  if (s_dice_frame >= kDiceAnimFrames) {
    // Last frame: radiation-seeded value wins.
    const uint8_t v = (uint8_t)((s_dice_final_seed % Settings::g_dice_sides) + 1);
    dice_show_value(v);
    lv_timer_del(t);
    dice_anim_timer = nullptr;
    Audio::click();
    return;
  }
  const uint8_t v = (uint8_t)((esp_random() % Settings::g_dice_sides) + 1);
  dice_show_value(v);
  // Ease-out: each frame's period grows by ~16 ms so the cycle visibly
  // slows towards the final value. Total ~700 ms.
  lv_timer_set_period(t, 40 + 16 * s_dice_frame);
}

void dice_start_roll() {
  if (dice_anim_timer) {
    lv_timer_del(dice_anim_timer);
    dice_anim_timer = nullptr;
  }
  s_dice_frame = 0;
  s_dice_final_seed = radiation_seed();
  dice_anim_timer = lv_timer_create(dice_anim_cb, 40, nullptr);
}

void on_dice_back(lv_event_t *)        { request_state_change(State::Settings); }
void on_dice_roll(lv_event_t *)        { dice_start_roll(); }
void on_dice_cycle_type(lv_event_t *) {
  size_t cur = 0;
  for (size_t i = 0; i < kDiceTypesN; i++) {
    if (kDiceTypes[i] == Settings::g_dice_sides) { cur = i; break; }
  }
  cur = (cur + 1) % kDiceTypesN;
  Settings::g_dice_sides = kDiceTypes[cur];
  // Not persisted: cycling is rapid and would hammer NVS.
  dice_update_type_label();
  dice_start_roll();
}
}  // anonymous namespace

void build_dice() {
  if (scr_dice) return;
  scr_dice = lv_obj_create(nullptr);
  style_dark_screen(scr_dice);

  // Back
  lv_obj_t * back = lv_button_create(scr_dice);
  lv_obj_set_size(back, 52, 32);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 4);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x2b3035), 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x373b3e), LV_STATE_PRESSED);
  lv_obj_set_style_radius(back, 6, 0);
  lv_obj_set_style_border_width(back, 0, 0);
  lv_obj_add_event_cb(back, on_dice_back, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * back_lbl = lv_label_create(back);
  lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_font(back_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(back_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_center(back_lbl);

  // Title
  lv_obj_t * title = lv_label_create(scr_dice);
  lv_label_set_text(title, "Dice");
  lv_obj_set_style_text_font(title, Theme::f_sub, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

  dice_value_lbl = lv_label_create(scr_dice);
  lv_obj_set_style_text_font(dice_value_lbl, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(dice_value_lbl, lv_color_hex(Theme::kAccent), 0);
  lv_obj_align(dice_value_lbl, LV_ALIGN_CENTER, 0, -36);
  lv_label_set_text(dice_value_lbl, "-");

  // Die type label below the value
  dice_type_lbl = lv_label_create(scr_dice);
  lv_obj_set_style_text_font(dice_type_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(dice_type_lbl, lv_color_hex(Theme::kMuted), 0);
  lv_obj_align(dice_type_lbl, LV_ALIGN_CENTER, 0, 8);
  dice_update_type_label();

  // Roll button (primary)
  lv_obj_t * roll = lv_button_create(scr_dice);
  lv_obj_set_size(roll, 160, 44);
  lv_obj_align(roll, LV_ALIGN_BOTTOM_MID, 0, -64);
  lv_obj_set_style_bg_color(roll, lv_color_hex(Theme::kAccent), 0);
  lv_obj_set_style_bg_color(roll, lv_color_hex(0x3380dd), LV_STATE_PRESSED);
  lv_obj_set_style_radius(roll, 10, 0);
  lv_obj_set_style_border_width(roll, 0, 0);
  lv_obj_add_event_cb(roll, on_dice_roll, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * roll_lbl = lv_label_create(roll);
  lv_label_set_text(roll_lbl, "Roll");
  lv_obj_set_style_text_font(roll_lbl, Theme::f_sub, 0);
  lv_obj_set_style_text_color(roll_lbl, lv_color_hex(0xffffff), 0);
  lv_obj_center(roll_lbl);

  // Change-die button (secondary)
  lv_obj_t * cycle = lv_button_create(scr_dice);
  lv_obj_set_size(cycle, 160, 32);
  lv_obj_align(cycle, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_obj_set_style_bg_color(cycle, lv_color_hex(Theme::kRowBg), 0);
  lv_obj_set_style_bg_color(cycle, lv_color_hex(Theme::kRowBgPressed), LV_STATE_PRESSED);
  lv_obj_set_style_radius(cycle, 6, 0);
  lv_obj_set_style_border_width(cycle, 0, 0);
  lv_obj_add_event_cb(cycle, on_dice_cycle_type, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * cycle_lbl = lv_label_create(cycle);
  lv_label_set_text(cycle_lbl, "Change die");
  lv_obj_set_style_text_font(cycle_lbl, Theme::f_muted, 0);
  lv_obj_set_style_text_color(cycle_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_center(cycle_lbl);
}

void destroy_dice() {
  if (dice_anim_timer) {
    lv_timer_del(dice_anim_timer);
    dice_anim_timer = nullptr;
  }
  if (!scr_dice) return;
  lv_obj_delete(scr_dice);
  scr_dice       = nullptr;
  dice_value_lbl = nullptr;
  dice_type_lbl  = nullptr;
}

void dice_enter() {
  if (!scr_dice) build_dice();
  dice_update_type_label();
  dice_start_roll();   // land already rolling - feels alive
  lv_screen_load(scr_dice);
}

void dice_leave() {
  if (dice_anim_timer) {
    lv_timer_del(dice_anim_timer);
    dice_anim_timer = nullptr;
  }
}

// remote OLED mirror

namespace {
lv_obj_t   * scr_remote        = nullptr;
lv_obj_t   * remote_canvas     = nullptr;
lv_obj_t   * remote_status     = nullptr;
lv_obj_t   * remote_interval_lbl = nullptr;
lv_obj_t   * remote_interval_slide = nullptr;
lv_timer_t * remote_timer      = nullptr;

constexpr uint16_t kRemoteW = 128;
constexpr uint16_t kRemoteH = 64;
// I1 canvas: 8 palette bytes + row-major 1bpp.
constexpr size_t   kRemoteBufBytes = (kRemoteW * kRemoteH / 8) + 8;
static uint8_t s_remote_canvas_buf[kRemoteBufBytes];
constexpr uint16_t kSsdW = 128;
constexpr uint16_t kSsdH = 64;

constexpr uint32_t kRemoteIntervalMinMs = 200;
constexpr uint32_t kRemoteIntervalMaxMs = 2000;

// Fetch on a core-0 task, transpose+invalidate on the LVGL thread.
TaskHandle_t      s_mirror_task        = nullptr;
SemaphoreHandle_t s_mirror_mux         = nullptr;
volatile bool     s_mirror_active      = false;
volatile uint32_t s_mirror_interval_ms = 1000;
uint8_t           s_mirror_shadow[1024];
bool              s_mirror_dirty       = false;
char              s_mirror_status_text[24] = {0};
uint32_t          s_mirror_status_colour   = 0;
bool              s_mirror_status_dirty    = false;

// SSD1306 page mode -> row-major 1bpp MSB-first.
void transpose_ssd_to_canvas(const uint8_t * src, uint8_t * dst) {
  uint8_t * const bitmap = dst + 8;   // skip palette
  memset(bitmap, 0, kRemoteW * kRemoteH / 8);
  const uint16_t row_bytes = kRemoteW / 8;
  for (uint8_t page = 0; page < 8; page++) {
    for (uint16_t col = 0; col < kSsdW; col++) {
      const uint8_t b = src[page * 128 + col];
      if (b == 0) continue;
      for (uint8_t bit = 0; bit < 8; bit++) {
        if (!((b >> bit) & 0x01)) continue;
        const uint16_t y = page * 8 + bit;
        bitmap[y * row_bytes + (col >> 3)] |= (uint8_t)(0x80 >> (col & 7));
      }
    }
  }
}

struct Phosphor { uint32_t fg; uint32_t bg; const char * name; };
constexpr Phosphor kPhosphors[4] = {
  { 0x80D0FFu, 0x101418u, "cyan"  },
  { 0x33FF66u, 0x001008u, "green" },
  { 0xFFAA00u, 0x180A00u, "amber" },
  { 0xFFFFFFu, 0x000000u, "white" },
};

lv_obj_t * remote_swatch_btns[4] = { nullptr, nullptr, nullptr, nullptr };

void remote_apply_palette(uint8_t idx) {
  if (idx > 3) idx = 0;
  if (!remote_canvas) return;
  const Phosphor & p = kPhosphors[idx];
  lv_canvas_set_palette(remote_canvas, 0,
    lv_color32_make((uint8_t)((p.bg >> 16) & 0xff),
                    (uint8_t)((p.bg >>  8) & 0xff),
                    (uint8_t)( p.bg        & 0xff), 0xff));
  lv_canvas_set_palette(remote_canvas, 1,
    lv_color32_make((uint8_t)((p.fg >> 16) & 0xff),
                    (uint8_t)((p.fg >>  8) & 0xff),
                    (uint8_t)( p.fg        & 0xff), 0xff));
  for (uint8_t i = 0; i < 4; i++) {
    if (!remote_swatch_btns[i]) continue;
    lv_obj_set_style_border_width(remote_swatch_btns[i],
                                  (i == idx) ? 2 : 0, 0);
    lv_obj_set_style_border_color(remote_swatch_btns[i],
                                  lv_color_hex(0xffffff), 0);
  }
  lv_obj_invalidate(remote_canvas);
}

void on_phosphor_pick(lv_event_t * e) {
  lv_obj_t * b = static_cast<lv_obj_t *>(lv_event_get_target(e));
  const uint8_t idx = (uint8_t)(uintptr_t)lv_obj_get_user_data(b);
  Settings::g_remote_phosphor = idx;
  Settings::save();
  remote_apply_palette(idx);
}

void remote_show_status(const char * msg, uint32_t colour) {
  if (!remote_status) return;
  lv_label_set_text(remote_status, msg);
  lv_obj_set_style_text_color(remote_status, lv_color_hex(colour), 0);
}

// Status update from the background task; LVGL applies on next tick.
void mirror_post_status(const char * msg, uint32_t colour) {
  if (!s_mirror_mux) return;
  if (xSemaphoreTake(s_mirror_mux, pdMS_TO_TICKS(20)) != pdTRUE) return;
  strncpy(s_mirror_status_text, msg, sizeof(s_mirror_status_text) - 1);
  s_mirror_status_text[sizeof(s_mirror_status_text) - 1] = '\0';
  s_mirror_status_colour = colour;
  s_mirror_status_dirty  = true;
  xSemaphoreGive(s_mirror_mux);
}

// Fetch loop; notify wakes it for the first fetch on enter.
void mirror_task_body(void * /*arg*/) {
  for (;;) {
    if (!s_mirror_active || WiFi.status() != WL_CONNECTED) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
      continue;
    }
    const int idx = g_detail_idx;
    if (idx < 0) {
      mirror_post_status("no station", Theme::kAccentYellow);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    IPAddress ip(0, 0, 0, 0);
    uint16_t  port = 0;
    if (Stations::g_mux &&
        xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (idx < (int)Stations::g_count) {
        ip   = Stations::g_stations[idx].ip;
        port = Stations::g_stations[idx].http_port;
      }
      xSemaphoreGive(Stations::g_mux);
    }
    if (ip == IPAddress(0,0,0,0) || port == 0) {
      mirror_post_status("no IP", Theme::kAccentYellow);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    char url[80];
    snprintf(url, sizeof(url), "http://%u.%u.%u.%u:%u/screen.bin",
             ip[0], ip[1], ip[2], ip[3], port);

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(1000);
    http.setTimeout(1000);
    int code = -1;
    if (http.begin(client, url)) code = http.GET();
    if (code == 200) {
      // /screen.bin is chunked. Dechunk in place; otherwise the size
      // header bleeds into the framebuffer.
      uint8_t raw[1100];
      int got = http.getStream().readBytes(raw, sizeof(raw));
      size_t in = 0, out = 0;
      for (;;) {
        size_t sz = 0;
        bool hex_seen = false;
        while (in < (size_t)got) {
          const char c = (char)raw[in];
          if (c == '\r' || c == '\n') {
            while (in < (size_t)got && (raw[in] == '\r' || raw[in] == '\n')) in++;
            break;
          }
          if      (c >= '0' && c <= '9') { sz = sz * 16 + (c - '0');      hex_seen = true; in++; }
          else if (c >= 'a' && c <= 'f') { sz = sz * 16 + 10 + (c - 'a'); hex_seen = true; in++; }
          else if (c >= 'A' && c <= 'F') { sz = sz * 16 + 10 + (c - 'A'); hex_seen = true; in++; }
          else                           { in++; }
        }
        if (!hex_seen || sz == 0) break;
        for (size_t j = 0; j < sz && in < (size_t)got; j++) raw[out++] = raw[in++];
        while (in < (size_t)got && (raw[in] == '\r' || raw[in] == '\n')) in++;
      }
      if (out == 1024) {
        if (xSemaphoreTake(s_mirror_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
          memcpy(s_mirror_shadow, raw, 1024);
          s_mirror_dirty = true;
          xSemaphoreGive(s_mirror_mux);
        }
        mirror_post_status("", Theme::kMuted);
      } else {
        char b[24];
        snprintf(b, sizeof(b), "short read %u", (unsigned)out);
        mirror_post_status(b, Theme::kAccentYellow);
      }
    } else if (code == 404) {
      mirror_post_status("not on this build", Theme::kAccentYellow);
    } else if (code < 0) {
      mirror_post_status("connect failed", 0xff5555);
    } else {
      char buf[24];
      snprintf(buf, sizeof(buf), "HTTP %d", code);
      mirror_post_status(buf, Theme::kAccentYellow);
    }
    http.end();

    // Chunked sleep so interval / exit take effect within ~50 ms.
    const uint32_t target = s_mirror_interval_ms;
    uint32_t slept = 0;
    while (slept < target && s_mirror_active) {
      uint32_t chunk = target - slept;
      if (chunk > 50) chunk = 50;
      vTaskDelay(pdMS_TO_TICKS(chunk));
      slept += chunk;
    }
  }
}

void mirror_start_task() {
  if (s_mirror_task) return;
  if (!s_mirror_mux) s_mirror_mux = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(mirror_task_body, "mirror", 4096, nullptr, 1,
                          &s_mirror_task, 0);
}

// LVGL-thread tick: shadow -> canvas + status flush. ~30 Hz.
void remote_timer_cb(lv_timer_t * /*t*/) {
  bool was_dirty    = false;
  bool status_dirty = false;
  char     local_status[24] = {0};
  uint32_t local_colour = 0;
  if (s_mirror_mux &&
      xSemaphoreTake(s_mirror_mux, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (s_mirror_dirty) {
      transpose_ssd_to_canvas(s_mirror_shadow, s_remote_canvas_buf);
      s_mirror_dirty = false;
      was_dirty = true;
    }
    if (s_mirror_status_dirty) {
      strncpy(local_status, s_mirror_status_text, sizeof(local_status) - 1);
      local_colour = s_mirror_status_colour;
      s_mirror_status_dirty = false;
      status_dirty = true;
    }
    xSemaphoreGive(s_mirror_mux);
  }
  if (was_dirty && remote_canvas) lv_obj_invalidate(remote_canvas);
  if (status_dirty)               remote_show_status(local_status, local_colour);
}

void on_remote_interval_change(lv_event_t * e) {
  lv_obj_t * s = static_cast<lv_obj_t *>(lv_event_get_target(e));
  int v = lv_slider_get_value(s);
  if (v < (int)kRemoteIntervalMinMs) v = (int)kRemoteIntervalMinMs;
  if (v > (int)kRemoteIntervalMaxMs) v = (int)kRemoteIntervalMaxMs;
  Settings::g_remote_interval_ms = (uint32_t)v;
  s_mirror_interval_ms           = (uint32_t)v;
  if (remote_interval_lbl) {
    char b[24];
    snprintf(b, sizeof(b), "Refresh %.1fs", v / 1000.0f);
    lv_label_set_text(remote_interval_lbl, b);
  }
  // Session-only; NVS wear isn't worth a slider drag.
}
}  // anonymous namespace

void build_remote_screen() {
  if (scr_remote) return;
  scr_remote = lv_obj_create(nullptr);
  style_dark_screen(scr_remote);

  // Back
  lv_obj_t * back = lv_button_create(scr_remote);
  lv_obj_set_size(back, 52, 32);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 4);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x2b3035), 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x373b3e), LV_STATE_PRESSED);
  lv_obj_set_style_radius(back, 6, 0);
  lv_obj_set_style_border_width(back, 0, 0);
  lv_obj_add_event_cb(back, on_remote_back, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * back_lbl = lv_label_create(back);
  lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_font(back_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(back_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_center(back_lbl);

  // Title
  lv_obj_t * title = lv_label_create(scr_remote);
  lv_label_set_text(title, "Mirror");
  lv_obj_set_style_text_font(title, Theme::f_sub, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

  // 1bpp canvas. Palette via remote_apply_palette().
  remote_canvas = lv_canvas_create(scr_remote);
  lv_canvas_set_buffer(remote_canvas, s_remote_canvas_buf,
                       kRemoteW, kRemoteH, LV_COLOR_FORMAT_I1);
  lv_obj_align(remote_canvas, LV_ALIGN_TOP_MID, 0, 56);

  remote_status = lv_label_create(scr_remote);
  lv_obj_set_style_text_font(remote_status, Theme::f_muted, 0);
  lv_obj_align(remote_status, LV_ALIGN_TOP_MID, 0, 128);
  lv_label_set_text(remote_status, "");

  remote_interval_lbl = lv_label_create(scr_remote);
  lv_obj_set_style_text_font(remote_interval_lbl, Theme::f_muted, 0);
  lv_obj_set_style_text_color(remote_interval_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(remote_interval_lbl, LV_ALIGN_TOP_LEFT, 12, 156);

  remote_interval_slide = lv_slider_create(scr_remote);
  lv_obj_set_width(remote_interval_slide, Board::SCREEN_W - 24);
  lv_obj_align(remote_interval_slide, LV_ALIGN_TOP_LEFT, 12, 184);
  lv_slider_set_range(remote_interval_slide,
                      (int)kRemoteIntervalMinMs, (int)kRemoteIntervalMaxMs);
  lv_slider_set_value(remote_interval_slide,
                      (int)Settings::g_remote_interval_ms, LV_ANIM_OFF);
  style_slider(remote_interval_slide);
  lv_obj_add_event_cb(remote_interval_slide, on_remote_interval_change,
                      LV_EVENT_VALUE_CHANGED, nullptr);

  // Phosphor swatches. Tap to apply + persist.
  constexpr int sw_w = 40, sw_h = 28, sw_gap = 8;
  constexpr int sw_row_w = 4 * sw_w + 3 * sw_gap;
  const int sw_x0 = (Board::SCREEN_W - sw_row_w) / 2;
  for (uint8_t i = 0; i < 4; i++) {
    lv_obj_t * b = lv_button_create(scr_remote);
    lv_obj_set_size(b, sw_w, sw_h);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, sw_x0 + i * (sw_w + sw_gap), 220);
    lv_obj_set_style_bg_color(b, lv_color_hex(kPhosphors[i].fg), 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_user_data(b, (void *)(uintptr_t)i);
    lv_obj_add_event_cb(b, on_phosphor_pick, LV_EVENT_CLICKED, nullptr);
    remote_swatch_btns[i] = b;
  }

  lv_obj_t * tap = lv_button_create(scr_remote);
  lv_obj_set_size(tap, 180, 40);
  lv_obj_align(tap, LV_ALIGN_BOTTOM_MID, 0, -16);
  lv_obj_set_style_bg_color(tap, lv_color_hex(Theme::kAccent), 0);
  lv_obj_set_style_bg_color(tap, lv_color_hex(0x3380dd), LV_STATE_PRESSED);
  lv_obj_set_style_radius(tap, 8, 0);
  lv_obj_set_style_border_width(tap, 0, 0);
  lv_obj_add_event_cb(tap, on_remote_tap, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * tap_lbl = lv_label_create(tap);
  lv_label_set_text(tap_lbl, "Cycle remote page");
  lv_obj_set_style_text_font(tap_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(tap_lbl, lv_color_hex(0xffffff), 0);
  lv_obj_center(tap_lbl);

  char b[24];
  snprintf(b, sizeof(b), "Refresh %.1fs", Settings::g_remote_interval_ms / 1000.0f);
  lv_label_set_text(remote_interval_lbl, b);

  remote_apply_palette(Settings::g_remote_phosphor);
}

void destroy_remote_screen() {
  if (remote_timer) {
    lv_timer_del(remote_timer);
    remote_timer = nullptr;
  }
  if (!scr_remote) return;
  lv_obj_delete(scr_remote);
  scr_remote            = nullptr;
  remote_canvas         = nullptr;
  remote_status         = nullptr;
  remote_interval_lbl   = nullptr;
  remote_interval_slide = nullptr;
  for (uint8_t i = 0; i < 4; i++) remote_swatch_btns[i] = nullptr;
}

void remote_screen_enter() {
  if (!scr_remote) build_remote_screen();
  mirror_start_task();
  s_mirror_interval_ms = Settings::g_remote_interval_ms;

  // Avoid flashing the previous station's last frame.
  memset(s_remote_canvas_buf + 8, 0, kRemoteW * kRemoteH / 8);
  if (remote_canvas) lv_obj_invalidate(remote_canvas);
  if (s_mirror_mux &&
      xSemaphoreTake(s_mirror_mux, pdMS_TO_TICKS(20)) == pdTRUE) {
    memset(s_mirror_shadow, 0, sizeof(s_mirror_shadow));
    s_mirror_dirty = false;
    xSemaphoreGive(s_mirror_mux);
  }

  s_mirror_active = true;
  remote_show_status("loading...", Theme::kMuted);
  if (s_mirror_task) xTaskNotifyGive(s_mirror_task);
  if (!remote_timer) {
    remote_timer = lv_timer_create(remote_timer_cb, 33, nullptr);
  }
  lv_screen_load(scr_remote);
}

void remote_screen_leave() {
  s_mirror_active = false;
  if (s_mirror_task) xTaskNotifyGive(s_mirror_task);
  if (remote_timer) {
    lv_timer_del(remote_timer);
    remote_timer = nullptr;
  }
}

// click-particle screensaver

namespace {

constexpr size_t   kDotCount          = 64;
constexpr uint8_t  kDotSize           = 4;
constexpr uint8_t  kFadeStep          = 6;
constexpr uint32_t kScreensaverTickMs = 60;

lv_obj_t   * scr_screensaver = nullptr;
lv_obj_t   * s_dots[kDotCount]    = { nullptr };
uint8_t      s_dot_opa[kDotCount] = { 0 };
uint8_t      s_next_dot_slot      = 0;
lv_timer_t * s_screensaver_timer = nullptr;
uint32_t     s_last_click_at_seen[Stations::kMax] = { 0 };
float        s_pre_screensaver_backlight = 1.0f;
bool         s_screensaver_active        = false;

// Hue per chipid (stable across boots, unlike slot index). S=80, V=95.
uint32_t fnv1a32(const char * s) {
  uint32_t h = 2166136261u;
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h;
}

lv_color_t color_for_chipid(const char * chipid) {
  const uint32_t h = fnv1a32(chipid);
  return lv_color_hsv_to_rgb((uint16_t)(h % 360), 80, 95);
}

void on_screensaver_tap(lv_event_t * /*e*/) {
  request_state_change(State::Main);
}

void screensaver_spawn(const char * chipid, uint32_t rng) {
  if (!scr_screensaver) return;
  const uint8_t slot = s_next_dot_slot;
  s_next_dot_slot = (uint8_t)((s_next_dot_slot + 1) % kDotCount);
  const int16_t x = (int16_t)(rng % (Board::SCREEN_W - kDotSize));
  const int16_t y = (int16_t)((rng >> 16) % (Board::SCREEN_H - kDotSize));
  lv_obj_set_pos(s_dots[slot], x, y);
  lv_obj_set_style_bg_color(s_dots[slot], color_for_chipid(chipid), 0);
  lv_obj_set_style_bg_opa(s_dots[slot], LV_OPA_COVER, 0);
  lv_obj_remove_flag(s_dots[slot], LV_OBJ_FLAG_HIDDEN);
  s_dot_opa[slot] = LV_OPA_COVER;
}

void screensaver_timer_cb(lv_timer_t * /*t*/) {
  for (size_t i = 0; i < kDotCount; i++) {
    if (s_dot_opa[i] == 0) continue;
    if (s_dot_opa[i] <= kFadeStep) {
      s_dot_opa[i] = 0;
      lv_obj_add_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      s_dot_opa[i] -= kFadeStep;
      lv_obj_set_style_bg_opa(s_dots[i], s_dot_opa[i], 0);
    }
  }

  if (!Stations::g_mux) return;
  if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(10)) != pdTRUE) return;
  for (size_t i = 0; i < Stations::g_count && i < Stations::kMax; i++) {
    const Stations::Station & s = Stations::g_stations[i];
    if (s.last_click_at_ms == s_last_click_at_seen[i]) continue;
    if (s_last_click_at_seen[i] != 0) {
      const uint32_t rng = (uint32_t)esp_random();
      screensaver_spawn(s.chipid, rng);
    }
    s_last_click_at_seen[i] = s.last_click_at_ms;
  }
  xSemaphoreGive(Stations::g_mux);
}

}  // anonymous namespace

void build_screensaver() {
  if (scr_screensaver) return;
  scr_screensaver = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_screensaver, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(scr_screensaver, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(scr_screensaver, 0, 0);
  lv_obj_add_event_cb(scr_screensaver, on_screensaver_tap,
                      LV_EVENT_CLICKED, nullptr);
  for (size_t i = 0; i < kDotCount; i++) {
    s_dots[i] = lv_obj_create(scr_screensaver);
    lv_obj_remove_style_all(s_dots[i]);
    lv_obj_set_size(s_dots[i], kDotSize, kDotSize);
    lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
    s_dot_opa[i] = 0;
  }
  s_next_dot_slot = 0;
}

void destroy_screensaver() {
  if (s_screensaver_timer) {
    lv_timer_del(s_screensaver_timer);
    s_screensaver_timer = nullptr;
  }
  if (!scr_screensaver) return;
  lv_obj_delete(scr_screensaver);
  scr_screensaver = nullptr;
  for (size_t i = 0; i < kDotCount; i++) s_dots[i] = nullptr;
}

void screensaver_enter() {
  if (!scr_screensaver) build_screensaver();
  s_pre_screensaver_backlight = Settings::g_backlight;
  s_screensaver_active        = true;
  Display::set_backlight(0.30f);
  if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(20)) == pdTRUE) {
    for (size_t i = 0; i < Stations::g_count && i < Stations::kMax; i++) {
      s_last_click_at_seen[i] = Stations::g_stations[i].last_click_at_ms;
    }
    xSemaphoreGive(Stations::g_mux);
  }
  if (!s_screensaver_timer) {
    s_screensaver_timer = lv_timer_create(screensaver_timer_cb,
                                          kScreensaverTickMs, nullptr);
  }
  lv_screen_load(scr_screensaver);
}

void screensaver_leave() {
  if (s_screensaver_active) {
    Display::set_backlight(s_pre_screensaver_backlight);
    s_screensaver_active = false;
  }
  if (s_screensaver_timer) {
    lv_timer_del(s_screensaver_timer);
    s_screensaver_timer = nullptr;
  }
}

// modal alarm overlay

namespace {
lv_obj_t   * s_alarm_modal     = nullptr;
lv_obj_t   * s_alarm_title_lbl = nullptr;
lv_obj_t   * s_alarm_count_lbl = nullptr;
lv_obj_t   * s_alarm_when_lbl  = nullptr;
lv_timer_t * s_alarm_when_timer = nullptr;
uint32_t     s_alarm_fired_ms   = 0;
time_t       s_alarm_fired_epoch = 0;
// 30s tick re-reads the Poller peak when this is set.
bool         s_alarm_is_radmon  = false;

void format_alarm_when(char * buf, size_t cap) {
  const uint32_t age_s = (fast_millis() - s_alarm_fired_ms) / 1000;
  size_t n = 0;
  if      (age_s < 60)    n += snprintf(buf + n, cap - n, "fired just now");
  else if (age_s < 3600)  n += snprintf(buf + n, cap - n, "fired %lum ago",
                                       (unsigned long)(age_s / 60));
  else if (age_s < 86400) n += snprintf(buf + n, cap - n, "fired %luh %lum ago",
                                       (unsigned long)(age_s / 3600),
                                       (unsigned long)((age_s % 3600) / 60));
  else                    n += snprintf(buf + n, cap - n, "fired %lud ago",
                                       (unsigned long)(age_s / 86400));
  if (s_alarm_fired_epoch > 1700000000) {
    struct tm tm_info;
    gmtime_r(&s_alarm_fired_epoch, &tm_info);
    snprintf(buf + n, cap - n, "  (%02d:%02d UTC)",
             tm_info.tm_hour, tm_info.tm_min);
  }
}

void on_alarm_dismiss(lv_event_t * /*e*/) {
  if (s_alarm_modal) lv_obj_add_flag(s_alarm_modal, LV_OBJ_FLAG_HIDDEN);
  if (s_alarm_when_timer) {
    lv_timer_del(s_alarm_when_timer);
    s_alarm_when_timer = nullptr;
  }
}

void alarm_when_tick(lv_timer_t * /*t*/) {
  if (!s_alarm_when_lbl) return;
  char buf[48];
  format_alarm_when(buf, sizeof(buf));
  lv_label_set_text(s_alarm_when_lbl, buf);
  if (s_alarm_is_radmon && s_alarm_count_lbl) {
    char counts[24];
    snprintf(counts, sizeof(counts), "%u of %u stations",
             (unsigned)Poller::g_radmon_alarm_down,
             (unsigned)Poller::g_radmon_alarm_known);
    lv_label_set_text(s_alarm_count_lbl, counts);
  }
}

void build_alarm_modal() {
  if (s_alarm_modal) return;
  s_alarm_modal = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_alarm_modal);
  lv_obj_set_size(s_alarm_modal, Board::SCREEN_W, Board::SCREEN_H);
  lv_obj_set_style_bg_color(s_alarm_modal, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s_alarm_modal, 220, 0);
  lv_obj_add_event_cb(s_alarm_modal, on_alarm_dismiss, LV_EVENT_CLICKED, nullptr);

  s_alarm_title_lbl = lv_label_create(s_alarm_modal);
  lv_label_set_text(s_alarm_title_lbl, "RADMON\nDOWN");
  lv_obj_set_style_text_font(s_alarm_title_lbl, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(s_alarm_title_lbl, lv_color_hex(0xff3030), 0);
  lv_obj_set_style_text_align(s_alarm_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_alarm_title_lbl, LV_ALIGN_CENTER, 0, -30);

  s_alarm_count_lbl = lv_label_create(s_alarm_modal);
  lv_label_set_text(s_alarm_count_lbl, "");
  lv_obj_set_style_text_font(s_alarm_count_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(s_alarm_count_lbl, lv_color_hex(0xffaaaa), 0);
  lv_obj_align(s_alarm_count_lbl, LV_ALIGN_CENTER, 0, 56);

  s_alarm_when_lbl = lv_label_create(s_alarm_modal);
  lv_label_set_text(s_alarm_when_lbl, "");
  lv_obj_set_style_text_font(s_alarm_when_lbl, Theme::f_muted, 0);
  lv_obj_set_style_text_color(s_alarm_when_lbl, lv_color_hex(0xaaaaaa), 0);
  lv_obj_align(s_alarm_when_lbl, LV_ALIGN_CENTER, 0, 86);

  lv_obj_t * dismiss = lv_label_create(s_alarm_modal);
  lv_label_set_text(dismiss, "tap to dismiss");
  lv_obj_set_style_text_font(dismiss, Theme::f_muted, 0);
  lv_obj_set_style_text_color(dismiss, lv_color_hex(0x888888), 0);
  lv_obj_align(dismiss, LV_ALIGN_BOTTOM_MID, 0, -24);

  lv_obj_add_flag(s_alarm_modal, LV_OBJ_FLAG_HIDDEN);
}
}  // anonymous namespace

// generic confirm modal

namespace {
lv_obj_t * s_confirm_modal = nullptr;
lv_obj_t * s_confirm_title = nullptr;
lv_obj_t * s_confirm_body  = nullptr;
void (*s_confirm_yes_cb)() = nullptr;

void on_confirm_no(lv_event_t * /*e*/) {
  if (s_confirm_modal) lv_obj_add_flag(s_confirm_modal, LV_OBJ_FLAG_HIDDEN);
  s_confirm_yes_cb = nullptr;
}

void on_confirm_yes(lv_event_t * /*e*/) {
  if (s_confirm_modal) lv_obj_add_flag(s_confirm_modal, LV_OBJ_FLAG_HIDDEN);
  auto cb = s_confirm_yes_cb;
  s_confirm_yes_cb = nullptr;
  if (cb) cb();
}

void build_confirm_modal() {
  if (s_confirm_modal) return;
  s_confirm_modal = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_confirm_modal);
  lv_obj_set_size(s_confirm_modal, Board::SCREEN_W, Board::SCREEN_H);
  lv_obj_set_style_bg_color(s_confirm_modal, lv_color_hex(0x101418), 0);
  lv_obj_set_style_bg_opa(s_confirm_modal, 240, 0);
  lv_obj_remove_flag(s_confirm_modal, LV_OBJ_FLAG_SCROLLABLE);

  s_confirm_title = lv_label_create(s_confirm_modal);
  lv_obj_set_style_text_font(s_confirm_title, Theme::f_sub, 0);
  lv_obj_set_style_text_color(s_confirm_title, lv_color_hex(Theme::kFg), 0);
  lv_obj_set_style_text_align(s_confirm_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(s_confirm_title, Board::SCREEN_W - 32);
  lv_label_set_long_mode(s_confirm_title, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_align(s_confirm_title, LV_ALIGN_TOP_MID, 0, 70);

  s_confirm_body = lv_label_create(s_confirm_modal);
  lv_obj_set_style_text_font(s_confirm_body, Theme::f_muted, 0);
  lv_obj_set_style_text_color(s_confirm_body, lv_color_hex(Theme::kMuted), 0);
  lv_obj_set_style_text_align(s_confirm_body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(s_confirm_body, Board::SCREEN_W - 40);
  lv_label_set_long_mode(s_confirm_body, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_align(s_confirm_body, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t * cancel = lv_button_create(s_confirm_modal);
  lv_obj_set_size(cancel, 100, 44);
  lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 12, -24);
  lv_obj_set_style_bg_color(cancel, lv_color_hex(0x2b3035), 0);
  lv_obj_set_style_bg_color(cancel, lv_color_hex(0x373b3e), LV_STATE_PRESSED);
  lv_obj_set_style_radius(cancel, 6, 0);
  lv_obj_set_style_border_width(cancel, 0, 0);
  lv_obj_add_event_cb(cancel, on_confirm_no, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * cancel_lbl = lv_label_create(cancel);
  lv_label_set_text(cancel_lbl, "Cancel");
  lv_obj_set_style_text_font(cancel_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_center(cancel_lbl);

  lv_obj_t * yes = lv_button_create(s_confirm_modal);
  lv_obj_set_size(yes, 100, 44);
  lv_obj_align(yes, LV_ALIGN_BOTTOM_RIGHT, -12, -24);
  lv_obj_set_style_bg_color(yes, lv_color_hex(0x882020), 0);
  lv_obj_set_style_bg_color(yes, lv_color_hex(0xa02828), LV_STATE_PRESSED);
  lv_obj_set_style_radius(yes, 6, 0);
  lv_obj_set_style_border_width(yes, 0, 0);
  lv_obj_add_event_cb(yes, on_confirm_yes, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * yes_lbl = lv_label_create(yes);
  lv_label_set_text(yes_lbl, "Confirm");
  lv_obj_set_style_text_font(yes_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(yes_lbl, lv_color_hex(0xffe0e0), 0);
  lv_obj_center(yes_lbl);

  lv_obj_add_flag(s_confirm_modal, LV_OBJ_FLAG_HIDDEN);
}
}  // anonymous namespace

void show_confirm(const char * title, const char * body, void (*on_yes)()) {
  if (!s_confirm_modal) build_confirm_modal();
  lv_label_set_text(s_confirm_title, title ? title : "");
  lv_label_set_text(s_confirm_body,  body  ? body  : "");
  s_confirm_yes_cb = on_yes;
  lv_obj_remove_flag(s_confirm_modal, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_confirm_modal);
}

void show_radmon_alarm(uint8_t down, uint8_t known) {
  if (!s_alarm_modal) build_alarm_modal();
  s_alarm_fired_ms = fast_millis();
  time(&s_alarm_fired_epoch);
  s_alarm_is_radmon = true;

  lv_label_set_text(s_alarm_title_lbl, "RADMON\nDOWN");

  char counts[24];
  snprintf(counts, sizeof(counts), "%u of %u stations",
           (unsigned)down, (unsigned)known);
  lv_label_set_text(s_alarm_count_lbl, counts);

  char when_buf[48];
  format_alarm_when(when_buf, sizeof(when_buf));
  lv_label_set_text(s_alarm_when_lbl, when_buf);

  lv_obj_remove_flag(s_alarm_modal, LV_OBJ_FLAG_HIDDEN);

  if (!s_alarm_when_timer) {
    s_alarm_when_timer = lv_timer_create(alarm_when_tick, 30000, nullptr);
  }
}

void show_cpm_alarm(int station_idx) {
  if (!s_alarm_modal) build_alarm_modal();
  s_alarm_fired_ms = fast_millis();
  time(&s_alarm_fired_epoch);
  s_alarm_is_radmon = false;

  lv_label_set_text(s_alarm_title_lbl, "CPM\nALERT");

  char counts[40] = "";
  if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(20)) == pdTRUE) {
    if (station_idx >= 0 && station_idx < (int)Stations::g_count) {
      const Stations::Station & s = Stations::g_stations[station_idx];
      snprintf(counts, sizeof(counts), "%s  %u cpm",
               Stations::label(s), (unsigned)s.cpm_now);
    }
    xSemaphoreGive(Stations::g_mux);
  }
  lv_label_set_text(s_alarm_count_lbl, counts);

  char when_buf[48];
  format_alarm_when(when_buf, sizeof(when_buf));
  lv_label_set_text(s_alarm_when_lbl, when_buf);

  lv_obj_remove_flag(s_alarm_modal, LV_OBJ_FLAG_HIDDEN);

  if (!s_alarm_when_timer) {
    s_alarm_when_timer = lv_timer_create(alarm_when_tick, 30000, nullptr);
  }
}

// firmware update overlay

namespace {
lv_obj_t * s_update_modal      = nullptr;
lv_obj_t * s_update_status_lbl = nullptr;

void build_update_modal() {
  if (s_update_modal) return;
  s_update_modal = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_update_modal);
  lv_obj_set_size(s_update_modal, Board::SCREEN_W, Board::SCREEN_H);
  lv_obj_set_style_bg_color(s_update_modal, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s_update_modal, LV_OPA_COVER, 0);

  lv_obj_t * title = lv_label_create(s_update_modal);
  lv_label_set_text(title, "FIRMWARE\nUPDATE");
  lv_obj_set_style_text_font(title, Theme::f_sub, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(Theme::kAccent), 0);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);

  s_update_status_lbl = lv_label_create(s_update_modal);
  lv_label_set_text(s_update_status_lbl, "uploading...");
  lv_obj_set_style_text_font(s_update_status_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(s_update_status_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(s_update_status_lbl, LV_ALIGN_CENTER, 0, 60);

  lv_obj_t * warn = lv_label_create(s_update_modal);
  lv_label_set_text(warn, "do not unplug");
  lv_obj_set_style_text_font(warn, Theme::f_muted, 0);
  lv_obj_set_style_text_color(warn, lv_color_hex(0xff5555), 0);
  lv_obj_align(warn, LV_ALIGN_BOTTOM_MID, 0, -30);
}
}  // anonymous namespace

void show_update_modal() {
  build_update_modal();
  lv_obj_remove_flag(s_update_modal, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_update_modal);
  // Render now; OTA write blocks the main loop.
  lv_refr_now(nullptr);
}

void update_modal_set_status(const char * msg) {
  if (s_update_status_lbl && msg) {
    lv_label_set_text(s_update_status_lbl, msg);
    lv_refr_now(nullptr);
  }
}

// info / about

lv_obj_t * scr_info = nullptr;

namespace {
lv_obj_t * info_ip_val       = nullptr;
lv_obj_t * info_mac_val      = nullptr;
lv_obj_t * info_host_val     = nullptr;
lv_obj_t * info_uptime_val   = nullptr;
lv_obj_t * info_heap_val     = nullptr;
lv_obj_t * info_heap_min_val = nullptr;
lv_obj_t * info_stations_val = nullptr;

void on_info_back(lv_event_t *) { request_state_change(State::Settings); }

uint8_t  s_ver_tap_count   = 0;
uint32_t s_ver_tap_last_ms = 0;
constexpr uint8_t  kVerTapTarget = 5;
constexpr uint32_t kVerTapWindowMs = 3000;

void ver_jiggle_cb(void * obj, int32_t v) {
  lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(obj), v, 0);
}

void on_info_version_tap(lv_event_t * e) {
  const uint32_t now = fast_millis();
  if (now - s_ver_tap_last_ms > kVerTapWindowMs) s_ver_tap_count = 0;
  s_ver_tap_last_ms = now;
  s_ver_tap_count++;

  // Cheap tap feedback: nudge translate_y a few px and ease back to 0.
  // Alternates direction by parity so successive taps look like a wobble.
  lv_obj_t * target = static_cast<lv_obj_t *>(lv_event_get_target(e));
  if (target) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, target);
    lv_anim_set_exec_cb(&a, ver_jiggle_cb);
    const int32_t kick = (s_ver_tap_count & 1) ? -4 : 4;
    lv_anim_set_values(&a, kick, 0);
    lv_anim_set_time(&a, 140);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
  }

  if (s_ver_tap_count >= kVerTapTarget) {
    s_ver_tap_count = 0;
    request_state_change(State::SpecCard);
  }
}

void on_spec_card_tap(lv_event_t *) { request_state_change(State::Info); }

lv_obj_t * make_info_row(lv_obj_t * parent, int y, const char * label) {
  lv_obj_t * lbl = lv_label_create(parent);
  lv_label_set_text(lbl, label);
  lv_obj_set_style_text_font(lbl, Theme::f_muted, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::kMuted), 0);
  lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 12, y);

  lv_obj_t * val = lv_label_create(parent);
  lv_obj_set_style_text_font(val, Theme::f_muted, 0);
  lv_obj_set_style_text_color(val, lv_color_hex(Theme::kFg), 0);
  lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_width(val, Board::SCREEN_W - 80 - 12);
  lv_obj_align(val, LV_ALIGN_TOP_LEFT, 80, y);
  return val;
}
}  // anonymous namespace

void build_info() {
  if (scr_info) return;
  scr_info = lv_obj_create(nullptr);
  style_dark_screen(scr_info);

  lv_obj_t * back = lv_button_create(scr_info);
  lv_obj_set_size(back, 56, 32);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 4);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x2b3035), 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x373b3e), LV_STATE_PRESSED);
  lv_obj_set_style_radius(back, 6, 0);
  lv_obj_set_style_border_width(back, 0, 0);
  lv_obj_add_event_cb(back, on_info_back, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * back_lbl = lv_label_create(back);
  lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_font(back_lbl, Theme::f_body, 0);
  lv_obj_set_style_text_color(back_lbl, lv_color_hex(Theme::kFg), 0);
  lv_obj_center(back_lbl);

  lv_obj_t * title = lv_label_create(scr_info);
  lv_label_set_text(title, "Info");
  lv_obj_set_style_text_font(title, Theme::f_sub, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(Theme::kFg), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 70, 10);

  int y = 56;
  const int dy = 26;
  info_ip_val       = make_info_row(scr_info, y, "IP");        y += dy;
  info_mac_val      = make_info_row(scr_info, y, "MAC");       y += dy;
  info_host_val     = make_info_row(scr_info, y, "Hostname");  y += dy;

  lv_obj_t * ver_val = make_info_row(scr_info, y, "Version");  y += dy;
  lv_label_set_text(ver_val, GADGET_VERSION);
  lv_obj_add_flag(ver_val, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(ver_val, on_info_version_tap, LV_EVENT_CLICKED, nullptr);
  s_ver_tap_count = 0;  // reset between visits

  lv_obj_t * build_val = make_info_row(scr_info, y, "Build");  y += dy;
  lv_label_set_text(build_val, __DATE__);

  info_uptime_val   = make_info_row(scr_info, y, "Uptime");    y += dy;
  info_heap_val     = make_info_row(scr_info, y, "Free heap"); y += dy;
  info_heap_min_val = make_info_row(scr_info, y, "Min heap");  y += dy;
  info_stations_val = make_info_row(scr_info, y, "Stations");  y += dy;
}

void destroy_info() {
  if (!scr_info) return;
  lv_obj_delete(scr_info);
  scr_info          = nullptr;
  info_ip_val       = nullptr;
  info_mac_val      = nullptr;
  info_host_val     = nullptr;
  info_uptime_val   = nullptr;
  info_heap_val     = nullptr;
  info_heap_min_val = nullptr;
  info_stations_val = nullptr;
}

lv_obj_t * scr_spec_card = nullptr;

void build_spec_card() {
  if (scr_spec_card) return;
  scr_spec_card = lv_obj_create(nullptr);
  style_dark_screen(scr_spec_card);
  lv_obj_add_flag(scr_spec_card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scr_spec_card, on_spec_card_tap, LV_EVENT_CLICKED, nullptr);

  lv_obj_t * card = lv_obj_create(scr_spec_card);
  lv_obj_set_size(card, Board::SCREEN_W - 20, Board::SCREEN_H - 20);
  lv_obj_center(card);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x1a1612), 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0xFBD000), 0);
  lv_obj_set_style_border_width(card, 2, 0);
  lv_obj_set_style_radius(card, 0, 0);
  lv_obj_set_style_pad_all(card, 12, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t * title = lv_label_create(card);
  lv_label_set_text(title, "TOP SECRET");
  lv_obj_set_style_text_font(title, Theme::f_sub, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFBD000), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t * sub = lv_label_create(card);
  lv_label_set_text(sub, "MARK III COUNTER");
  lv_obj_set_style_text_font(sub, Theme::f_muted, 0);
  lv_obj_set_style_text_color(sub, lv_color_hex(0x8b7d6b), 0);
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 36);

  char specs[256];
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(specs, sizeof(specs),
    "SERIAL    %02X%02X%02X%02X%02X%02X\n"
    "STATION   %s\n"
    "PROJECT   MANHATTAN\n"
    "TYPE      TS/SCI\n"
    "DATE      1945-2026\n"
    "VER       %s",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
    Settings::g_hostname,
    GADGET_VERSION);

  lv_obj_t * body = lv_label_create(card);
  lv_label_set_text(body, specs);
  lv_obj_set_style_text_font(body, Theme::f_muted, 0);
  lv_obj_set_style_text_color(body, lv_color_hex(0xe8e2d4), 0);
  lv_obj_align(body, LV_ALIGN_TOP_LEFT, 4, 72);

  // Pivot lands near label centre so the rotation looks intentional.
  lv_obj_t * stamp = lv_label_create(scr_spec_card);
  lv_label_set_text(stamp, "DECLASSIFIED");
  lv_obj_set_style_text_font(stamp, Theme::f_sub, 0);
  lv_obj_set_style_text_color(stamp, lv_color_hex(0xc0392b), 0);
  lv_obj_set_style_transform_pivot_x(stamp, 80, 0);
  lv_obj_set_style_transform_pivot_y(stamp, 14, 0);
  lv_obj_set_style_transform_rotation(stamp, -150, 0);   // -15 deg
  lv_obj_align(stamp, LV_ALIGN_CENTER, -8, 24);
}

void destroy_spec_card() {
  if (!scr_spec_card) return;
  lv_obj_delete(scr_spec_card);
  scr_spec_card = nullptr;
}

lv_obj_t * scr_post = nullptr;

namespace {
constexpr size_t   kPostLineCount  = 11;
constexpr uint32_t kPostLineMs     = 200;
constexpr uint32_t kPostSettleMs   = 2000;
constexpr int      kPostCols       = 30;   // 240 px / 8 px (unscii_8)

lv_obj_t * post_lines[kPostLineCount] = {nullptr};
lv_obj_t * post_status_lbl            = nullptr;
char       s_post_buf[kPostLineCount][72];
uint8_t    s_post_revealed = 0;
uint32_t   s_post_next_ms  = 0;
uint32_t   s_post_settle_end_ms = 0;
bool       s_post_settle_started = false;

struct PoolLine { const char * label; const char * status; };

// systemd-style boot log: "Label........ [ STATUS ]". Entries whose
// combined length exceeds kPostCols spill the bracket onto a second
// indented row; four are drawn per boot.
const PoolLine s_post_pool[] = {
  {"Photons",           "TRAPPED"},
  {"Sarcasm Module",    "ON"},
  {"Spicy Atoms",       "CONTAINED"},
  {"Neutrinos",         "BEHAVING"},
  {"Mu Meson",          "DECAYED"},
  {"Trefoil",           "RADIANT"},
  {"Cosmic Ray Buffer", "EMPTY"},
  {"Lead Apron",        "HEAVY"},
  {"Higgs Boson",       "MISPLACED"},
  {"Bananas Detected",  "STANDBY"},
  {"Schrodinger's Cat", "UNOBSERVED"},
  {"Cherenkov Glow",    "DIMMED"},
  {"Background Noise",  "CRISPY"},
  {"Half-Life Timer",   "RUNNING"},
  {"EPA Regulations",   "IGNORED"},
  {"Planck's Constant", "MINISCULE"},
  {"Fermi Paradox",     "UNRESOLVED"},
  {"Hawking Radiation", "EVAPORATING"},
  {"Hubble Focus",      "DEEP"},
  {"OLED Display",      "NOT FOUND"},
  {"Firmware Stability","QUESTIONABLE"},
  {"Error Log",         "FULL"},
  {"Brownian Motion Unit","STEWING"},
  {"Improbability Factor","1:1"},
  {"Self-Destruct Logic","AD-BLOCKED"},
};
constexpr size_t kPoolCount = sizeof(s_post_pool) / sizeof(s_post_pool[0]);

void format_pool_line(char * buf, size_t cap, const PoolLine & p) {
  if (!buf || cap < 2) return;
  const size_t llen = strlen(p.label);
  const size_t slen = strlen(p.status);
  const size_t bracket = 4 + slen;   // "[ X ]"
  size_t pos = 0;

  if (llen + 1 + bracket <= (size_t)kPostCols) {
    // Single line: "label" + dots + " [ status ]"
    int dots = kPostCols - (int)llen - 1 - (int)bracket;
    if (dots < 1) dots = 1;
    pos += snprintf(buf, cap, "%s", p.label);
    while (dots-- > 0 && pos < cap - 1) buf[pos++] = '.';
    pos += snprintf(buf + pos, cap - pos, " [ %s ]", p.status);
  } else {
    // Two lines: label + dots to full width, then spaces + bracket on row 2.
    int line1_dots  = kPostCols - (int)llen;
    int line2_spaces = kPostCols - (int)bracket;
    if (line1_dots  < 0) line1_dots  = 0;
    if (line2_spaces < 0) line2_spaces = 0;
    pos += snprintf(buf, cap, "%s", p.label);
    while (line1_dots-- > 0 && pos < cap - 1) buf[pos++] = '.';
    if (pos < cap - 1) buf[pos++] = '\n';
    while (line2_spaces-- > 0 && pos < cap - 1) buf[pos++] = ' ';
    pos += snprintf(buf + pos, cap - pos, "[ %s ]", p.status);
  }
  buf[cap - 1] = '\0';
}

void on_post_tap(lv_event_t * /*e*/) {
  // Skip straight into the boot WiFi check; main.cpp's State::Post tick
  // will see post_tick() == true on the next iteration and transition out.
  s_post_revealed = kPostLineCount;
  s_post_settle_started = true;
  s_post_settle_end_ms = fast_millis();
}
}

void build_post(const char * ssid) {
  if (scr_post) return;
  scr_post = lv_obj_create(nullptr);
  style_dark_screen(scr_post);
  lv_obj_add_flag(scr_post, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scr_post, on_post_tap, LV_EVENT_CLICKED, nullptr);

  // Trefoil + identity. Echoes parent firmware's Log::banner.
  snprintf(s_post_buf[0], sizeof(s_post_buf[0]), "   ___");
  snprintf(s_post_buf[1], sizeof(s_post_buf[1]), "   \\_/    ESPGeiger Gadget");
  snprintf(s_post_buf[2], sizeof(s_post_buf[2]), ".--.O.--. v %s", GIT_VERSION);
  snprintf(s_post_buf[3], sizeof(s_post_buf[3]), " \\/   \\/");
  snprintf(s_post_buf[4], sizeof(s_post_buf[4]), " ");

  // Pick 5 distinct lines from the pool. Fisher-Yates on a small index
  // array; cheap and unbiased.
  uint8_t idx[kPoolCount];
  for (size_t i = 0; i < kPoolCount; i++) idx[i] = (uint8_t)i;
  for (size_t i = kPoolCount - 1; i > 0; i--) {
    const uint32_t j = esp_random() % (i + 1);
    const uint8_t  t = idx[i]; idx[i] = idx[j]; idx[j] = t;
  }
  format_pool_line(s_post_buf[5], sizeof(s_post_buf[5]),
                   PoolLine{"Booting Firmware", "OK"});
  for (size_t i = 1; i < 5; i++) {
    format_pool_line(s_post_buf[5 + i], sizeof(s_post_buf[5 + i]),
                     s_post_pool[idx[i - 1]]);
  }
  if (ssid && ssid[0]) {
    snprintf(s_post_buf[10], sizeof(s_post_buf[10]),
             "Connecting %s", ssid);
  } else {
    snprintf(s_post_buf[10], sizeof(s_post_buf[10]),
             "no network saved");
  }

  // Stack labels by measuring each one's actual rendered height after we
  // set its full text; mixed single-line and 2-line entries position
  // without overlap or wasted whitespace.
  int y = 6;
  for (size_t i = 0; i < kPostLineCount; i++) {
    post_lines[i] = lv_label_create(scr_post);
    lv_obj_set_style_text_font(post_lines[i], &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(post_lines[i], lv_color_hex(Theme::kFg), 0);
    lv_obj_set_width(post_lines[i], Board::SCREEN_W);
    lv_label_set_long_mode(post_lines[i], LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(post_lines[i], s_post_buf[i]);
    lv_obj_update_layout(post_lines[i]);
    lv_obj_align(post_lines[i], LV_ALIGN_TOP_LEFT, 0, y);
    y += lv_obj_get_height(post_lines[i]) + 4;
    // Clear so the cinematic reveals from blank.
    lv_label_set_text(post_lines[i], "");
  }
  // Trefoil + version banner in theme accent (yellow for Gadget, green for Retro).
  for (size_t i = 0; i < 4; i++) {
    lv_obj_set_style_text_color(post_lines[i], lv_color_hex(Theme::kAccent), 0);
  }

  s_post_revealed       = 0;
  s_post_next_ms        = fast_millis() + kPostLineMs;
  s_post_settle_started = false;
  s_post_settle_end_ms  = 0;
}

bool post_tick() {
  const uint32_t now = fast_millis();
  if (s_post_revealed < kPostLineCount) {
    if (now < s_post_next_ms) return false;
    if (post_lines[s_post_revealed]) {
      lv_label_set_text(post_lines[s_post_revealed], s_post_buf[s_post_revealed]);
    }
    s_post_revealed++;
    s_post_next_ms = now + kPostLineMs;
    return false;
  }
  // All lines revealed - 2 s settle so the funny ticker has a turn before
  // the boot hands off to ConnectingSaved.
  if (!s_post_settle_started) {
    s_post_settle_started = true;
    s_post_settle_end_ms  = now + kPostSettleMs;
  }
  return now >= s_post_settle_end_ms;
}

bool post_reveal_done() {
  return s_post_revealed >= kPostLineCount;
}

bool post_is_settled() {
  return s_post_settle_started && (fast_millis() >= s_post_settle_end_ms);
}

void destroy_post() {
  if (!scr_post) return;
  lv_obj_delete(scr_post);
  scr_post = nullptr;
  post_status_lbl = nullptr;
  for (size_t i = 0; i < kPostLineCount; i++) post_lines[i] = nullptr;
}

void info_refresh() {
  if (!scr_info) return;
  char b[32];

  if (info_ip_val) {
    if (WiFi.status() == WL_CONNECTED) {
      const IPAddress ip = WiFi.localIP();
      snprintf(b, sizeof(b), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    } else {
      strcpy(b, "not connected");
    }
    lv_label_set_text(info_ip_val, b);
  }
  if (info_mac_val) {
    // uint8_t* overload skips the Arduino String alloc.
    static char s_mac_cached[18] = {0};
    if (!s_mac_cached[0]) {
      uint8_t m[6];
      WiFi.macAddress(m);
      snprintf(s_mac_cached, sizeof(s_mac_cached),
               "%02X:%02X:%02X:%02X:%02X:%02X",
               m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    lv_label_set_text(info_mac_val, s_mac_cached);
  }
  if (info_host_val) {
    lv_label_set_text(info_host_val, Settings::g_hostname);
  }
  if (info_uptime_val) {
    char up[20];
    format_uptime(millis() / 1000UL, up, sizeof(up));
    lv_label_set_text(info_uptime_val, up);
  }
  if (info_heap_val) {
    snprintf(b, sizeof(b), "%lu B", (unsigned long)ESP.getFreeHeap());
    lv_label_set_text(info_heap_val, b);
  }
  if (info_heap_min_val) {
    multi_heap_info_t hi;
    heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snprintf(b, sizeof(b), "%lu B", (unsigned long)hi.minimum_free_bytes);
    lv_label_set_text(info_heap_min_val, b);
  }
  if (info_stations_val) {
    size_t n = 0, udp = 0;
    if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(20)) == pdTRUE) {
      n = Stations::g_count;
      for (size_t i = 0; i < n; i++) {
        if (Stations::is_udp_active(Stations::g_stations[i])) udp++;
      }
      xSemaphoreGive(Stations::g_mux);
    }
    snprintf(b, sizeof(b), "%u / %u (%u UDP)",
             (unsigned)n, (unsigned)Stations::g_max, (unsigned)udp);
    lv_label_set_text(info_stations_val, b);
  }
}

// teardown

void destroy_setup_screens() {
  if (scr_picker) {
    lv_obj_delete(scr_picker);
    scr_picker     = nullptr;
    picker_status  = nullptr;
    picker_list    = nullptr;
  }
  destroy_password();
}

}  // namespace Ui
