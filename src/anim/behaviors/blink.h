// Blink: aperiodic three-phase eyelid close+hold+open with rare double blinks.
//
// Owns one number: `lid_close ∈ [0, 1]` — the fraction by which the upper
// lid centerline has descended from the top of the eye. composePose() mixes
// it with emotion droop contributions to produce final pose lid amounts.
//
// Reads Modulators for:
//   * blink_duration_mult     - slow / speed up the close+hold+open phases
//   * long_blink_chance + range - emotions can occasionally extend the hold
//                                 (sleepy uses this for "drowsy long blinks")
//
// Owns the LOWER_LID_TRAVEL_FRACTION constant because "the upper lid does
// most of the closing motion" is a property of blinks, not of any emotion.
// composePose() reads this constant to translate `lid_close` into a separate
// lower-lid contribution.

#pragma once

#include <Arduino.h>

#include "../eye_pose.h"

// How far the lower lid rises during a blink as a fraction of how far the
// upper lid descends. Real eye blinks are ~80% upper-lid driven — this
// matches that bias and keeps the closing motion organic.
namespace blink {
constexpr float LOWER_LID_TRAVEL_FRACTION = 0.30f;
}

struct BlinkState {
  // Output: 0 = open, 1 = fully closed. Sampled per frame from the blink
  // phase curve. Always 0 when no blink is in flight.
  float lid_close;

  // Schedule.
  uint32_t blink_next_ms;     // start time of next blink (when idle)
  uint32_t blink_start_ms;    // 0 if no blink in flight, else start time

  // Per-blink phase durations. Sampled once at blink start so individual
  // blinks have stable timing even if Modulators change mid-blink.
  uint16_t blink_close_ms;
  uint16_t blink_hold_ms;
  uint16_t blink_open_ms;

  // Queued second blink (double-blink behavior). Set just before a blink
  // ends; the next schedule check uses a much shorter gap.
  bool double_pending;
};

void blinkInit(BlinkState &s, uint32_t now_ms);

// Drives the blink schedule + samples the in-flight blink curve. Runs every
// frame; reads Modulators so emotions can extend hold ("sleepy blink") or
// slow the entire phase ("tired").
void blinkUpdate(BlinkState &s, const Modulators &mods, uint32_t now_ms);
