// Attentive / Listening: a "huh? did I hear that?" emotion.
//
// Triggered externally (sound, touch, face appearance, finger snap) with a
// direction toward the suspected source. The eyes:
//   1. Freeze briefly — a tiny "what was that" pause.
//   2. Glance toward the direction with a snappy ease.
//   3. Hold there, listening — eyes slightly wider, pupils slightly smaller,
//      gaze focus tightens, blinks soft-suppressed.
//   4. Do ONE small verification glance (subtle offset, then back) — the
//      "let me double-check that" beat that makes the reaction feel animal.
//   5. Relax smoothly back to baseline.
//
// Architecturally identical shape to curiosity:
//   * Owns one scalar amount in [0, 1] + a small phase machine + a smoothed
//     gaze offset that composePose() reads.
//   * Writes ONLY into Modulators (and into its own state read at compose
//     time). Never touches other behaviors directly.
//   * Asymmetric tau on amount (fast attack, soft release) so noticing feels
//     snappier than relaxing.
//
// No autonomous trigger — curiosity already handles spontaneous "noticing"
// and attentive is meant for sensor-driven activation. Tying it to an
// internal timer would compete with curiosity for the same animation slot.
//
// Composes cleanly with other behaviors via the modulator bus:
//   * sleepy + attentive  -> droopy creature still slightly alert
//   * curiosity + attentive -> contributions stack (wider eyes, smaller
//                              pupil) — meant for "noticed AND attentive"
//                              moments triggered close together
//   * idle_gaze remains active; its drift_range/hold/tau modulators are
//     tightened by attentive so wandering settles down without freezing.
//   * micro_motion remains active; its range/tau are similarly tightened
//     so the wobble reads as "alive but holding still."

#pragma once

#include <Arduino.h>

#include "../eye_pose.h"

struct AttentiveState {
  // Lifecycle phases. Each tick checks two things independently:
  //   * Has phase_until_ms elapsed? -> advance the phase machine.
  //   * Has end_ms elapsed (and we're not already in Relax)? -> bail to
  //     Relax even mid-phase, so a short trigger always exits cleanly.
  enum Phase : uint8_t {
    Idle    = 0,
    Freeze  = 1,
    Glance  = 2,
    Hold    = 3,
    Verify  = 4,
    Relax   = 5,
  };

  Phase    phase;
  uint32_t phase_until_ms;
  uint32_t end_ms;        // when to drop into Relax (set by trigger)
  bool     verify_done;   // we only do one verify glance per activation

  // Smoothed activation in [0, 1]. Drives every modulator contribution.
  // Asymmetric tau: snappy on the way up (noticing), softer on the way
  // down (relaxing).
  float amount;
  float target_amount;

  // Commanded direction (in pixels of pupil offset). Set by the trigger;
  // the phase machine routes it into gaze_tx/gaze_ty at the right moments.
  float dir_x, dir_y;

  // Smoothed pupil-offset contribution (in pixels), shared by both eyes.
  // composePose() adds these to pose.pupil_dx/dy. The per-eye idle_gaze
  // extras keep running on top, so even during the listening hold the
  // pupils have tiny independent asymmetry — reads as "alive."
  float gaze_x,  gaze_y;
  float gaze_tx, gaze_ty;
};

void attentiveInit(AttentiveState &s, uint32_t now_ms);

// External activation. Re-callable mid-sequence — the smoother just
// retargets, so a sequence of close-together sound events reads as one
// extended attentive moment rather than separate twitches.
//
//   intensity   ∈ [0, 1]   facial-expression strength (eye widen, pupil
//                          shrink, lid retract, blink suppression).
//                          Direction is NOT scaled by intensity — even a
//                          low-intensity activation still snaps the pupils
//                          toward the source. Pass 0 for "look there
//                          calmly," ~0.7 for an obvious reaction.
//   duration_ms            total active time before Relax kicks in. The
//                          internal freeze/glance/hold/verify timings are
//                          drawn from random ranges that fit comfortably
//                          inside this window; for durations < ~600 ms the
//                          verify glance is skipped automatically.
//   direction_x ∈ [-1, 1]  -1 = look left, +1 = look right.
//   direction_y ∈ [-1, 1]  -1 = look up,   +1 = look down.
//                          (Same sign convention as screen coordinates.)
void attentiveTrigger(AttentiveState &s,
                      float intensity, uint32_t duration_ms,
                      float direction_x, float direction_y,
                      uint32_t now_ms);

// Per-frame update. Runs the phase machine + smoothers; called from the
// emotion pass of eyeStateUpdate (same slot as sleepyUpdate / curiosityUpdate).
void attentiveUpdate(AttentiveState &s, uint32_t now_ms, float dt);

// Pure function — writes contributions into the Modulator bus. Always safe
// to call; returns immediately when amount == 0 so the awake stack is a
// no-op when nothing has been triggered.
void attentiveModulate(const AttentiveState &s, Modulators &mods);
