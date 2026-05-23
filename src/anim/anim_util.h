// Tiny, header-only math helpers shared by every behavior + the renderer.
//
// Rule: nothing in this header should know about EyeState, Eye, Modulators,
// or any animation type. These are pure scalar utilities so any module can
// pull them in without creating circular dependencies.

#pragma once

#include <Arduino.h>

namespace anim_util {

// Uniform float in [lo, hi]. Resolution is 0.0001 — plenty for animation
// timing, keeps the Arduino RNG fast.
inline float frand(float lo, float hi) {
  return lo + (hi - lo) * (random(0, 10001) / 10000.0f);
}

// Inclusive uniform unsigned. Mirrors the half-open Arduino API but lets
// callers pass natural ranges (e.g. 90..180 ms).
inline uint32_t urand(uint32_t lo, uint32_t hi) {
  return random(lo, hi + 1);
}

// Cubic Hermite smoothstep clamped to [0, 1]. Used for ease in/out curves
// where a linear ramp would feel mechanical.
inline float smoothstep01(float x) {
  if (x <= 0.0f) return 0.0f;
  if (x >= 1.0f) return 1.0f;
  return x * x * (3.0f - 2.0f * x);
}

// Linear interpolate. t is NOT clamped — callers that need clamping should
// pass a clamped t (allows over/undershoot for spring-style effects later).
inline float lerp(float a, float b, float t) {
  return a + (b - a) * t;
}

inline float clamp01(float x) {
  return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

inline float clampf(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

}  // namespace anim_util
