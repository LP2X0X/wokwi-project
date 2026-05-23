#include "sleepy.h"

#include <math.h>

#include "../anim_util.h"

namespace {

// ---- Mood evolution ----

// Easing tau for `amount` itself — long, so transitions in/out of sleepiness
// feel like a slow mood change, not a switch.
constexpr float    SLEEPY_TAU_S         = 1.5f;

// Autonomous re-roll cadence: how long the toy stays at one target before
// drifting to a new one. 8–25 s gives a believable "mood" rhythm.
constexpr uint16_t SLEEPY_HOLD_MIN_MS   = 8000;
constexpr uint16_t SLEEPY_HOLD_MAX_MS   = 25000;

// ---- Modulator contributions at amount = 1 ----
//
// All sleepy effects scale linearly with `amount`, so behavior is continuous.
// At amount = 0 every contribution below is zero and the animation matches
// the awake baseline exactly.

// Lid biases at full sleepy. Fractions of sclera height — composePose()
// translates them into normalized lid amounts directly.
constexpr float    SLEEPY_UPPER_DROOP_FRACTION   = 0.4f;
constexpr float    SLEEPY_LOWER_RISE_FRACTION    = 0.2f;

// Static downward bias on the pupil at full sleepy, pixels.
constexpr float    SLEEPY_PUPIL_BIAS_PX          = 1.5f;

// Drift modulation strengths (max contribution at amount = 1).
constexpr float    SLEEPY_DRIFT_TAU_MULT         = 1.5f;
constexpr float    SLEEPY_DRIFT_RANGE_REDUCTION  = 0.5f;
constexpr float    SLEEPY_DRIFT_HOLD_MULT        = 1.5f;

// Blink modulation.
constexpr float    SLEEPY_BLINK_DURATION_MULT    = 1.5f;
constexpr uint8_t  SLEEPY_LONG_BLINK_PCT_AT_FULL = 30;
constexpr uint16_t SLEEPY_LONG_BLINK_HOLD_MIN_MS = 300;
constexpr uint16_t SLEEPY_LONG_BLINK_HOLD_MAX_MS = 900;

}  // namespace

void sleepyInit(SleepyState &s, uint32_t now_ms) {
  s.amount  = 0.0f;
  s.target  = 0.0f;
  // Early first re-roll so the autonomous mood starts varying soon after
  // boot rather than holding a flat awake baseline for 25 s.
  s.next_ms = now_ms + anim_util::urand(2000, 6000);
}

void sleepySet(SleepyState &s, float target) {
  s.target = anim_util::clamp01(target);
}

void sleepyUpdate(SleepyState &s, uint32_t now_ms, float dt) {
  using namespace anim_util;

  // Autonomous mood drift: re-roll a new target every 8–25 s using a soft
  // distribution. Most of the time the toy is awake/lightly drowsy; rarely
  // it gets very sleepy — same shape as how a real little creature drifts.
  if ((int32_t)(now_ms - s.next_ms) >= 0) {
    const float bucket = frand(0.0f, 1.0f);
    if (bucket < 0.55f) {
      s.target = frand(0.00f, 0.20f);    // bright, alert
    } else if (bucket < 0.90f) {
      s.target = frand(0.20f, 0.55f);    // mildly drowsy
    } else {
      s.target = frand(0.65f, 1.00f);    // very sleepy
    }
    s.next_ms = now_ms + urand(SLEEPY_HOLD_MIN_MS, SLEEPY_HOLD_MAX_MS);
  }

  // Critically-damped easing toward target with a long tau — gives the soft,
  // organic transition the user asked for.
  const float k = 1.0f - expf(-dt / SLEEPY_TAU_S);
  s.amount += (s.target - s.amount) * k;
  s.amount = clamp01(s.amount);
}

void sleepyModulate(const SleepyState &s, Modulators &mods) {
  const float a = s.amount;
  if (a <= 0.0f) return;  // fast path — no contributions when fully awake

  // Rate modulators stack multiplicatively so multiple emotions slowing
  // things down feel right (sleepy 1.75x * tired 1.2x = 2.1x slower).
  mods.drift_tau_mult       *= 1.0f + a * SLEEPY_DRIFT_TAU_MULT;
  mods.drift_range_mult     *= 1.0f - a * SLEEPY_DRIFT_RANGE_REDUCTION;
  mods.drift_hold_mult      *= 1.0f + a * SLEEPY_DRIFT_HOLD_MULT;
  mods.blink_duration_mult  *= 1.0f + a * SLEEPY_BLINK_DURATION_MULT;

  // Spatial contributions stack additively — sleepy droop + a future wince
  // would each push the lid a little further down without fighting.
  mods.lid_upper_droop      += a * SLEEPY_UPPER_DROOP_FRACTION;
  mods.lid_lower_rise       += a * SLEEPY_LOWER_RISE_FRACTION;
  mods.pupil_y_bias_px      += a * SLEEPY_PUPIL_BIAS_PX;

  // Long-blink contribution. Multiple emotions can raise the chance; the
  // duration range comes from this emotion (last writer wins for now —
  // good enough until two emotions actually want different ranges).
  mods.long_blink_chance        += a * (SLEEPY_LONG_BLINK_PCT_AT_FULL / 100.0f);
  mods.long_blink_extra_min_ms   = SLEEPY_LONG_BLINK_HOLD_MIN_MS;
  mods.long_blink_extra_max_ms   = SLEEPY_LONG_BLINK_HOLD_MAX_MS;
}
