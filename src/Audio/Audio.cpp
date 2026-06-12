/*
  Audio.cpp - PWM-as-DAC click voice

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

#include "Audio.h"

#include <Arduino.h>
#include <math.h>

#include "../Settings/Settings.h"
#include "../Util/FastMillis.h"

namespace Audio {

namespace {
bool          s_inited        = false;
uint8_t       s_tokens        = 5;
uint32_t      s_last_token_ms = 0;
constexpr uint32_t kMinIntervalMs = 25;     // 40 Hz cap, ~2400 cpm
}

void init() {
  if (s_inited) return;
  // Pulldown between clicks; otherwise SPI on adjacent pins couples in
  // as a faint tick.
  pinMode(kSpeakerPin, INPUT_PULLDOWN);
  ledcSetup(kLedcChannel, kClickCarrierHz, kLedcRes);
  s_inited        = true;
  s_last_token_ms = fast_millis();
}

void click() {
  if (!s_inited || !Settings::g_audio_enabled) return;
  if (Settings::g_audio_volume == 0) return;

  // Token bucket; one per kMinIntervalMs idle, depth 5.
  const uint32_t now = fast_millis();
  const uint32_t elapsed = now - s_last_token_ms;
  if (elapsed >= kMinIntervalMs) {
    const uint8_t gained = (uint8_t)(elapsed / kMinIntervalMs);
    uint16_t t = (uint16_t)s_tokens + gained;
    if (t > 5) t = 5;
    s_tokens = (uint8_t)t;
    s_last_token_ms = now;
  }
  if (s_tokens == 0) return;
  s_tokens--;

  // Voice math mirrors parent AudioTick::playClickChirp, output as 8-bit
  // LEDC duty against the 40 kHz carrier.
  const uint8_t peak_duty = (uint8_t)(
      (uint16_t)kClickDutyMax * Settings::g_audio_volume / 100);
  if (peak_duty == 0) return;

  const float inv_fs    = 1.0f / (float)kClickSampleRateHz;
  const float f_lo      = kClickBaseFreqHz;
  const float f_hi      = f_lo * 2.0f;
  const float env_dec   = expf(-inv_fs / kClickAmpTauSec);
  const float chirp_dec = expf(-inv_fs / kClickChirpTauSec);
  const float two_pi    = 6.2831853f;
  const float swing     = (float)peak_duty;

  float    env       = 1.0f;
  float    chirp_env = 1.0f;
  float    phase     = 0.0f;
  uint32_t nr        = (uint32_t)micros() ^ 0xC11C6BEDu;   // "clicked"-ish

  ledcAttachPin(kSpeakerPin, kLedcChannel);
  ledcWriteTone(kLedcChannel, kClickCarrierHz);
  ledcWrite(kLedcChannel, 128);

  const uint32_t start_us = micros();
  for (uint16_t i = 0; i < kClickSampleCount; i++) {
    // Downward chirp 2*f_lo -> f_lo, ~0.5 ms snap.
    const float f_inst = f_lo + (f_hi - f_lo) * chirp_env;
    phase += two_pi * f_inst * inv_fs;

    // xorshift32 -> [-1, +1).
    nr ^= nr << 13; nr ^= nr >> 17; nr ^= nr << 5;
    const float n = ((int32_t)(nr & 0xFFFF) - 32768) * (1.0f / 32768.0f);

    float eff_env = env;
    if (i < kClickAttackSamples) {
      eff_env *= (float)i / (float)kClickAttackSamples;
    }
    const uint16_t to_end = (uint16_t)(kClickSampleCount - i);
    if (to_end <= kClickExitSamples) {
      eff_env *= (float)to_end / (float)kClickExitSamples;
    }

    const float sample =
        (sinf(phase) * (1.0f - kClickNoiseMix) + n * kClickNoiseMix) * eff_env;

    const int bias = (int)((float)kClickPeakBias * eff_env);
    int duty = bias + (int)(sample * swing);
    if (duty < 0)   duty = 0;
    if (duty > 255) duty = 255;
    ledcWrite(kLedcChannel, (uint8_t)duty);

    env       *= env_dec;
    chirp_env *= chirp_dec;

    // Sample timing drifts under variable per-sample compute, so sync
    // against the loop's start time, not the previous iteration.
    while ((micros() - start_us) < ((uint32_t)i + 1) * kClickSampleUs) { }
  }

  ledcWrite(kLedcChannel, 0);
  // arduino-esp32 leaves the pin floating after detach; force pulldown.
  ledcDetachPin(kSpeakerPin);
  pinMode(kSpeakerPin, INPUT_PULLDOWN);
}

void klaxon() {
  if (!s_inited) return;
  beep(880, 200); delay(60);
  beep(660, 200); delay(60);
  beep(880, 200);
}

void beep(uint16_t freq_hz, uint16_t duration_ms) {
  if (!s_inited) return;
  ledcAttachPin(kSpeakerPin, kLedcChannel);
  ledcWriteTone(kLedcChannel, freq_hz);
  ledcWrite(kLedcChannel,
            (uint8_t)((uint16_t)kClickDutyMax * Settings::g_audio_volume / 100));
  delay(duration_ms);
  ledcWrite(kLedcChannel, 0);
  ledcDetachPin(kSpeakerPin);
  pinMode(kSpeakerPin, INPUT_PULLDOWN);
}

}  // namespace Audio
