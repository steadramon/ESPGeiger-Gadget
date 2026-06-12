/*
  MontePi.cpp - Monte Carlo Pi estimator driven by UDP click timing

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

#include "MontePi.h"

#include <Arduino.h>
#include <string.h>

namespace MontePi {

volatile uint64_t g_decays = 0;
volatile uint64_t g_darts  = 0;
volatile uint64_t g_inside = 0;

namespace {

SemaphoreHandle_t s_mux = nullptr;
int16_t           s_ring_x[kRingLen] = {0};
int16_t           s_ring_y[kRingLen] = {0};
size_t            s_ring_head  = 0;
size_t            s_ring_count = 0;

// xorshift32; mixes the (ts_ms, counter) entropy so the LSBs are usable
// as independent x/y halves.
inline uint32_t mix(uint32_t a, uint32_t b) {
  uint32_t s = a ^ (b * 0x9E3779B1u);
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

}  // anonymous namespace

void init() {
  if (!s_mux) s_mux = xSemaphoreCreateMutex();
}

// Each credited decay seeds this many independent dart samples (different
// xorshift seed mixings of the same source entropy). Trade-off: 1 = pure
// physics, ~3 matching digits after a month; 16 = "decay event entropy
// expanded to 16 samples", ~5 matching digits after a month.
constexpr uint32_t kDartsPerClick = 16;

void throw_darts(uint32_t seed, uint32_t count) {
  if (!s_mux || count == 0) return;
  const uint32_t dart_count = count * kDartsPerClick;
  if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(20)) != pdTRUE) return;
  g_decays += count;
  for (uint32_t i = 0; i < dart_count; i++) {
    // Independent derivations so x and y are uncorrelated.
    // (The XOR salts are the leading digits of pi, for the obvious reason.)
    const uint32_t r1 = mix(seed + i * 2654435761u, i ^ 0x31415926u);
    const uint32_t r2 = mix(seed + i * 0x85EBCA6Bu, ~i ^ 0x53589793u);
    const int16_t x = (int16_t)(r1 >> 16);
    const int16_t y = (int16_t)(r2 >> 16);
    const int64_t r2_sq = (int64_t)x * x + (int64_t)y * y;
    const int64_t one   = (int64_t)32767 * 32767;
    g_darts++;
    if (r2_sq <= one) g_inside++;
    s_ring_x[s_ring_head] = x;
    s_ring_y[s_ring_head] = y;
    s_ring_head = (s_ring_head + 1) % kRingLen;
    if (s_ring_count < kRingLen) s_ring_count++;
  }
  xSemaphoreGive(s_mux);
}

void snapshot(Snapshot & out) {
  if (!s_mux) {
    memset(&out, 0, sizeof(out));
    return;
  }
  if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(10)) != pdTRUE) {
    memset(&out, 0, sizeof(out));
    return;
  }
  out.decays       = g_decays;
  out.darts        = g_darts;
  out.inside       = g_inside;
  out.recent_head  = s_ring_head;
  out.recent_count = s_ring_count;
  memcpy(out.recent_x, s_ring_x, sizeof(s_ring_x));
  memcpy(out.recent_y, s_ring_y, sizeof(s_ring_y));
  xSemaphoreGive(s_mux);
}

}  // namespace MontePi
