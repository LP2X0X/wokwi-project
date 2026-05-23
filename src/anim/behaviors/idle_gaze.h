// Idle gaze: autonomous "looking around" on top of the always-on micro motion.
//
// Where micro_motion provides the constant 1-2 px wobble that keeps the eye
// from looking dead, idle gaze schedules occasional larger glances with
// natural pauses in between — the difference between vibration and attention.
// They compose additively at render time, so blinking and sleepy lids stay
// fully in charge of their own layers; idle gaze only ever moves the pupils.
//
// State machine, one event at a time:
//   HOLDING : target sits still; the smoother eases the eye to it; long pause.
//   MOVING  : a new target was picked; smoother is in flight; short window.
//
// Target picking is weighted (~60% small, ~30% medium, ~10% large with some
// vertical) so most movement reads as a subtle drift and only rarely as a
// definite look. The character "spends most of its time resting" because the
// HOLDING phase is deliberately long.
//
// Asymmetry comes from
//   * per-eye smoothing taus (each move samples a slightly different tau
//     per eye, so the pupils arrive at the target a beat apart),
//   * a tiny per-eye target jitter (sub-pixel; reads as life, not bug).
// Both eyes share the same schedule because real eyes are coordinated; the
// asymmetry is in HOW they get there, not in whether they move.
//
// Reads Modulators:
//   * drift_range_mult  - sleepy shrinks the glance amplitude
//   * drift_hold_mult   - sleepy stretches the holds
//   * drift_tau_mult    - sleepy slows the movement
// Reads GazeIntent: when an external gaze signal is active (face tracker,
// scripted scene, ...) idle gaze fades its offset toward zero so the
// external signal becomes the dominant source. When it releases, idle gaze
// resumes from wherever the next phase tick lands.

#pragma once

#include <Arduino.h>

#include "../eye_pose.h"

struct IdleGazeState {
  enum Phase : uint8_t { Holding = 0, Moving = 1 };

  Phase    phase;
  uint32_t phase_until_ms;     // when current phase ends

  // Per-eye smoothed offsets in pixels (added on top of micro_motion drift,
  // applied by the renderer based on Eye::side).
  float gx_l, gy_l;
  float gx_r, gy_r;

  // Per-eye easing targets — same chosen point ± tiny jitter for each eye.
  float tx_l, ty_l;
  float tx_r, ty_r;

  // Per-eye easing time constants, sampled fresh at each move start.
  float tau_l;
  float tau_r;
};

void idleGazeInit(IdleGazeState &s, uint32_t now_ms);

// Per-frame update. Always smooths the offsets; re-rolls targets / advances
// the state machine only when an event is due. Modulators drive sleepy
// timing/scaling; GazeIntent (when active) causes idle gaze to fade out.
void idleGazeUpdate(IdleGazeState    &s,
                    const Modulators &mods,
                    const GazeIntent &gaze,
                    uint32_t          now_ms,
                    float             dt);
