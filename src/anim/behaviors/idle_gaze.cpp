#include "idle_gaze.h"

#include <math.h>

#include "../anim_util.h"

namespace {

// ---- Tunables (private) ----
// Glance buckets, in pixels. Tiny next to micro_motion's 2 px wobble — but
// the long holds between glances are what make even 1-2 px feel intentional.
// Glance amplitudes in source pixels; the renderer scales them by
// Eye::scale, so on the 240-px physical panels (PHYS_SCALE = 4.0) a
// LARGE glance is ±40 px — a clear, deliberate look. Subtle SMALL and
// MEDIUM buckets keep the dominant feel of "mostly resting, occasional
// real glance."
constexpr float SMALL_RANGE_PX   = 3.0f;
constexpr float MEDIUM_RANGE_PX  = 6.0f;
constexpr float LARGE_RANGE_PX   = 10.0f;

// Vertical weight per bucket. Real eye movement is more horizontal than
// vertical; large bucket allows occasional curious upward/downward looks.
constexpr float VERT_WEIGHT_SMALL  = 0.4f;
constexpr float VERT_WEIGHT_MEDIUM = 0.5f;
constexpr float VERT_WEIGHT_LARGE  = 0.7f;

// Bucket probabilities (cumulative). ~60% small / ~30% medium / ~10% large.
constexpr float BUCKET_CUM_SMALL  = 0.60f;
constexpr float BUCKET_CUM_MEDIUM = 0.90f;

// Hold + move durations BEFORE sleepy modulators. Holds are deliberately long
// so the character spends most of its time still.
constexpr uint16_t HOLD_MIN_MS = 1500;
constexpr uint16_t HOLD_MAX_MS = 4500;
constexpr uint16_t MOVE_MIN_MS = 280;
constexpr uint16_t MOVE_MAX_MS = 1100;

// First-move stagger so two boots don't look identical. Short enough that
// the toy still wakes up "alive" quickly.
constexpr uint16_t INITIAL_HOLD_MIN_MS = 800;
constexpr uint16_t INITIAL_HOLD_MAX_MS = 2200;

// Per-move easing tau range. Soft default with occasional quicker flicks.
constexpr float TAU_MIN_S = 0.22f;
constexpr float TAU_MAX_S = 0.55f;

// Asymmetry knobs — very subtle on purpose. ±0.45 px target jitter and ±18%
// tau jitter are below the threshold of "weird," well above "perfectly
// synced robot."
constexpr float ASYMMETRY_PX        = 0.0f;
constexpr float ASYMMETRY_TAU_FRAC  = 0.0f;

// External-gaze threshold. If GazeIntent's weight is above this, treat idle
// behavior as overridden and ease toward 0. Below the threshold we let idle
// gaze continue so very-weak external signals don't suppress liveliness.
constexpr float GAZE_OVERRIDE_WEIGHT = 0.5f;

// Pick a glance offset from the weighted bucket distribution. Pure function;
// no state access — easy to swap out (e.g. for a future "curiosity" emotion
// that biases targets toward a face direction).
void rollGlanceTarget(float &out_x, float &out_y) {
  using namespace anim_util;
  const float bucket = frand(0.0f, 1.0f);

  float range_px;
  float vert_weight;
  if (bucket < BUCKET_CUM_SMALL) {
    range_px    = SMALL_RANGE_PX;
    vert_weight = VERT_WEIGHT_SMALL;
  } else if (bucket < BUCKET_CUM_MEDIUM) {
    range_px    = MEDIUM_RANGE_PX;
    vert_weight = VERT_WEIGHT_MEDIUM;
  } else {
    range_px    = LARGE_RANGE_PX;
    vert_weight = VERT_WEIGHT_LARGE;
  }

  out_x = frand(-range_px, range_px);
  out_y = frand(-range_px, range_px) * vert_weight;
}

}  // namespace

