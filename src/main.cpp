/*
  main.cpp - setup + main loop + state machine driver

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

// Splash -> ConnectingSaved -> Main (or via Picker / Password if no creds).
// Event handlers use request_state_change(); enter_state() runs at the top
// of state_tick to avoid tearing down widgets mid-dispatch.

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <lvgl.h>

#include "Audio/Audio.h"
#include "CrashDump/CrashDump.h"
#include "Discovery/Discovery.h"
#include "Display/Display.h"
#include "Led/Led.h"
#include "MontePi/MontePi.h"
#include "Poller/Poller.h"
#include "Settings/Settings.h"
#include "State/State.h"
#include "Stations/Stations.h"
#include "Theme/Theme.h"
#include "UdpRx/UdpRx.h"
#include "Util/FastMillis.h"
#include "Ui/Ui.h"
#include "WebServer/WebServer.h"
#include "Wifi/Wifi.h"
#include "board.h"

namespace {

constexpr uint32_t kConnectTimeoutMs = 20000;

uint32_t g_state_started = 0;
volatile State g_pending_state    = State::Splash;
volatile bool  g_state_change_req = false;
bool g_mdns_started = false;
uint32_t g_last_main_refresh_ms   = 0;
uint32_t g_last_stations_tick_ms  = 0;
uint32_t g_last_detail_refresh_ms = 0;
uint32_t g_last_info_refresh_ms   = 0;

void enter_state(State s) {
  g_state = s;
  g_state_started = fast_millis();

  if (s != State::Password)     Ui::destroy_password();
  if (s != State::Settings)     Ui::destroy_settings();
  if (s != State::SettingsEdit) Ui::destroy_settings_edit();
  if (s != State::Main)         Ui::destroy_main();
  if (s != State::Detail) {
    Ui::destroy_detail();
    Poller::set_priority_index(-1);
  }
  if (s != State::Dice) {
    Ui::dice_leave();
    Ui::destroy_dice();
  }
  if (s != State::RemoteScreen) {
    Ui::remote_screen_leave();
    Ui::destroy_remote_screen();
  }
  if (s != State::Screensaver) {
    Ui::screensaver_leave();
    Ui::destroy_screensaver();
  }
  if (s != State::Info) Ui::destroy_info();
  if (s != State::SpecCard) Ui::destroy_spec_card();
  // scr_post survives the whole boot path; destroyed on Main entry or
  // when bounced to the picker. See destroy_setup_screens.
  if (s != State::Post && s != State::ConnectingSaved &&
      s != State::Connecting && s != State::Splash) {
    Ui::destroy_post();
  }

  switch (s) {
    case State::Splash: {
      lv_screen_load(Ui::scr_splash);
      // Kick off WiFi immediately so the 4 s splash overlaps the connect.
      char ssid[Wifi::kSsidLen];
      char pass[Wifi::kPassLen];
      if (Wifi::load_creds(ssid, pass)) {
        strncpy(g_pending_ssid, ssid, kSsidMax - 1);
        g_pending_ssid[kSsidMax - 1] = '\0';
        char msg[80];
        snprintf(msg, sizeof(msg), "Connecting to %s...", ssid);
        Ui::splash_set_status(msg);
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, pass);
      } else {
        Ui::splash_set_status("starting...");
      }
      break;
    }

    case State::ConnectingSaved: {
      char ssid[Wifi::kSsidLen];
      char pass[Wifi::kPassLen];
      Wifi::load_creds(ssid, pass);
      strncpy(g_pending_ssid, ssid, kSsidMax - 1);
      g_pending_ssid[kSsidMax - 1] = '\0';
      char msg[80];
      snprintf(msg, sizeof(msg), "Connecting to %s...", ssid);
      Ui::splash_set_status(msg);
      // Stay on the POST screen if it's still alive (Gadget/Retro boot
      // path); otherwise the SVG splash (Default).
      lv_screen_load(Ui::scr_post ? Ui::scr_post : Ui::scr_splash);
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid, pass);
      break;
    }

    case State::Picker: {
      // arduino-esp32 v3 wedges in WIFI_SCAN_RUNNING without this reset.
      WiFi.disconnect(/*eraseap=*/false, /*wifioff=*/true);
      delay(50);
      WiFi.mode(WIFI_STA);
      delay(100);
      Wifi::g_scan_count = 0;
      Ui::picker_clear_list();
      Ui::picker_set_status("scanning...");
      lv_screen_load(Ui::scr_picker);
      int rc = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
      Serial.printf("[picker] scanNetworks(async) -> %d\n", rc);
      break;
    }

    case State::Password:
      Ui::build_password();
      Ui::password_show_for(g_pending_ssid);
      lv_screen_load(Ui::scr_password);
      break;

    case State::Connecting: {
      char msg[80];
      snprintf(msg, sizeof(msg), "connecting to %s...", g_pending_ssid);
      Ui::splash_set_status(msg);
      lv_screen_load(Ui::scr_post ? Ui::scr_post : Ui::scr_splash);
      break;
    }

    case State::Settings:
      Ui::build_settings();
      Ui::settings_refresh();
      lv_screen_load(Ui::scr_settings);
      break;

    case State::SettingsEdit:
      Ui::build_settings_edit();
      if (Ui::g_pending_settings_edit == Ui::SettingsEditTarget::Hostname) {
        Ui::settings_edit_show_hostname();
      } else {
        Ui::settings_edit_show_multicast();
      }
      lv_screen_load(Ui::scr_settings_edit);
      break;

    case State::TouchCal: {
      // calibrate_touch blocks and writes to the panel directly; LVGL
      // doesn't know, so invalidate after to force a repaint.
      uint16_t cal[Settings::kTouchCalLen] = {0};
      int8_t   off_x = 0, off_y = 0;
      Display::calibrate_touch(cal, &off_x, &off_y);
      memcpy(Settings::g_touch_cal, cal, sizeof(cal));
      Settings::g_touch_offset_x = off_x;
      Settings::g_touch_offset_y = off_y;
      Display::set_touch_calibration(Settings::g_touch_cal);
      Display::set_touch_offset(Settings::g_touch_offset_x,
                                Settings::g_touch_offset_y);
      Settings::save();
      Serial.printf("[touch] saved cal: %u %u %u %u %u %u %u %u  off %d,%d\n",
                    cal[0], cal[1], cal[2], cal[3],
                    cal[4], cal[5], cal[6], cal[7],
                    (int)off_x, (int)off_y);
      enter_state(State::Settings);
      lv_obj_invalidate(lv_screen_active());
      break;
    }

    case State::Detail:
      Ui::build_detail();
      Ui::detail_refresh();
      lv_screen_load(Ui::scr_detail);
      Poller::set_priority_index(g_detail_idx);
      break;

    case State::Dice:
      Ui::dice_enter();
      break;

    case State::RemoteScreen:
      Ui::remote_screen_enter();
      break;

    case State::Screensaver:
      Ui::screensaver_enter();
      break;

    case State::Info:
      Ui::build_info();
      Ui::info_refresh();
      lv_screen_load(Ui::scr_info);
      break;

    case State::SpecCard:
      Ui::build_spec_card();
      lv_screen_load(Ui::scr_spec_card);
      break;

    case State::Post: {
      char ssid[Wifi::kSsidLen];
      char pass[Wifi::kPassLen];
      const bool have_creds = Wifi::load_creds(ssid, pass);
      if (have_creds) {
        strncpy(g_pending_ssid, ssid, kSsidMax - 1);
        g_pending_ssid[kSsidMax - 1] = '\0';
      }
      // ssid lands in the boot log; status line at the bottom is owned by
      // the funny ticker.
      Ui::build_post(have_creds ? ssid : nullptr);
      if (have_creds) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, pass);
      }
      lv_screen_load(Ui::scr_post);
      break;
    }

    case State::Main:
      Ui::build_main();
      if (!g_mdns_started) {
        g_mdns_started = MDNS.begin(Settings::g_hostname);
        Discovery::set_started(g_mdns_started);
        Poller::set_started(true);
        UdpRx::set_started(true);
        WebSrv::init();
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        if (g_mdns_started) MDNS.addService("http", "tcp", 80);
        const IPAddress ip = WiFi.localIP();
        const IPAddress gw = WiFi.gatewayIP();
        Serial.printf("[wifi] connected: SSID=%s IP=%u.%u.%u.%u GW=%u.%u.%u.%u "
                      "RSSI=%d  mdns=%d\n",
                      g_pending_ssid,
                      ip[0], ip[1], ip[2], ip[3],
                      gw[0], gw[1], gw[2], gw[3],
                      (int)WiFi.RSSI(),
                      (int)g_mdns_started);
      }
      Ui::main_refresh_list();
      lv_screen_load(Ui::scr_main);
      Ui::destroy_setup_screens();
      break;
  }
}

