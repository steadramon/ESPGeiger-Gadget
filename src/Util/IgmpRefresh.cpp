/*
  IgmpRefresh.cpp - see header.

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

#include "IgmpRefresh.h"

#include <Arduino.h>
#include <WiFi.h>
#include <tcpip_adapter.h>
#include <lwip/igmp.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>

namespace IgmpRefresh {

namespace {

// Runs in the lwIP tcpip thread (via tcpip_callback). Walks the netif's joined
// group list and re-sends a Membership Report for each, bypassing the BSD
// socket layer. beginMulticast() cannot be trusted to do this after long
// uptime on this lwIP build.
void report_groups_cb(void * ctx) {
  igmp_report_groups((struct netif *)ctx);
}

}  // namespace

void refresh() {
  if (WiFi.status() != WL_CONNECTED) return;
  void * nif = nullptr;
  if (tcpip_adapter_get_netif(TCPIP_ADAPTER_IF_STA, &nif) != ESP_OK || !nif) {
    Serial.println("[udp] IgmpRefresh: no STA netif");
    return;
  }
  // Marshal into the tcpip thread; igmp_report_groups() touches the netif group
  // list that the stack also walks, so it must not run from this task directly.
  if (tcpip_callback(report_groups_cb, nif) != ERR_OK) {
    Serial.println("[udp] IgmpRefresh: tcpip mailbox full");
  }
}

}  // namespace IgmpRefresh