void idleGazeInit(IdleGazeState &s, uint32_t now_ms) {
  s.phase          = IdleGazeState::Holding;
  s.phase_until_ms = now_ms + anim_util::urand(INITIAL_HOLD_MIN_MS,
                                                INITIAL_HOLD_MAX_MS);

  s.gx_l = s.gy_l = 0.0f;
  s.gx_r = s.gy_r = 0.0f;
  s.tx_l = s.ty_l = 0.0f;
  s.tx_r = s.ty_r = 0.0f;
  s.tau_l = s.tau_r = 0.35f;
}

void idleGazeUpdate(IdleGazeState    &s,
                    const Modulators &mods,
                    const GazeIntent &gaze,
                    uint32_t          now_ms,
                    float             dt) {
  using namespace anim_util;

  // External gaze override: fade idle behavior to 0 so the face tracker (or
  // whatever wrote GazeIntent) becomes the dominant source. We freeze the
  // phase machine while overridden — when the override releases, the next
  // tick picks up where it left off.
  const bool overridden = gaze.active && gaze.weight > GAZE_OVERRIDE_WEIGHT;

  // ---- Phase machine: only re-evaluate when an event is due. ----
  if (!overridden && (int32_t)(now_ms - s.phase_until_ms) >= 0) {
    if (s.phase == IdleGazeState::Holding) {
      // Hold ended -> pick a new glance target.
      float bx, by;
      rollGlanceTarget(bx, by);

      // Apply sleepy/range scaling globally so the character looks around
      // less far when drowsy.
      bx *= mods.drift_range_mult;
      by *= mods.drift_range_mult;

      // Per-eye target jitter (asymmetry pass 1).
      s.tx_l = bx + frand(-ASYMMETRY_PX, ASYMMETRY_PX);
      s.ty_l = by + frand(-ASYMMETRY_PX, ASYMMETRY_PX);
      s.tx_r = bx + frand(-ASYMMETRY_PX, ASYMMETRY_PX);
      s.ty_r = by + frand(-ASYMMETRY_PX, ASYMMETRY_PX);

      // Per-move tau, sleepy-scaled, then jittered per eye (asymmetry pass 2).
      const float tau_base = frand(TAU_MIN_S, TAU_MAX_S) * mods.drift_tau_mult;
      s.tau_l = tau_base * (1.0f + frand(-ASYMMETRY_TAU_FRAC, ASYMMETRY_TAU_FRAC));
      s.tau_r = tau_base * (1.0f + frand(-ASYMMETRY_TAU_FRAC, ASYMMETRY_TAU_FRAC));

      // Move window. Sleepy stretches it so the smoother has time to settle
      // even with a larger tau.
      const uint32_t move_base = urand(MOVE_MIN_MS, MOVE_MAX_MS);
      s.phase_until_ms = now_ms +
          (uint32_t)((float)move_base * mods.drift_tau_mult);
      s.phase = IdleGazeState::Moving;
    } else {
      // Move ended -> long, quiet hold. Targets stay; the smoother converges
      // to the chosen point and the character "rests" there.
      const uint32_t hold_base = urand(HOLD_MIN_MS, HOLD_MAX_MS);
      s.phase_until_ms = now_ms +
          (uint32_t)((float)hold_base * mods.drift_hold_mult);
      s.phase = IdleGazeState::Holding;
    }
  }

  // ---- Easing every frame regardless of phase. ----
  // When overridden, ease toward 0 to release control to the external source.
  const float target_x_l = overridden ? 0.0f : s.tx_l;
  const float target_y_l = overridden ? 0.0f : s.ty_l;
  const float target_x_r = overridden ? 0.0f : s.tx_r;
  const float target_y_r = overridden ? 0.0f : s.ty_r;

  // Two independent critically-damped low-passes. Per-eye taus mean the
  // pupils arrive at the target a beat apart — subtle, never broken-looking.
  const float kl = 1.0f - expf(-dt / s.tau_l);
  const float kr = 1.0f - expf(-dt / s.tau_r);
  s.gx_l += (target_x_l - s.gx_l) * kl;
  s.gy_l += (target_y_l - s.gy_l) * kl;
  s.gx_r += (target_x_r - s.gx_r) * kr;
  s.gy_r += (target_y_r - s.gy_r) * kr;
}
