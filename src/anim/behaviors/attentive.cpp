#include "attentive.h"

#include <math.h>

#include "../anim_util.h"

namespace {

// ---- Amount evolution ----
// Snappy on the way up (the "huh?" beat reads instantly), soft on the way
// down (the creature relaxes, doesn't snap back). Slightly slower than
// curiosity's TAU_IN_S = 0.10 — attentive is "noticing a sound nearby,"
// not "wide-eyed alert."
constexpr float TAU_AMOUNT_IN_S  = 0.12f;
constexpr float TAU_AMOUNT_OUT_S = 0.40f;

// ---- Gaze smoothing ----
// Two taus depending on phase. During Glance / Verify we want a definite
// dart toward the new target (animal-quick); during Hold the smoother is
// barely moving anyway, so a slightly looser tau makes any residual
// movement read as "settling" rather than "snapping."
constexpr float TAU_GAZE_SNAP_S = 0.08f;
constexpr float TAU_GAZE_HOLD_S = 0.30f;

// ---- Phase durations (ms) ----
// FREEZE: brief beat where the pupils DON'T move toward dir — the creature
// is processing the stimulus. Too short reads as robotic, too long reads
// as confused; 80-180 ms feels right for "did I hear something?"
constexpr uint16_t FREEZE_MIN_MS = 80;
constexpr uint16_t FREEZE_MAX_MS = 180;

// GLANCE: snappy move to the source. Width of this window controls how
// "decisive" the look feels.
constexpr uint16_t GLANCE_MIN_MS = 180;
constexpr uint16_t GLANCE_MAX_MS = 280;

// VERIFY: tiny secondary glance during the hold — a brief offset before
// returning to the primary direction. Length is dwell time at the offset.
constexpr uint16_t VERIFY_MIN_MS = 220;
constexpr uint16_t VERIFY_MAX_MS = 360;

// RELAX: how long after end_ms before we're fully back to baseline. The
// smoothers reach ~95% by RELAX_MIN_MS so the phase ending is when the
// behavior is essentially neutral again.
constexpr uint16_t RELAX_MIN_MS = 500;
constexpr uint16_t RELAX_MAX_MS = 750;

// When the verify glance gets scheduled INSIDE the hold. Picked relative
// to Glance->Hold transition. We require at least VERIFY_SCHED_HEADROOM_MS
// of hold left after the verify finishes, otherwise we skip verify (short
// activations end cleanly without a half-formed double-glance).
constexpr uint16_t VERIFY_SCHED_MIN_MS      = 300;
constexpr uint16_t VERIFY_SCHED_MAX_MS      = 700;
constexpr uint16_t VERIFY_SCHED_HEADROOM_MS = 250;

// Verify glance shape. The verify target is a fraction of the primary
// direction (partial pull-back toward center) plus a small random jitter.
// MAIN_FRAC < 1 reads as "looking nearby but not quite at the same spot."
constexpr float VERIFY_MAIN_FRAC = 0.55f;
constexpr float VERIFY_JITTER_PX = 1.4f;

// ---- Direction scaling ----
// direction_x/y from the trigger are in [-1, 1] semantic units. We scale
// them to pixel offsets here. Vertical range is smaller than horizontal
// because real eye movement is more horizontal than vertical — a "look
// down" reaction is more subtle than a "look right."
constexpr float DIR_RANGE_PX_X = 5.0f;
constexpr float DIR_RANGE_PX_Y = 3.0f;

// ---- Modulator contributions at amount = 1 ----
// All scale linearly with `amount`, so a partial activation is a partial
// expression — and amount = 0 contributes nothing (composePose path is a
// no-op for awake stacks).

// Eye widening. Lower than curiosity's 0.50 — attentive is "alert and
// holding still," not "wide-eyed at something interesting."
constexpr float EYE_OPEN_BOOST = 0.20f;

// Pupil shrink. Subtle — reads as "focused" rather than "surprised."
constexpr float PUPIL_SHRINK = 0.18f;

// Upper-lid retraction. Cancels mild sleepy droop first, then opens the
// eye a touch further; clamped at composePose so it never pushes lids
// past fully-open.
constexpr float LID_UPPER_RETRACT = 0.15f;

// Drift focus. Bigger reductions than curiosity because the listening
// hold should read as "very still" — only micro motion keeps the eyes
// from looking dead.
constexpr float DRIFT_RANGE_REDUCTION = 0.60f;   // -60% wander
constexpr float DRIFT_TAU_INCREASE    = 0.20f;   // +20% tau (smoother)
constexpr float DRIFT_HOLD_BOOST      = 1.50f;   // +150% hold

// Blink suppression contribution. Stacks into mods.blink_inhibit; the blink
// behavior checks the total and defers any blink that would start while
// this is high. Soft, not hard: when amount falls below ~0.3 normal blinks
// resume.
constexpr float BLINK_INHIBIT_AT_PEAK = 1.0f;

}  // namespace