void state_tick() {
  if (g_state_change_req) {
    g_state_change_req = false;
    enter_state(g_pending_state);
  }
  switch (g_state) {
    case State::Splash: {
      Ui::splash_tick_funny();
      // 4 s minimum splash; jumps straight to Main if WiFi is already up,
      // otherwise sits here until kConnectTimeoutMs.
      const uint32_t since = fast_millis() - g_state_started;
      if (WiFi.status() == WL_CONNECTED && since > 4000) {
        enter_state(State::Main);
        break;
      }
      if (since > 4000) {
        if (g_pending_ssid[0] == '\0') {
          enter_state(State::Picker);
        } else if (since > kConnectTimeoutMs) {
          enter_state(State::Picker);
        }
      }
      break;
    }

    case State::Post: {
      Ui::post_tick();

      // Gate the Main transition on the 4 s settle so a fast WiFi connect
      // doesn't snatch the screen before the user sees anything.
      if (Ui::post_is_settled()) {
        if (WiFi.status() == WL_CONNECTED) {
          enter_state(State::Main);
          break;
        }
        if (g_pending_ssid[0] == '\0') { enter_state(State::Picker); break; }
        if (fast_millis() - g_state_started > kConnectTimeoutMs) {
          enter_state(State::Picker);
        }
      }
      break;
    }

    case State::ConnectingSaved:
    case State::Connecting: {
      Ui::splash_tick_funny();
      wl_status_t s = WiFi.status();
      if (s == WL_CONNECTED) {
        enter_state(State::Main);
      } else if (fast_millis() - g_state_started > kConnectTimeoutMs) {
        enter_state(State::Picker);
      }
      break;
    }

    case State::Picker: {
      // Stop touching the radio once the list is populated.
      static uint32_t last_secs_shown    = 0;
      static uint32_t last_state_started = 0;
      static bool     populated          = false;
      if (last_state_started != g_state_started) {
        last_state_started = g_state_started;
        populated = false;
      }
      if (populated) break;

      int n = WiFi.scanComplete();
      uint32_t elapsed = fast_millis() - g_state_started;
      uint32_t secs = elapsed / 1000;

      if (n == WIFI_SCAN_RUNNING) {
        if (secs != last_secs_shown) {
          last_secs_shown = secs;
          char buf[40];
          snprintf(buf, sizeof(buf), "scanning... %lus", (unsigned long)secs);
          Ui::picker_set_status(buf);
        }
        if (elapsed > 15000) {
          // Radio wedged in RUNNING.
          Serial.println("[picker] scan stuck - restarting");
          WiFi.scanDelete();
          enter_state(State::Picker);
        }
      } else if (n >= 0) {
        last_secs_shown = 0;
        size_t kept = Wifi::process_scan(n);
        Serial.printf("[picker] scan complete: %d raw -> %u after filter\n",
                      n, (unsigned)kept);
        Ui::picker_populate();
        if (kept == 0) {
          Ui::picker_set_status("nothing nearby - tap to retry");
        } else {
          char buf[24];
          snprintf(buf, sizeof(buf), "%u nearby", (unsigned)kept);
          Ui::picker_set_status(buf);
        }
        WiFi.scanDelete();
        populated = true;
      } else if (n == WIFI_SCAN_FAILED && elapsed < 1500) {
        // Genuine early failure; scanDelete also leaves FAILED set so
        // gate on elapsed.
        Serial.println("[picker] scanComplete=FAILED, retrying");
        WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
        g_state_started = fast_millis();
        last_secs_shown = 0;
      }
      break;
    }

    case State::Main: {
      if (WiFi.status() != WL_CONNECTED) {
        enter_state(State::ConnectingSaved);
        break;
      }
      if (Settings::g_screensaver_secs > 0 &&
          lv_display_get_inactive_time(nullptr) >=
            (uint32_t)Settings::g_screensaver_secs * 1000) {
        request_state_change(State::Screensaver);
        break;
      }
      const uint32_t now = fast_millis();
      if (now - g_last_stations_tick_ms >= 1000) {
        g_last_stations_tick_ms = now;
        if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
          Stations::tick(now);
          xSemaphoreGive(Stations::g_mux);
        }
      }
      // Dirty flag or 2 s fallback for "Ns ago" labels.
      if (Stations::g_list_dirty || now - g_last_main_refresh_ms >= 2000) {
        g_last_main_refresh_ms = now;
        Stations::g_list_dirty = false;
        Ui::main_refresh_list();
      }
      break;
    }

    case State::Detail: {
      if (WiFi.status() != WL_CONNECTED) {
        enter_state(State::ConnectingSaved);
        break;
      }
      const uint32_t now = fast_millis();
      if (now - g_last_stations_tick_ms >= 1000) {
        g_last_stations_tick_ms = now;
        if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
          Stations::tick(now);
          xSemaphoreGive(Stations::g_mux);
        }
      }
      if (now - g_last_detail_refresh_ms >= 1000) {
        g_last_detail_refresh_ms = now;
        Ui::detail_refresh();
      }
      break;
    }

    case State::Info: {
      const uint32_t now = fast_millis();
      if (now - g_last_info_refresh_ms >= 1000) {
        g_last_info_refresh_ms = now;
        Ui::info_refresh();
      }
      break;
    }

    default:
      break;
  }
}

}  // anonymous namespace

