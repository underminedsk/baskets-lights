// Pattern rendering: turns the pure pattern math (pattern_math.h) into SK6812
// RGBW colors on a NeoPixelBus strip. The time/space behavior lives in pmath::
// so it can be unit-tested; this layer only maps intensity -> pixels.
#pragma once

#include <Arduino.h>
#include <NeoPixelBus.h>

#include "beacon.h"
#include "pattern_math.h"

namespace patterns {

enum PatternId : uint16_t {
  PULSE = 0,         // slow global brightness pulse
  PALETTE_DRIFT = 1  // hue drift across a palette (later)
};

// Slow breathing pulse in the white channel — calm and power-efficient by design.
inline RgbwColor pulse(int64_t synced_us, uint8_t brightness, float x, float y) {
  const float period_s = 4.0f;
  const float spatial = (x + y) * 0.05f;  // 0 for Milestone 1
  float s = pmath::pulseIntensity(synced_us, period_s, spatial);
  uint8_t w = (uint8_t)lroundf(s * brightness);
  return RgbwColor(0, 0, 0, w);
}

// Render one pattern into a NeoPixelBus strip.
template <typename StripT>
inline void render(StripT& strip, const Beacon& b, int64_t synced_us, float x,
                   float y) {
  switch (b.pattern_id) {
    case PULSE:
    default: {
      RgbwColor c = pulse(synced_us, b.brightness, x, y);
      for (uint16_t i = 0; i < strip.PixelCount(); i++) strip.SetPixelColor(i, c);
      break;
    }
  }
}

}  // namespace patterns
