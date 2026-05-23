// Top-level public API for the procedural eye animation system.
//
// This header is intentionally small. It is the ONLY header `main.cpp` (or
// any future app code) needs to include for routine use:
//
//   #include "anim/eye_anim.h"
//
//   EyeState eyes;
//   eyeStateInit(eyes, millis());
//
//   // every frame:
//   eyeStateUpdate(eyes, millis());
//   renderEyes(display, kLeftEye, kRightEye, eyes);
//
// Architecture in one paragraph:
//   * `EyeState` is a *bag of behavior states* (micro motion, blink, sleepy,
//     gaze) plus a single composed `EyePose`. Each behavior owns its own
//     fields in its own header — this struct just hosts them so the app
//     side has a single value to pass around.
//   * `eyeStateUpdate()` runs a fixed pipeline every frame:
//       1. Emotions update their internal scalars.
//       2. Emotions write into a fresh `Modulators`.
//       3. Motion behaviors (drift, blink) update, reading Modulators + Gaze.
//       4. composePose() reads behavior states + Modulators -> EyePose.
//   * Renderer is a pure function of (Eye geometry, EyePose). It cannot
//     observe sleepiness, blink phase, or any other internal state — only
//     the composed pose.
//
// Adding a new emotion (surprise, happy, sad, ...): drop a new file under
// `behaviors/`, give it `xxxState` + `xxxUpdate` + `xxxModulate`, add a
// member to `EyeState`, and call them in eye_anim.cpp's update pipeline.
// Existing behaviors do not change.
//
// Adding a new motion behavior (look-at, micro-saccades, ...): drop another
// behavior file, have it read `Modulators` + `GazeIntent` + its own state,
// and either write directly into the pose during composePose() or into a
// new BehaviorState that composePose pulls from.

#pragma once

#include <Arduino.h>
#include <Adafruit_SSD1306.h>

#include "eye_pose.h"
#include "eye_render.h"
#include "behaviors/micro_motion.h"
#include "behaviors/blink.h"
#include "behaviors/sleepy.h"
#include "behaviors/curiosity.h"
#include "behaviors/idle_gaze.h"

// Container of every behavior's private state plus the composed pose.
// Behaviors only ever touch THEIR sub-struct; the orchestrator (eye_anim.cpp)
// is the only thing that touches the whole EyeState.
struct EyeState {
  MicroMotionState micro;
  BlinkState       blink;
  SleepyState      sleepy;
  CuriosityState   curiosity;
  IdleGazeState    idle_gaze;

  // External gaze input (face detector, IMU, scripted scenes). Set via the
  // eyeSetGazeTarget() / eyeClearGazeTarget() helpers. Defaults to inactive
  // so existing setups behave identically to before.
  GazeIntent       gaze;

  // Composed output from the last eyeStateUpdate(). The renderer reads this
  // (and only this) when drawing.
  EyePose          pose;

  uint32_t         last_update_ms;
};

// Seed RNG, zero state, schedule the first blink + sleepy mood roll.
void eyeStateInit(EyeState &s, uint32_t now_ms);

// Per-frame entrypoint. Computes dt, runs the behavior pipeline, composes
// the final pose. Renderer reads `s.pose` afterwards.
void eyeStateUpdate(EyeState &s, uint32_t now_ms);

// Renders both eyes from `s.pose`. Convenience that hides EyePose from
// callers that just want to draw and forget. Internally calls
// renderEyes(d, l, r, s.pose).
void renderEyes(Adafruit_SSD1306 &d,
                const Eye &left, const Eye &right,
                const EyeState &s);

// Render a single eye on its own display. Convenience overload that pulls
// the pose out of EyeState. Use when each eye lives on its own physical
// OLED:
//   renderEyeOn(displayL, kLeftEye,  eyes);
//   renderEyeOn(displayR, kRightEye, eyes);
void renderEyeOn(Adafruit_SSD1306 &d, const Eye &eye, const EyeState &s);

// External hook: nudge the sleepy target (e.g. from a light sensor or time
// of day). The autonomous re-roll inside sleepyUpdate() will eventually
// overwrite this — call once for "stay sleepy a while", or every frame to
// pin it to a sensor value.
void eyeStateSetSleepy(EyeState &s, float target);

// External event hook: fire the wide-eye curiosity expression. Designed
// for sensor / scripted-scene activation:
//   * Finger snap detected      -> eyeTriggerCuriosity(eyes, 0.9f, 1200);
//   * Face just appeared        -> eyeTriggerCuriosity(eyes, 0.7f, 2000);
//   * Light touch on a sensor   -> eyeTriggerCuriosity(eyes, 0.6f, 1500);
//
// `intensity` ∈ [0, 1] scales eye-widening, pupil-shrink, gaze focus, and
// motion energy. `duration_ms` is how long to hold before the soft decay
// starts. Re-callable any time — the smoother just retargets, no hard
// switching, fully interruptible / restartable. Calling with intensity = 0
// just lets the current activation decay early.
void eyeTriggerCuriosity(EyeState &s, float intensity, uint32_t duration_ms);

// External gaze input. `target_x`/`target_y` are pupil-offset pixels
// (same units as drift). `weight` blends with random drift:
//   0.0 = ignored (pure micro motion), 1.0 = drift fully tracks gaze.
// Call from a face-tracking loop every frame; smoothing inside micro_motion
// turns even a noisy detection signal into organic eye follow.
void eyeSetGazeTarget(EyeState &s,
                      float target_x, float target_y, float weight = 1.0f);

// Stop applying any gaze input. Equivalent to setting weight = 0 but also
// flips the active flag, so other behaviors (debug/UX) can know the system
// is back in pure-autonomous mode.
void eyeClearGazeTarget(EyeState &s);
