/*
  CrashDump.cpp - boot-time read of reset reason + coredump summary

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

#include "CrashDump.h"

#include <Arduino.h>
#include <esp_core_dump.h>
#include <stdlib.h>
#include <string.h>

namespace CrashDump {

namespace {
Info s_info = {};
}

void init() {
  s_info.reset_reason = esp_reset_reason();

  // Summary is ~2 KB; heap is freshest at boot, transient alloc.
  esp_core_dump_summary_t * sum =
    (esp_core_dump_summary_t *)calloc(1, sizeof(esp_core_dump_summary_t));
  if (!sum) return;
  if (esp_core_dump_get_summary(sum) == ESP_OK) {
    s_info.has_coredump = true;
    s_info.exc_pc       = sum->exc_pc;
    s_info.exc_cause    = sum->ex_info.exc_cause;
    s_info.exc_vaddr    = sum->ex_info.exc_vaddr;
    strncpy(s_info.task, sum->exc_task, sizeof(s_info.task) - 1);
    s_info.task[sizeof(s_info.task) - 1] = '\0';
    uint32_t depth = sum->exc_bt_info.depth;
    if (depth > 16) depth = 16;
    s_info.bt_depth     = (uint8_t)depth;
    s_info.bt_corrupted = sum->exc_bt_info.corrupted;
    for (uint8_t i = 0; i < s_info.bt_depth; i++) {
      s_info.bt[i] = sum->exc_bt_info.bt[i];
    }
    Serial.printf("[crash] coredump found: task=%s pc=0x%08lx cause=%lu\n",
                  s_info.task, (unsigned long)s_info.exc_pc,
                  (unsigned long)s_info.exc_cause);
  }
  free(sum);

  Serial.printf("[crash] reset reason: %s (%d)\n",
                reset_reason_str(), (int)s_info.reset_reason);
}

const Info & info() { return s_info; }

const char * reset_reason_str() {
  switch (s_info.reset_reason) {
    case ESP_RST_POWERON:   return "Power-on";
    case ESP_RST_EXT:       return "External pin";
    case ESP_RST_SW:        return "Software";
    case ESP_RST_PANIC:     return "Exception/panic";
    case ESP_RST_INT_WDT:   return "Interrupt WDT";
    case ESP_RST_TASK_WDT:  return "Task WDT";
    case ESP_RST_WDT:       return "Other WDT";
    case ESP_RST_DEEPSLEEP: return "Deep sleep";
    case ESP_RST_BROWNOUT:  return "Brownout";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "Unknown";
  }
}

esp_err_t erase() {
  const esp_err_t rc = esp_core_dump_image_erase();
  if (rc == ESP_OK) {
    s_info.has_coredump = false;
  }
  return rc;
}

}  // namespace CrashDump
