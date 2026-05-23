// Procedural eye animation for a binocular pair on a 1bpp SSD1306.
//
// Design contract:
//   * Behaviors are independent layers. Each updateXxx() reads time and writes
//     into EyeState. They never read each other. Adding sleepy/curiosity/etc.
//     later means adding another updateXxx() that pokes the same fields.
//   * Rendering is a pure function of EyeState + the static Eye descriptors.
//   * Pupils are dynamic offsets, eyelids are procedural rectangle masks.
//     No pre-baked frames anywhere.
//
// Frame composition:
//     1. clear
//     2. draw sclera bitmap (WHITE)
//     3. draw pupil bitmap with (drift_x, drift_y) offset (BLACK = hole)
//     4. draw the upper eyelid as a procedural curved arc per column (BLACK)
//        descending from above. Curvature follows spherical perspective:
//          centerline above eye center -> arc bows UP (∩)
//          centerline at eye center    -> flat line
//          centerline below eye center -> arc bows DOWN (∪)
//     5. draw the lower eyelid the same way but rising from below, with
//        smaller travel (real eyes blink mostly with the upper lid).
//     6. push to display
//
// Pixels outside the eye silhouette are no-ops because painting BLACK on an
// already-off pixel doesn't change anything — the lid shape gets clipped to
// the eye automatically.
//
// Eyes are treated as a binocular pair (shared drift, shared blink) because
// that's what reads as "alive" — independent eyes look broken.

#pragma once

#include <Arduino.h>
#include <Adafruit_SSD1306.h>

// Static, build-time eye geometry. One Eye per physical eye.
struct EyeBitmap {
  const uint8_t *bmp;
  int16_t        w;
  int16_t        h;
  int16_t        x;   // top-left in screen coords (sclera position)
  int16_t        y;
};

struct PupilBitmap {
  const uint8_t *bmp;
  int16_t        w;
  int16_t        h;
  int16_t        x;   // top-left at neutral gaze
  int16_t        y;
};

struct Eye {
  EyeBitmap   sclera;
  PupilBitmap pupil;
  // The eyelid layer is procedural (computed from sclera geometry + a global
  // curvature constant), so it doesn't carry its own bitmap. Add a per-eye
  // `float lid_curve` here later if you want asymmetric expressions.
};

// Live, per-frame state. Behaviors mutate this; render reads it.
struct EyeState {
  // ---- micro motion (shared across both eyes) ----
  float    drift_x;          // current pupil offset, pixels (smoothed)
  float    drift_y;
  float    drift_tx;         // current target the drift is easing toward
  float    drift_ty;
  uint32_t drift_next_ms;    // when to pick a new target

  // ---- blink ----
  float    lid_close;        // 0 = open, 1 = fully closed (smooth, procedural)
  uint32_t blink_next_ms;    // scheduled start of next blink (when idle)
  uint32_t blink_start_ms;   // 0 if no blink in flight
  uint16_t blink_close_ms;   // closing phase length (set per-blink)
  uint16_t blink_hold_ms;    // fully-closed hold
  uint16_t blink_open_ms;    // opening phase length
  bool     double_pending;   // queued second blink right after this one

  // ---- sleepy mode ----
  // A single 0..1 scalar that other layers READ as a multiplier — they never
  // branch on it. At sleepy_amount = 0 the animation is identical to fully
  // awake. Effects ramp in continuously as it grows: lids droop, pupil sinks,
  // drift slows, blinks lengthen, occasional long sleepy blinks appear.
  float    sleepy_amount;    // current, smoothed (0..1)
  float    sleepy_target;    // currently easing toward this (0..1)
  uint32_t sleepy_next_ms;   // when to autonomously re-roll sleepy_target

  // ---- bookkeeping ----
  uint32_t last_update_ms;
};

// Seed RNG, zero state, schedule the first blink.
void eyeStateInit(EyeState &s, uint32_t now_ms);

// Per-frame entrypoint. Computes dt internally and runs the behavior layers.
void eyeStateUpdate(EyeState &s, uint32_t now_ms);

// Individual behavior layers — public so you can compose your own update()
// (e.g. skip blink while showing an emotion, or layer a "look at" override
// before micro motion). They read each other's outputs through EyeState only;
// order matters: sleepy first (it parameters drift + blink), then drift, then
// blink.
void updateSleepyState(EyeState &s, uint32_t now_ms, float dt);
void updateMicroMotion(EyeState &s, uint32_t now_ms, float dt);
void updateBlink(EyeState &s, uint32_t now_ms);

// External hook: nudge sleepy_target (e.g. from a light sensor or time of
// day). The autonomous re-roll inside updateSleepyState() will eventually
// overwrite this — call it on each tick if you want it to "stick", or call it
// once and let the autonomous behavior take over.
void eyeStateSetSleepy(EyeState &s, float target);

// Renders both eyes. Does clearDisplay() and display() itself so callers don't
// have to know about the layer order.
void renderEyes(Adafruit_SSD1306 &d,
                const Eye &left, const Eye &right,
                const EyeState &s);

// Lower-level: render one eye's layers into the current framebuffer without
// clearing or pushing. Useful when composing more elements on the screen.
void renderEye(Adafruit_SSD1306 &d, const Eye &eye, const EyeState &s);
