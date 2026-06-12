/*
  Audio.h - PWM-as-DAC click voice

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

// LEDC PWM-as-DAC click voice on GPIO 26 (CYD SPEAK pin). Mirrors the
// parent firmware's AudioTick mode 0 voice. Speaker's mechanical low-pass
// converts the 40 kHz carrier's duty stream into audible signal.

#pragma once

#include <stdint.h>

namespace Audio {

constexpr int      kSpeakerPin   = 26;
constexpr int      kLedcChannel  = 0;       // backlight has ch 7
constexpr uint8_t  kLedcRes      = 8;

constexpr uint16_t kClickCarrierHz     = 40000;
constexpr uint16_t kClickSampleRateHz  = 20000;
constexpr uint16_t kClickSampleCount   = 200;
constexpr uint16_t kClickSampleUs      = 50;

constexpr float    kClickBaseFreqHz    = 2500.0f;
constexpr float    kClickAmpTauSec     = 0.010f;
constexpr float    kClickChirpTauSec   = 0.0005f;
constexpr float    kClickNoiseMix      = 0.10f;
constexpr uint16_t kClickAttackSamples = 5;
constexpr uint16_t kClickExitSamples   = 12;

// Bias 128 -> 32 stopped the click peak sagging VDD and dipping the
// display. AC swing unchanged.
constexpr uint8_t  kClickPeakBias = 32;
// Same fix: 22 = no rail sag, higher reproduces the dip.
constexpr uint8_t  kClickDutyMax  = 22;

void init();
void click();
void beep(uint16_t freq_hz, uint16_t duration_ms);

// Blocking, ~500 ms.
void klaxon();

}  // namespace Audio
