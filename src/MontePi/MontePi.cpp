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

// splitmix64. A single xorshift round (the old approach) is linear, so its top
// bits stayed correlated with the seed counter and biased the ratio (~3.156).
uint64_t s_state = 0x9E3779B97F4A7C15ull;

inline uint64_t next64() {
  uint64_t z = (s_state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

}  // anonymous namespace

void init() {
  if (!s_mux) s_mux = xSemaphoreCreateMutex();
}

// Dart samples per credited decay. 1 = pure physics; 16 = more samples per
// decay, same 1/sqrt(N) convergence in total darts.
constexpr uint32_t kDartsPerClick = 16;

void throw_darts(uint32_t seed, uint32_t count) {
  if (!s_mux || count == 0) return;
  const uint32_t dart_count = count * kDartsPerClick;
  if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(20)) != pdTRUE) return;
  g_decays += count;
  // Fold click entropy into the stream so darts track decay timing.
  s_state ^= (uint64_t)seed * 0x9E3779B97F4A7C15ull;
  // Circle inscribed in a 2^32 square centred on origin; fraction inside -> pi/4.
  const uint64_t kR2 = (uint64_t)2147483648ull * 2147483648ull;  // (2^31)^2
  for (uint32_t i = 0; i < dart_count; i++) {
    const uint64_t a = next64();
    const int64_t dx = (int64_t)(uint32_t)(a >> 32) - 2147483648ll;
    const int64_t dy = (int64_t)(uint32_t)(a)       - 2147483648ll;
    g_darts++;
    if ((uint64_t)(dx * dx) + (uint64_t)(dy * dy) <= kR2) g_inside++;
    s_ring_x[s_ring_head] = (int16_t)(dx >> 16);
    s_ring_y[s_ring_head] = (int16_t)(dy >> 16);
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
