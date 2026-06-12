/*
  WebServer.h - diagnostics web UI

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

// Tiny HTTP server exposing:
//   GET /       - status page (IP, FW version, station count, link to /shot)
//   GET /shot   - current screen as a 24-bit BMP (line-by-line readback)
//
// Sync arduino-esp32 WebServer on port 80. ~12 KB flash, single-connection,
// no concurrency. Start once after WiFi is up; pump WebServer::loop() from
// the Arduino main loop.

#pragma once

namespace WebSrv {

// Construct + start the server. Idempotent.
void init();

// Pump pending requests. Call from loop() while WiFi is up.
void loop();

}  // namespace WebSrv
