// Curiosity: a "wide-eye, focused attention" emotion.
//
// When active, the character looks engaged — sclera scales up slightly, lids
// retract toward fully open, the pupil shrinks, and gaze focus tightens
// (smaller drift range, longer holds, slightly snappier easing). Reads like
// a tiny creature noticing something interesting; not anime shock, not
// robotic alert.
//
// Triggers two ways:
//
//   * Autonomous: every CURIOSITY_AUTO_HOLD_MIN..MAX seconds, a small
//     chance to fire a soft activation. Keeps the toy feeling alive even
//     when nothing external is happening.
//
//   * External:   curiosityTrigger(state, intensity, duration_ms, now_ms),
//                 wrapped at the public API as eyeTriggerCuriosity() — for
//                 sensor-driven activation (sound, touch, face appearance,
//                 finger snap, motion). Re-callable any time; the smoother
//                 eases to whatever the latest target is.
//
// Follows the same shape as sleepy:
//   * Owns one scalar (`amount` in [0, 1]) plus a target + hold timer.
//   * update() evolves amount with asymmetric tau (fast in, slower out)
//     so noticing feels snappier than relaxing.
//   * modulate() writes contributions to Modulators; never touches other
//     behaviors directly. Adding/removing curiosity changes nothing outside
//     this file + a few wiring lines in eye_anim.

#pragma once

#include <Arduino.h>

#include "../eye_pose.h"

struct CuriosityState {
  // Smoothed curiosity amount [0, 1]. 0 = baseline; 1 = peak attention.
  // The renderer never reads this directly — everything flows through
  // Modulators (consistent with how sleepy is wired).
  float    amount;

  // What `amount` is easing toward. Set by autonomous re-roll or by an
  // external trigger.
  float    target;

  // When to drop target back to 0 (end of the active hold). Until then,
  // target stays at the activation intensity.
  uint32_t hold_until_ms;

  // When to roll the next autonomous trigger check.
  uint32_t next_autoroll_ms;
};

void curiosityInit(CuriosityState &s, uint32_t now_ms);

// External activation. `intensity` ∈ [0, 1] scales the peak amount;
// `duration_ms` is how long to hold before fading. Re-callable any time:
// the smoother just retargets, no hard switching, interruptible/restartable.
void curiosityTrigger(CuriosityState &s,
                      float intensity, uint32_t duration_ms,
                      uint32_t now_ms);

// Per-frame update: drops target to 0 when the hold expires, rolls the
// autonomous trigger schedule, and eases `amount` toward target with
// fast-attack / slow-decay taus.
void curiosityUpdate(CuriosityState &s, uint32_t now_ms, float dt);

// Pure function — writes contributions into the Modulator bus. Called
// every frame after curiosityUpdate, before motion behaviors read mods.
void curiosityModulate(const CuriosityState &s, Modulators &mods);