void request_state_change(State s) {
  g_pending_state    = s;
  g_state_change_req = true;
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.printf("%s (%ldx%ld) boot\n",
                Board::NAME, (long)Board::SCREEN_W, (long)Board::SCREEN_H);

  Util::fast_millis_begin();
  CrashDump::init();

  Theme::init();
  Settings::load();
  Theme::apply((Theme::Mode)Settings::g_theme_mode);
  Led::init();
  Audio::init();
  Stations::init();
  MontePi::init();
  Discovery::start_task();
  Poller::start_task();
  UdpRx::start_task();
  Display::init();

  // Soft-ramp the backlight; an instant LEDC step kicks the 5V rail
  // and couples a click through the speaker amp.
  {
    const float target = Settings::g_backlight;
    Display::set_backlight(0.0f);
    for (int step = 1; step <= 20; step++) {
      Display::set_backlight(target * (float)step / 20.0f);
      delay(3);
    }
  }

  Ui::build_splash();
  Ui::build_picker();
  Ui::update_crt_corners();

  // POST cinematic for any non-Default theme; Default keeps the long SVG splash.
  const bool show_post = (Settings::g_theme_mode != (uint8_t)Theme::Mode::Default);
  enter_state(show_post ? State::Post : State::Splash);
}

void loop() {
  uint32_t lv_next = lv_timer_handler();
  state_tick();
  WebSrv::loop();
  static uint32_t s_last_led_ms = 0;
  const uint32_t now = fast_millis();
  if (now - s_last_led_ms >= 1000) {
    s_last_led_ms = now;
    Led::tick();
  }

  // After 30 s of stable run, mark the running slot valid; otherwise
  // ESP-IDF rolls back to the previous slot on next boot.
  static bool s_ota_marked = false;
  if (!s_ota_marked && now > 30000) {
    s_ota_marked = true;
    const esp_partition_t * running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
      if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        Serial.println("[ota] marked app valid, rollback cancelled");
      } else {
        Serial.println("[ota] mark_app_valid failed");
      }
    }
  }

  // LEDC owned by this thread; flags from core 0 cross here.
  if (Poller::g_radmon_alarm_pending) {
    const uint8_t down  = Poller::g_radmon_alarm_down;
    const uint8_t known = Poller::g_radmon_alarm_known;
    Poller::g_radmon_alarm_pending = false;
    Poller::g_radmon_alarm_idx     = -1;
    Serial.printf("[klaxon] radmon majority down %u/%u\n", down, known);
    Ui::show_radmon_alarm(down, known);
    Audio::klaxon();
  }

  if (UdpRx::g_cpm_alarm_pending) {
    const int idx = UdpRx::g_cpm_alarm_idx;
    UdpRx::g_cpm_alarm_pending = false;
    UdpRx::g_cpm_alarm_idx     = -1;
    Serial.printf("[klaxon] cpm alert station=%d\n", idx);
    Ui::show_cpm_alarm(idx);
    Audio::klaxon();
  }
#ifdef DEBUG_GADGET
  // Heap + poll/UDP deltas. If [hb] stops, this thread is wedged.
  static uint32_t s_last_hb_ms     = 0;
  static uint32_t s_last_hb_polls  = 0;
  static uint32_t s_last_hb_pkts   = 0;
  if (now - s_last_hb_ms >= 2000) {
    s_last_hb_ms = now;
    const uint32_t polls = Poller::g_poll_count;
    const uint32_t pkts  = UdpRx::g_pkt_count;
    multi_heap_info_t hi;
    heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("[hb] t=%lu free=%u min=%u big=%u polls+%lu udp+%lu\n",
                  (unsigned long)now,
                  (unsigned)hi.total_free_bytes,
                  (unsigned)hi.minimum_free_bytes,
                  (unsigned)hi.largest_free_block,
                  (unsigned long)(polls - s_last_hb_polls),
                  (unsigned long)(pkts  - s_last_hb_pkts));
    s_last_hb_polls = polls;
    s_last_hb_pkts  = pkts;
  }
#endif
  if (lv_next < 5)  lv_next = 5;
  if (lv_next > 50) lv_next = 50;
  delay(lv_next);
}
