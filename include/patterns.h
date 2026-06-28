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
  PULSE = 0,         // uniform slow breathing pulse (all nodes in unison)
  PALETTE_DRIFT = 1, // hue drift across a palette (later)
  SWEEP = 2          // brightness wave that travels across the field by position
};

// Uniform breathing pulse in the white channel — every node in unison.
inline RgbwColor pulse(int64_t synced_us, uint8_t brightness) {
  float s = pmath::pulseIntensity(synced_us, /*period_s*/ 4.0f, /*spatial*/ 0.0f);
  return RgbwColor(0, 0, 0, (uint8_t)lroundf(s * brightness));
}

// Position-aware sweep: a wave of brightness travels across the field, so the
// pulse physically moves from lantern to lantern. params let the conductor tune
// it live: params[0] = period in ms, params[1] = wavelength in hundredths of a
// coordinate unit (both fall back to sensible defaults when 0).
inline RgbwColor sweep(int64_t synced_us, uint8_t brightness, float x, float y,
                       const uint16_t params[4]) {
  float period_s = params[0] ? params[0] / 1000.0f : 4.0f;
  float wavelength = params[1] ? params[1] / 100.0f : 3.0f;
  float s = pmath::sweepIntensity(synced_us, x, period_s, wavelength);
  return RgbwColor(0, 0, 0, (uint8_t)lroundf(s * brightness));
}

// Render one pattern into a NeoPixelBus strip (all pixels share one color for
// these 16-pixel rings; per-pixel spatial effects can come later).
template <typename StripT>
inline void render(StripT& strip, const Beacon& b, int64_t synced_us, float x,
                   float y) {
  RgbwColor c;
  switch (b.pattern_id) {
    case SWEEP:
      c = sweep(synced_us, b.brightness, x, y, b.params);
      break;
    case PULSE:
    default:
      c = pulse(synced_us, b.brightness);
      break;
  }
  for (uint16_t i = 0; i < strip.PixelCount(); i++) strip.SetPixelColor(i, c);
}

}  // namespace patterns
