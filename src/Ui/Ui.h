/*
  Ui.h - LVGL screens

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

// LVGL screens for ESPGeiger Gadget - sized for 240x320 portrait.
//
// Screens are built once at boot (cheap fixed widgets) or lazily on first
// entry (heavier ones like the password keyboard). Setup screens are torn
// down on Main entry to return the keyboard widget's pool memory.

#pragma once

#include <Arduino.h>
#include <lvgl.h>

namespace Ui {

extern lv_obj_t * scr_splash;
extern lv_obj_t * scr_picker;
extern lv_obj_t * scr_password;
extern lv_obj_t * scr_main;
extern lv_obj_t * scr_settings;
extern lv_obj_t * scr_detail;

// Built once at boot.
void build_splash();
void build_picker();

// Lazy - destroyed when leaving (password keyboard is ~30 KB of LVGL pool).
void build_password();
void destroy_password();

void build_main();
void destroy_main();

// Splash helpers.
void splash_set_status(const char * msg);

// Rotates a cheeky status line on the splash while WiFi is coming up.
// No-op when not in Gadget theme mode.
void splash_tick_funny();

// Picker helpers.
void picker_set_status(const char * msg);
void picker_clear_list();
void picker_populate();              // reads Wifi::g_scan and fills the list

// Password helpers.
void password_show_for(const char * ssid);

// Main screen - scrollable station list. Set top status from main.cpp;
// list rows are rebuilt from Stations::g_stations[] by main_refresh_list().
void main_set_status(const char * msg);
void main_refresh_list();

// Settings - lazy-built on entry, destroyed on leave (sliders + toggle
// widgets aren't free in the LVGL pool).
void build_settings();
void destroy_settings();
void settings_refresh();   // pull current Settings:: globals into the widgets

// Settings edit (textarea + keyboard) - currently only used for the UDP
// multicast addr+port. Lazy-built, destroyed on leave.
extern lv_obj_t * scr_settings_edit;
void build_settings_edit();
void destroy_settings_edit();
void settings_edit_show_multicast();   // pre-fill textarea with "addr:port"
void settings_edit_show_hostname();    // pre-fill textarea with the hostname

// Tap handlers in Settings set this before requesting State::SettingsEdit;
// the entry function dispatches to the matching show_*.
enum class SettingsEditTarget { Multicast, Hostname };
extern SettingsEditTarget g_pending_settings_edit;

// Detail screen - single station view with big CPM, three sparklines.
// Lazy-built, destroyed on leave (chart widgets are heavy).
void build_detail();
void destroy_detail();
void detail_refresh();   // pull current Stations::g_stations[State::g_detail_idx] into widgets

// Frees the setup-only screens (picker, password) on Main entry, returning
// their LVGL pool memory.
void destroy_setup_screens();

// Dice roller mini-app - lazy build/destroy. dice_enter() also kicks off
// the per-frame cycle animation so the screen lands already rolling.
void build_dice();
void destroy_dice();
void dice_enter();   // call from State::Dice entry - seeds animation timer
void dice_leave();   // call from State::Dice exit  - kills animation timer

// Remote OLED mirror - fetches /screen.bin from g_detail_idx every 1s,
// transposes the SSD1306 page-mode layout into a 1bpp LVGL canvas, and
// paints. 404 / timeout -> "not available" message + auto-return after a
// short delay. /screen/tap drives the remote's button to cycle pages.
void build_remote_screen();
void destroy_remote_screen();
void remote_screen_enter();   // arms fetch timer, kicks first fetch
void remote_screen_leave();   // cancels timer

// Click-particle screensaver. Drop a dot for each newly credited UDP
// click; particles fade over a few seconds. Tap anywhere to dismiss.
void build_screensaver();
void destroy_screensaver();
void screensaver_enter();
void screensaver_leave();

// Read-only "About" screen reached from a row in Settings.
extern lv_obj_t * scr_info;
void build_info();
void destroy_info();
void info_refresh();

extern lv_obj_t * scr_spec_card;
void build_spec_card();
void destroy_spec_card();

// Mono boot-log screen for Gadget + Retro modes.
extern lv_obj_t * scr_post;
void build_post(const char * ssid = nullptr);
void destroy_post();
bool post_tick();          // true once reveal + settle is done
bool post_is_settled();    // true once the 2 s settle window has elapsed


// Modal alarm overlay. Drawn on lv_layer_top so it floats above the
// active screen; needs a tap to dismiss.
void show_radmon_alarm(uint8_t down, uint8_t known);
void show_cpm_alarm(int station_idx);

// Generic yes/cancel confirmation modal. Cancel just hides; Confirm
// calls on_yes then hides.
void show_confirm(const char * title, const char * body, void (*on_yes)());

// Firmware-update overlay. Shown opaque while OTA is in progress so the
// user sees the activity isn't malicious. set_status pushes a one-liner
// onto the modal between flash chunks.
void show_update_modal();
void update_modal_set_status(const char * msg);

// Toggle the 4 CRT corner masks (top-layer) based on Settings::g_theme_mode.
// Called once at boot + on every theme change.
void update_crt_corners();

}  // namespace Ui
