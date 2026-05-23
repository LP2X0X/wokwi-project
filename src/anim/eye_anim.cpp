#include "eye_anim.h"

#include <esp_random.h>

#include "anim_util.h"

namespace {

// composePose is the sole bridge between behavior states and the final
// pose. Reads:
//   - micro motion state (pupil offsets)
//   - blink state        (lid_close)
//   - the modulator stack (emotion contributions)
// Writes:
//   - everything in EyePose
//
// Adding a new behavior whose output feeds into the pose means giving it a
// state struct, exposing a getter or fields for what composePose needs, and
// folding it into the math here. Behaviors never write to the pose
// themselves — keeping all assembly here means there's exactly one place to
// look when "why is the eye half-closed?" comes up.
void composePose(const EyeState &s, const Modulators &mods, EyePose &out) {
  using namespace anim_util;

  // Pupil position: drift carries the live offset, emotion adds y bias.
  // Pupil scale is the modulator stack's multiplier (1.0 today; future
  // surprise/dilation lands here without touching anything else).
  out.pupil_dx    = s.micro.drift_x;
  out.pupil_dy    = s.micro.drift_y + mods.pupil_y_bias_px;
  out.pupil_scale = mods.pupil_scale_mult;

  // Lid amounts in dimensionless [0..1]. Renderer scales by per-eye height
  // so the same pose draws on any-sized eye / asymmetric eyes / future
  // per-eye poses (wink etc.) cleanly.
  //
  //   upper = blink_lid_close                 + emotion_droop
  //   lower = blink_lid_close * LOWER_LID_FRAC + emotion_lower_rise
  //
  // The `LOWER_LID_TRAVEL_FRACTION` lives in blink because "the upper lid
  // does most of the closing work" is a property of blinks specifically;
  // emotion contributions to upper/lower are independent.
  float upper = s.blink.lid_close + mods.lid_upper_droop;
  float lower = s.blink.lid_close * blink::LOWER_LID_TRAVEL_FRACTION +
                mods.lid_lower_rise;
  out.upper_lid_amount = clamp01(upper);
  out.lower_lid_amount = clamp01(lower);

  // Master open multiplier (forward-compat hook; defaults to 1.0).
  out.eye_open_amount = 1.0f + mods.eye_open_add;
}

}  // namespace

// ---------- public API ----------

void eyeStateInit(EyeState &s, uint32_t now_ms) {
  randomSeed(esp_random());

  microMotionInit(s.micro,  now_ms);
  blinkInit      (s.blink,  now_ms);
  sleepyInit     (s.sleepy, now_ms);

  s.gaze = GazeIntent{false, 0.0f, 0.0f, 0.0f};

  s.last_update_ms = now_ms;

  // Seed the pose with a neutral composition so callers that render before
  // their first eyeStateUpdate() (rare, but possible) don't draw garbage.
  composePose(s, Modulators::neutral(), s.pose);
}

void eyeStateUpdate(EyeState &s, uint32_t now_ms) {
  uint32_t prev = s.last_update_ms ? s.last_update_ms : now_ms;
  float dt = (now_ms - prev) / 1000.0f;
  if (dt < 0.0f) dt = 0.0f;
  if (dt > 0.1f) dt = 0.1f;     // clamp big dt (paused tab, lost frames)
  s.last_update_ms = now_ms;

  // Two-pass behavior loop:
  //
  //   Pass 1: emotions update their internal scalars (sleepy, surprise, ...)
  //   Pass 2: emotions write into a fresh Modulators bus.
  //   Pass 3: motion behaviors (drift, blink) update, reading the bus.
  //   Pass 4: composePose() reads behavior states + bus -> final EyePose.
  //
  // No behavior reads another behavior's state directly. The bus is the
  // only coupling, which keeps the system a DAG — no cycles, no surprises.

  // Pass 1: emotion updates.
  sleepyUpdate(s.sleepy, now_ms, dt);
  // Add new emotions here:
  //   surpriseUpdate(s.surprise, now_ms, dt);
  //   happyUpdate   (s.happy,    now_ms, dt);

  // Pass 2: build the modulator stack.
  Modulators mods = Modulators::neutral();
  sleepyModulate(s.sleepy, mods);
  // Add new emotions here:
  //   surpriseModulate(s.surprise, mods);
  //   happyModulate   (s.happy,    mods);

  // Pass 3: motion behaviors consume modulators + gaze.
  microMotionUpdate(s.micro, mods, s.gaze, now_ms, dt);
  blinkUpdate      (s.blink, mods, now_ms);

  // Pass 4: build the pose for this frame.
  composePose(s, mods, s.pose);
}

void renderEyes(Adafruit_SSD1306 &d,
                const Eye &left, const Eye &right,
                const EyeState &s) {
  ::renderEyes(d, left, right, s.pose);
}

void eyeStateSetSleepy(EyeState &s, float target) {
  sleepySet(s.sleepy, target);
}

void eyeSetGazeTarget(EyeState &s,
                      float target_x, float target_y, float weight) {
  s.gaze.active   = true;
  s.gaze.target_x = target_x;
  s.gaze.target_y = target_y;
  s.gaze.weight   = anim_util::clamp01(weight);
}

void eyeClearGazeTarget(EyeState &s) {
  s.gaze.active = false;
  s.gaze.weight = 0.0f;
}
