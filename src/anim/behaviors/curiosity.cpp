#include "curiosity.h"

#include <math.h>

#include "../anim_util.h"

namespace {

// ---- Amount evolution ----
// Asymmetric tau: snappy on the way up ("ooh, what's that?"), softer on
// the way down so the character relaxes instead of snapping back.
// 0.10 s tau in = ~95% of target reached in ~0.3 s → leaves room for the
// hold to be a clear ~1 s plateau before decay starts.
constexpr float TAU_IN_S  = 0.10f;
constexpr float TAU_OUT_S = 0.50f;

// ---- Autonomous trigger schedule ----
// How often the behavior considers triggering itself. 8–25 s gives a
// natural rhythm — long enough that the toy isn't constantly "curious,"
// short enough that you don't wait minutes between activations.
constexpr uint16_t AUTO_HOLD_MIN_MS  = 8000;
constexpr uint16_t AUTO_HOLD_MAX_MS  = 25000;

// Probability per re-roll that the toy spontaneously looks curious.
constexpr float    AUTO_TRIGGER_CHANCE = 0.30f;

// Random intensity / duration window for self-triggered curiosity.
// External triggers can be stronger / weaker / longer / shorter — these
// just shape the autonomous mood. Duration is "ramp + hold"; with TAU_IN
// = 0.10 s the ramp eats ~0.3 s so 1300–1700 ms total = ~1.0–1.4 s of
// visible peak hold.
constexpr float    AUTO_INTENSITY_MIN  = 0.55f;
constexpr float    AUTO_INTENSITY_MAX  = 0.95f;
constexpr uint16_t AUTO_DURATION_MIN_MS = 1300;
constexpr uint16_t AUTO_DURATION_MAX_MS = 1700;

// ---- Modulator contributions at amount = 1 ----
// All curiosity effects scale linearly with `amount`, so behavior is
// continuous — at amount = 0 every contribution below is zero and the
// animation matches the awake baseline exactly.

// Sclera growth via eye_open_add → pose.eye_open_amount. Renderer stretches
// the sclera bitmap VERTICALLY by this multiplier (width is untouched, so
// the eye gets tall, not bigger overall — matches the Ghibli reference).
// 0.90 means sclera height ≈ 1.9× baseline at peak, which is "almost twice"
// the visible white area; height overflows the OLED at PHYS_SCALE = 2.0,
// which is intentional (the fur cutout absorbs the overflow).
constexpr float EYE_OPEN_BOOST = 0.50f;

// Pupil shrink via pupil_scale_mult → pose.pupil_scale. Mild on purpose —
// the dominant visual cue is the tall sclera; the pupil staying close to
// baseline reads as "alert" rather than "scared" (which would be a much
// smaller pupil).
constexpr float PUPIL_SHRINK = 0.5f;

// Lid retraction. NEGATIVE additive into lid_upper_droop / lid_lower_rise
// drives the lids toward fully open; composePose clamps the sum to [0, 1],
// so this first neutralizes any sleepy droop, then can't go past
// fully-open. (Side effect at full curiosity + full blink: lid_close = 1
// minus retract clamps the visible blink to 1 - retract. That reads as
// "alert creature blinks less deeply" which actually matches the look.)
constexpr float LID_UPPER_RETRACT = 0.30f;
constexpr float LID_LOWER_RETRACT = 0.15f;

// Gaze focus modulators. Drift behaviors (micro_motion + idle_gaze) read
// these from the bus, so curiosity tightens both layers without either
// of them knowing curiosity exists.
constexpr float DRIFT_RANGE_REDUCTION = 0.30f;  // -30% wander at peak
constexpr float DRIFT_HOLD_BOOST      = 1.50f;  // +150% hold duration at peak
constexpr float DRIFT_TAU_REDUCTION   = 0.15f;  // -15% tau at peak (snappier)

}  // namespace

void curiosityInit(CuriosityState &s, uint32_t now_ms) {
  s.amount        = 0.0f;
  s.target        = 0.0f;
  s.hold_until_ms = now_ms;
  // First autoroll is slightly randomized so two boots don't fire at the
  // same wall-clock moment.
  s.next_autoroll_ms = now_ms + anim_util::urand(AUTO_HOLD_MIN_MS,
                                                  AUTO_HOLD_MAX_MS);
}

void curiosityTrigger(CuriosityState &s,
                      float intensity, uint32_t duration_ms,
                      uint32_t now_ms) {
  s.target        = anim_util::clamp01(intensity);
  s.hold_until_ms = now_ms + duration_ms;
}

void curiosityUpdate(CuriosityState &s, uint32_t now_ms, float dt) {
  using namespace anim_util;

  // Drop target when the active hold expires — autonomous decay back to 0.
  if ((int32_t)(now_ms - s.hold_until_ms) >= 0) {
    s.target = 0.0f;
  }

  // Autonomous re-roll. We do this AFTER the hold-expiry check so an
  // autoroll that fires while another curiosity is winding down can extend
  // it cleanly.
  if ((int32_t)(now_ms - s.next_autoroll_ms) >= 0) {
    if (frand(0.0f, 1.0f) < AUTO_TRIGGER_CHANCE) {
      s.target        = frand(AUTO_INTENSITY_MIN, AUTO_INTENSITY_MAX);
      s.hold_until_ms = now_ms + urand(AUTO_DURATION_MIN_MS,
                                        AUTO_DURATION_MAX_MS);
    }
    s.next_autoroll_ms = now_ms + urand(AUTO_HOLD_MIN_MS, AUTO_HOLD_MAX_MS);
  }

  // Asymmetric easing — fast on the way up, slow on the way down.
  const float tau = (s.target > s.amount) ? TAU_IN_S : TAU_OUT_S;
  const float k   = 1.0f - expf(-dt / tau);
  s.amount += (s.target - s.amount) * k;
  s.amount = clamp01(s.amount);
}

void curiosityModulate(const CuriosityState &s, Modulators &mods) {
  const float a = s.amount;
  if (a <= 0.0f) return;  // fast path — no contributions when neutral

  // Sclera / pupil shape contributions.
  mods.eye_open_add     += a * EYE_OPEN_BOOST;
  mods.pupil_scale_mult *= 1.0f - a * PUPIL_SHRINK;

  // Lid retraction — additive, negative pushes lids open. Stacks with
  // sleepy droop, so a sleepy creature noticing something still opens up
  // partially.
  mods.lid_upper_droop  -= a * LID_UPPER_RETRACT;
  mods.lid_lower_rise   -= a * LID_LOWER_RETRACT;

  // Gaze focus. Rate contributions stack MULTIPLICATIVELY with sleepy /
  // future emotions; a sleepy * curious blend reads as "drowsy but
  // suddenly attentive on something" — very small range, very long hold.
  mods.drift_range_mult *= 1.0f - a * DRIFT_RANGE_REDUCTION;
  mods.drift_hold_mult  *= 1.0f + a * DRIFT_HOLD_BOOST;
  mods.drift_tau_mult   *= 1.0f - a * DRIFT_TAU_REDUCTION;
}
