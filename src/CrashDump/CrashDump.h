/*
  CrashDump.h - boot-time read of reset reason + coredump summary

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

// Snapshots the last-reset reason and any flash coredump summary at boot,
// so the /crash web page can show a post-mortem for the previous run.

#pragma once

#include <esp_err.h>
#include <esp_system.h>
#include <stdint.h>

namespace CrashDump {

struct Info {
  esp_reset_reason_t reset_reason;
  bool               has_coredump;
  uint32_t           exc_pc;
  uint32_t           exc_cause;
  uint32_t           exc_vaddr;
  char               task[16];
  uint8_t            bt_depth;
  bool               bt_corrupted;
  uint32_t           bt[16];
};

void          init();
const Info &  info();
const char *  reset_reason_str();
esp_err_t     erase();  // wipes the coredump partition

}  // namespace CrashDump
