// Micro motion: continuous, non-repeating pupil drift.
//
// What it does: every drift_hold ms, picks a new random target inside
// DRIFT_RANGE_PX and smooths the pupil toward it with a critically-damped
// low-pass filter (tau = DRIFT_TAU_S). Reads no other behaviors directly;
// instead it reads:
//
//   * Modulators: emotions can stretch tau / shrink range / hold longer
//                 (e.g. sleepy makes the eyes drift slower and shorter).
//   * GazeIntent: external gaze targets blend into the smoother so the
//                 pupil follows real-world targets without ever locking on.
//
// Adding a new "look at" override later means writing a Modulator (e.g.
// freeze drift while a curiosity behavior holds the gaze) — never touching
// micro_motion's internals.

#pragma once

#include <Arduino.h>

#include "../eye_pose.h"

struct MicroMotionState {
  float    drift_x;        // current smoothed offset, pixels
  float    drift_y;
  float    drift_tx;       // current target the smoother is easing toward
  float    drift_ty;
  uint32_t drift_next_ms;  // when to roll a new target
};

void microMotionInit(MicroMotionState &s, uint32_t now_ms);

// Smooths drift_x/drift_y toward (drift_tx, drift_ty) every frame, and
// re-rolls (tx, ty) when the hold timer expires. Modulators scale tau,
// range, and hold-duration. GazeIntent biases the smoother toward an
// external target.
void microMotionUpdate(MicroMotionState &s,
                       const Modulators  &mods,
                       const GazeIntent  &gaze,
                       uint32_t           now_ms,
                       float              dt);