void attentiveInit(AttentiveState &s, uint32_t now_ms) {
  (void)now_ms;
  s.phase          = AttentiveState::Idle;
  s.phase_until_ms = 0;
  s.end_ms         = 0;
  s.verify_done    = false;
  s.amount         = 0.0f;
  s.target_amount  = 0.0f;
  s.dir_x          = 0.0f;
  s.dir_y          = 0.0f;
  s.gaze_x         = 0.0f;
  s.gaze_y         = 0.0f;
  s.gaze_tx        = 0.0f;
  s.gaze_ty        = 0.0f;
}

void attentiveTrigger(AttentiveState &s,
                      float intensity, uint32_t duration_ms,
                      float direction_x, float direction_y,
                      uint32_t now_ms) {
  using namespace anim_util;

  s.target_amount = clamp01(intensity);
  s.dir_x         = direction_x * DIR_RANGE_PX_X;
  s.dir_y         = direction_y * DIR_RANGE_PX_Y;
  s.end_ms        = now_ms + duration_ms;
  s.verify_done   = false;

  if (s.phase == AttentiveState::Idle || s.phase == AttentiveState::Relax) {
    // Fresh activation — start the entry sequence. Gaze target stays at
    // zero during Freeze on purpose; the pause before any movement is what
    // sells the "huh?" beat.
    s.phase          = AttentiveState::Freeze;
    s.phase_until_ms = now_ms + urand(FREEZE_MIN_MS, FREEZE_MAX_MS);
    s.gaze_tx        = 0.0f;
    s.gaze_ty        = 0.0f;
  } else {
    // Already attentive — retarget without re-running the freeze beat. The
    // smoothers will ease pupils to the new direction so a sequence of
    // close-together sound events reads as one extended attentive moment.
    if (s.phase != AttentiveState::Freeze) {
      s.gaze_tx = s.dir_x;
      s.gaze_ty = s.dir_y;
    }
  }
}

