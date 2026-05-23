// Data contracts shared between the orchestrator, behaviors, and renderer.
//
// Three structs live here. They have no methods (other than `neutral()`) and
// no animation logic — they're just typed buses that decouple the layers.
//
//   EyePose      = the *output* of the behavior pipeline. The renderer reads
//                  ONLY this + the static Eye geometry. Switching renderers
//                  (LCD, RGB matrix, simulator) means re-implementing one
//                  function that consumes EyePose; behaviors don't change.
//
//   Modulators   = the *bus* between emotion-style behaviors (sleepy,
//                  surprise, happy, ...) and motion-style behaviors (drift,
//                  blink, ...). Emotions write into it; motion behaviors
//                  read it. This is what lets emotions stack additively
//                  without any direct cross-references.
//
//   GazeIntent   = an *input* set from outside (face detector, IMU, ...).
//                  Micro motion blends this with random drift, so the eyes
//                  follow real-world targets but never freeze.
//
// Order in the update pipeline:
//   1. emotion behaviors update their internal scalars  (private state)
//   2. emotion behaviors write into Modulators          (shared bus)
//   3. motion behaviors update internal state, reading Modulators + GazeIntent
//   4. composePose() reads behavior states + Modulators -> EyePose
//   5. renderer reads (Eye geometry, EyePose) -> pixels

#pragma once

#include <Arduino.h>

// The final, per-frame description of how a binocular pair of eyes should
// be drawn. Renderer = pure function of (Eye geometry, EyePose).
//
// Lid amounts are dimensionless ratios in [0, 1]:
//   0 = fully open, 1 = fully closed (lid centerline at the far edge of the
//   eye). The renderer multiplies by the per-eye sclera height to get
//   absolute pixel offsets — so the same pose draws correctly on any-sized
//   eye, and asymmetric / future per-eye poses just need their own copies.
//
// Pupil offsets are in pixels (the eye is small enough that fixed-pixel
// drift reads the same on both sides; if you ever scale eyes asymmetrically
// you'd add per-eye pose copies).
//
// `pupil_scale` and `eye_open_amount` are forward-compatible hooks. They're
// 1.0 in the current pipeline and the renderer treats them as multipliers;
// surprise/wide-eyes/squint behaviors land cleanly on these later without
// touching anything else.
struct EyePose {
  float pupil_dx;          // pixels, additive offset from neutral pupil pos
  float pupil_dy;          // pixels
  // Per-eye additive offsets on top of pupil_dx/dy. Idle gaze writes here to
  // inject tiny natural asymmetry — both eyes share the same chosen target,
  // but each side has its own smoothing tau and a small target jitter, so
  // during transitions the pupils arrive at slightly different times. At
  // rest, both extras converge to the same point.
  //
  // Renderer selects which extra to apply based on `Eye::side`. Behaviors
  // that don't care about asymmetry (e.g. micro motion) keep writing to
  // pupil_dx/dy as before.
  float pupil_dx_l_extra;
  float pupil_dy_l_extra;
  float pupil_dx_r_extra;
  float pupil_dy_r_extra;
  float pupil_scale;       // 1.0 = neutral; future: surprise/dilation scale
  float upper_lid_amount;  // 0..1, fraction of eye height covered from top
  float lower_lid_amount;  // 0..1, fraction of eye height covered from bot
  float eye_open_amount;   // 1.0 = neutral; future: wide-eyes (>1) / squint
};

// Modulator bus written by emotion-style behaviors, read by motion-style
// behaviors and composePose(). Contributions compose multiplicatively for
// "rate" knobs (so multiple emotions slowing things down stack as expected:
// sleepy 1.5x * tired 1.2x = 1.8x slower) and additively for spatial offsets
// (a sleepy droop and a wince each push the lid down independently).
//
// Always start a frame from `neutral()` and let each emotion write into it.
// Behaviors should never branch on which emotion is active — they only read
// the post-stack values.
struct Modulators {
  // Drift / micro-motion timing (multipliers; 1.0 = neutral).
  float drift_tau_mult;          // >1 = slower easing toward target
  float drift_range_mult;        // <1 = wander less far
  float drift_hold_mult;         // >1 = hold each target longer

  // Blink timing (multipliers; 1.0 = neutral).
  float blink_duration_mult;     // >1 = slower blink (close/hold/open scaled)

  // Lid spatial contributions (additive in normalized [0..1] lid units).
  float lid_upper_droop;         // upper lid extra droop (0..1)
  float lid_lower_rise;          // lower lid extra rise  (0..1)

  // Pupil spatial contributions.
  float pupil_y_bias_px;         // additive y bias in pixels
  float pupil_scale_mult;        // multiplier on pupil scale (1.0 = neutral)

  // Long-blink (extended hold) controls. Multiple emotions can raise the
  // chance; the most-recently-written extra-hold range wins (good enough
  // until two emotions actually fight over this).
  float    long_blink_chance;        // 0..1
  uint16_t long_blink_extra_min_ms;
  uint16_t long_blink_extra_max_ms;

  // Master eye-open additive contribution (forward-compat, currently
  // composePose just propagates into pose.eye_open_amount = 1 + this).
  float eye_open_add;

  // Build a no-op modulator stack — every multiplier 1, every additive 0.
  // Helper rather than a constructor so callers can `Modulators m = neutral()`
  // (works in C++14, no aggregate-init of all twelve fields needed).
  static Modulators neutral() {
    Modulators m{};
    m.drift_tau_mult           = 1.0f;
    m.drift_range_mult         = 1.0f;
    m.drift_hold_mult          = 1.0f;
    m.blink_duration_mult      = 1.0f;
    m.lid_upper_droop          = 0.0f;
    m.lid_lower_rise           = 0.0f;
    m.pupil_y_bias_px          = 0.0f;
    m.pupil_scale_mult         = 1.0f;
    m.long_blink_chance        = 0.0f;
    m.long_blink_extra_min_ms  = 0;
    m.long_blink_extra_max_ms  = 0;
    m.eye_open_add             = 0.0f;
    return m;
  }
};

// External gaze input. Set from face detection, IMU, scripted scenes, etc.
// `target_*` are in the same coordinate space as drift (pixels of pupil
// offset from neutral). `weight` blends between random drift and gaze:
//
//   weight = 0.0  -> ignored, micro motion runs normally
//   weight = 0.5  -> drift smooths halfway toward gaze target
//   weight = 1.0  -> drift smooths fully to gaze target (random ignored)
//
// Smoothing happens inside micro_motion, so updating this struct every frame
// from a face tracker yields organic eye follow without judder.
struct GazeIntent {
  bool  active;
  float target_x;
  float target_y;
  float weight;
};
