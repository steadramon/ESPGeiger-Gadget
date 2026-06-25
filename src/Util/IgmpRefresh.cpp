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
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <lwip/igmp.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>

namespace IgmpRefresh {

namespace {

// Must run on the tcpip thread: igmp_report_groups() mutates the netif group
// list that the stack also walks.
void report_groups_cb(void * ctx) {
  igmp_report_groups((struct netif *)ctx);
}

}  // namespace

void refresh() {
  if (WiFi.status() != WL_CONNECTED) return;
  esp_netif_t * sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  struct netif * nif = sta ? (struct netif *)esp_netif_get_netif_impl(sta) : nullptr;
  if (!nif) {
    Serial.println("[udp] IgmpRefresh: no STA netif");
    return;
  }
  if (tcpip_callback(report_groups_cb, nif) != ERR_OK) {
    Serial.println("[udp] IgmpRefresh: tcpip mailbox full");
  }
}

}  // namespace IgmpRefresh
