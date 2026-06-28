// Pure pattern math — the time/space functions behind each pattern, with no
// dependency on the LED library. Patterns are f(x, y, t), so a pulse can travel
// across the physical field once real (x,y) coordinates arrive in Milestone 2.
// Kept separate from patterns.h so the wrap/continuity behavior is host-testable.
#pragma once

#include <math.h>
#include <stdint.h>

namespace pmath {

static constexpr float kPi = 3.14159265358979323846f;

// Map microseconds to a phase in [0,1) over `period_s` seconds. Continuous and
// monotonic within a period; wraps cleanly at the boundary (no visible hitch).
inline float phase(int64_t t_us, float period_s) {
  double secs = (double)t_us / 1e6;
  double p = fmod(secs / (double)period_s, 1.0);
  if (p < 0) p += 1.0;  // fmod keeps the sign of the dividend; force [0,1)
  return (float)p;
}

// Breathing pulse intensity in [0,1]: a smooth raised cosine. `spatial` shifts
// the phase per-node so the field can ripple; it is 0 for Milestone 1.
inline float pulseIntensity(int64_t synced_us, float period_s, float spatial) {
  float p = phase(synced_us, period_s) + spatial;
  return 0.5f * (1.0f - cosf(2.0f * kPi * p));
}

}  // namespace pmath
