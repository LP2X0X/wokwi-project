// Sleepy: a single 0..1 mood scalar that other behaviors read as a multiplier.
//
// This is the canonical example of an "emotion" behavior:
//   * Owns one scalar (`amount`) plus a re-roll schedule.
//   * `update()` evolves the scalar autonomously (mood drift + smoothing).
//   * `modulate()` is a pure function from this state to the Modulator bus —
//     it never mutates anything else, never reads other behaviors.
//
// To add a new emotion (surprise, curious, sad, ...) copy this file:
//   1. Pick the scalar(s) it owns.
//   2. Implement update() — autonomous behavior, external setter, etc.
//   3. Implement modulate() — write into Modulators (additive offsets,
//      multiplicative rates). Never branch elsewhere on which emotion is
//      active; everything ends up in Modulators.

#pragma once

#include <Arduino.h>

#include "../eye_pose.h"

struct SleepyState {
  // Smoothed sleepy amount in [0, 1]. 0 = wide awake, 1 = barely keeping
  // eyes open. Other behaviors NEVER read this directly — sleepyModulate()
  // translates it into Modulator contributions on every frame.
  float    amount;

  // What `amount` is easing toward. Set by autonomous mood drift, or by
  // sleepySet() (external override — light sensor, time of day, etc.).
  float    target;

  // When to autonomously re-roll `target`. The toy holds one mood for
  // SLEEPY_HOLD_MIN..MAX seconds before drifting to a new bucket.
  uint32_t next_ms;
};

void sleepyInit(SleepyState &s, uint32_t now_ms);

// External override. Will be slowly overwritten by autonomous mood drift —
// call once for "stay sleepy for a while", or every frame to pin it
// (sensor-driven mode).
void sleepySet(SleepyState &s, float target);

// Updates `amount` by easing toward `target`, and re-rolls `target`
// occasionally so the toy doesn't sit at one mood forever.
void sleepyUpdate(SleepyState &s, uint32_t now_ms, float dt);

// Pure function: write this state's contributions into the modulator bus.
// Called every frame after sleepyUpdate() and before any motion behavior
// reads `mods`. Multiple emotions stack into the same `mods` instance.
void sleepyModulate(const SleepyState &s, Modulators &mods);
