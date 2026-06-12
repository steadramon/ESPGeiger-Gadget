// LVGL 9.x configuration for ESPGeiger Gadget (Sunton 2.8" CYD, 240x320).
//
// Picked up via -DLV_CONF_PATH in platformio.ini.

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

// We feed lv_tick via lv_tick_set_cb(millis) in setup(); no OS thread.
#define LV_USE_OS LV_OS_NONE
#define LV_DEF_REFR_PERIOD 30

// Use the IDF general heap (malloc/free) instead of LVGL's built-in
// fixed-size pool. The fixed pool capped at 40 KB by the dram0 segment
// ceiling fragmented under widget churn and starved the Settings page.
// IDF heap has ~80 KB free at boot and uses TLSF, which fragments far
// less. LV_MEM_SIZE is ignored under CLIB mode.
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING  LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
// Compiled in for the Dice mini-app's roll value - Theme::f_title at
// montserrat_28 felt small for a "headline" number on the centre of
// the screen. ~30 KB flash cost.
#define LV_FONT_MONTSERRAT_48 1
// UNSCII bitmap fonts (CC0). UNSCII_16 is 16-wide x 16-tall; UNSCII_8 is
// 8x8 and is used by the Gadget-mode POST screen so the 240 px panel can
// hold a full mono boot-log line.
#define LV_FONT_UNSCII_16 1
#define LV_FONT_UNSCII_8  1
#define LV_FONT_DEFAULT &lv_font_montserrat_16

#define LV_USE_SYSMON       0
#define LV_USE_PERF_MONITOR 0

// Logging - toggled up via DEBUG_GADGET in the *_debug envs.
#ifdef DEBUG_GADGET
  #define LV_USE_LOG 1
  #define LV_LOG_LEVEL LV_LOG_LEVEL_INFO
  #define LV_LOG_PRINTF 1
#else
  #define LV_USE_LOG 0
#endif

// Default dark theme; overridden in code with the brand palette.
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 1
#define LV_THEME_DEFAULT_TRANSITION_TIME 80

// Widgets actually used.
#define LV_USE_LABEL    1
#define LV_USE_BUTTON   1
#define LV_USE_LIST     1
#define LV_USE_KEYBOARD 1
#define LV_USE_TEXTAREA 1
#define LV_USE_IMAGE    1
#define LV_USE_LED      1
#define LV_USE_BAR      1
#define LV_USE_ARC      1
#define LV_USE_SLIDER   1
#define LV_USE_SWITCH   1
#define LV_USE_LINE     1
#define LV_USE_CHART    1

// Software renderer - fine on ESP32 at 240x320.
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_COMPLEX 1

#endif // LV_CONF_H