void attentiveUpdate(AttentiveState &s, uint32_t now_ms, float dt) {
  using namespace anim_util;

  if (s.phase != AttentiveState::Idle) {
    // Override: if the active duration is up, drop straight into Relax even
    // mid-phase. Keeps short triggers from getting stuck inside a long
    // verify dwell.
    if (s.phase != AttentiveState::Relax &&
        (int32_t)(now_ms - s.end_ms) >= 0) {
      s.phase          = AttentiveState::Relax;
      s.phase_until_ms = now_ms + urand(RELAX_MIN_MS, RELAX_MAX_MS);
      s.target_amount  = 0.0f;
      s.gaze_tx        = 0.0f;
      s.gaze_ty        = 0.0f;
    }

    // Normal phase advance.
    if ((int32_t)(now_ms - s.phase_until_ms) >= 0) {
      switch (s.phase) {
        case AttentiveState::Freeze: {
          // Freeze ended — dart toward the source.
          s.phase          = AttentiveState::Glance;
          s.gaze_tx        = s.dir_x;
          s.gaze_ty        = s.dir_y;
          s.phase_until_ms = now_ms + urand(GLANCE_MIN_MS, GLANCE_MAX_MS);
          break;
        }
        case AttentiveState::Glance: {
          // Glance ended — settle into listening hold. Try to schedule a
          // single verify glance partway through if there's room.
          s.phase = AttentiveState::Hold;
          const int32_t remaining = (int32_t)(s.end_ms - now_ms);
          const int32_t headroom  = VERIFY_SCHED_MIN_MS + VERIFY_SCHED_HEADROOM_MS;
          if (!s.verify_done && remaining > headroom) {
            const uint32_t cap_raw = (uint32_t)(remaining - VERIFY_SCHED_HEADROOM_MS);
            const uint32_t cap     = cap_raw < VERIFY_SCHED_MAX_MS
                                         ? cap_raw : VERIFY_SCHED_MAX_MS;
            const uint32_t verify_in = urand(VERIFY_SCHED_MIN_MS, cap);
            s.phase_until_ms = now_ms + verify_in;
          } else {
            // No time for verify — just hold until end_ms triggers Relax.
            s.phase_until_ms = s.end_ms;
          }
          break;
        }
        case AttentiveState::Hold: {
          // Scheduled time to do the verify glance.
          if (!s.verify_done) {
            s.verify_done    = true;
            s.phase          = AttentiveState::Verify;
            s.gaze_tx        = s.dir_x * VERIFY_MAIN_FRAC +
                               frand(-VERIFY_JITTER_PX, VERIFY_JITTER_PX);
            s.gaze_ty        = s.dir_y * VERIFY_MAIN_FRAC +
                               frand(-VERIFY_JITTER_PX, VERIFY_JITTER_PX);
            s.phase_until_ms = now_ms + urand(VERIFY_MIN_MS, VERIFY_MAX_MS);
          } else {
            // Already verified — coast until end_ms drops us into Relax.
            s.phase_until_ms = s.end_ms;
          }
          break;
        }
        case AttentiveState::Verify: {
          // Verify ended — return gaze to primary direction.
          s.phase          = AttentiveState::Hold;
          s.gaze_tx        = s.dir_x;
          s.gaze_ty        = s.dir_y;
          s.phase_until_ms = s.end_ms;
          break;
        }
        case AttentiveState::Relax: {
          // Decay window finished — back to fully Idle. The smoothers have
          // already brought amount + gaze close to zero by now.
          s.phase   = AttentiveState::Idle;
          s.amount  = 0.0f;
          s.gaze_x  = 0.0f;
          s.gaze_y  = 0.0f;
          break;
        }
        default: break;
      }
    }
  }

  // ---- Per-frame smoothing (runs even in Idle so any tiny residuals decay
  //      cleanly to zero on the first idle tick after Relax). ----
  const float tau_amount = (s.target_amount > s.amount) ? TAU_AMOUNT_IN_S
                                                        : TAU_AMOUNT_OUT_S;
  const float k_amount   = 1.0f - expf(-dt / tau_amount);
  s.amount += (s.target_amount - s.amount) * k_amount;
  s.amount = clamp01(s.amount);

  const bool snap_phase = (s.phase == AttentiveState::Glance ||
                           s.phase == AttentiveState::Verify);
  const float tau_gaze  = snap_phase ? TAU_GAZE_SNAP_S : TAU_GAZE_HOLD_S;
  const float k_gaze    = 1.0f - expf(-dt / tau_gaze);
  s.gaze_x += (s.gaze_tx - s.gaze_x) * k_gaze;
  s.gaze_y += (s.gaze_ty - s.gaze_y) * k_gaze;
}

void attentiveModulate(const AttentiveState &s, Modulators &mods) {
  const float a = s.amount;
  if (a <= 0.0f) return;  // fast path — nothing to contribute

  // Facial expression contributions.
  mods.eye_open_add     += a * EYE_OPEN_BOOST;
  mods.pupil_scale_mult *= 1.0f - a * PUPIL_SHRINK;
  mods.lid_upper_droop  -= a * LID_UPPER_RETRACT;

  // Drift focus — narrower, slower, longer holds. Multiplicative so they
  // stack cleanly with sleepy / curiosity contributions instead of fighting.
  mods.drift_range_mult *= 1.0f - a * DRIFT_RANGE_REDUCTION;
  mods.drift_tau_mult   *= 1.0f + a * DRIFT_TAU_INCREASE;
  mods.drift_hold_mult  *= 1.0f + a * DRIFT_HOLD_BOOST;

  // Soft blink suppression. The blink behavior treats blink_inhibit as a
  // gate: when the running total exceeds ~0.3 it defers blink attempts;
  // when it falls back below that, blinks resume on their normal schedule.
  mods.blink_inhibit    += a * BLINK_INHIBIT_AT_PEAK;
}
