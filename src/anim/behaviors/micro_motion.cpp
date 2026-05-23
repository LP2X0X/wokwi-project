#include "micro_motion.h"

#include <math.h>

#include "../anim_util.h"

namespace {

// Tunables — kept private to this translation unit so other modules can't
// silently grow a dependency on them.
//
// FPS-INVARIANT BY DESIGN. All knobs below are in WALL-CLOCK units (seconds
// and milliseconds). The smoother uses `k = 1 - exp(-dt/tau)`, which is
// mathematically equivalent over the same wall-clock interval regardless of
// how `dt` is sliced — so changing FRAME_INTERVAL_MS in main.cpp does NOT
// change the perceived drift speed. Higher fps only means smoother rendered
// motion (more sub-pixel dither resolution, no integer-snap stepping); it
// does not make the eye drift faster.
//
// One caveat: `eyeStateUpdate()` clamps `dt` at 0.1 s to bound the smoother
// during paused tabs / lost frames. If you intentionally drop below ~10 fps
// the clamp will start to compress motion (the smoother will "fall behind"
// wall clock). At the supported 50–120 fps range it is never engaged.
//
// To slow/speed perceived motion, change THESE values, not FRAME_INTERVAL_MS:
//   * range  -> drift amplitude in pixels
//   * tau    -> easing softness in seconds (bigger = slower glide)
//   * hold   -> wall-clock duration between random target re-rolls
constexpr float    DRIFT_RANGE_PX    = 1.0f;
constexpr float    DRIFT_TAU_S       = 0.55f;
constexpr uint16_t DRIFT_HOLD_MIN_MS = 900;
constexpr uint16_t DRIFT_HOLD_MAX_MS = 3000;

}  // namespace

void microMotionInit(MicroMotionState &s, uint32_t now_ms) {
  s.drift_x = 0.0f;
  s.drift_y = 0.0f;
  s.drift_tx = 0.0f;
  s.drift_ty = 0.0f;
  s.drift_next_ms = now_ms;  // forces a target roll on the first update
}

void microMotionUpdate(MicroMotionState &s,
                       const Modulators  &mods,
                       const GazeIntent  &gaze,
                       uint32_t           now_ms,
                       float              dt) {
  using namespace anim_util;

  // 1. Re-roll the random drift target when the hold timer expires.
  //    Range shrinks and hold duration grows when emotions raise the
  //    appropriate modulators (sleepy, tired, ...). Range never goes negative
  //    and hold never goes below the base — modulators are checked at write
  //    time so anything like negative range is a programming error elsewhere.
  if ((int32_t)(now_ms - s.drift_next_ms) >= 0) {
    const float range = DRIFT_RANGE_PX * mods.drift_range_mult;
    s.drift_tx = frand(-range, range);
    s.drift_ty = frand(-range, range);

    const uint32_t base = urand(DRIFT_HOLD_MIN_MS, DRIFT_HOLD_MAX_MS);
    s.drift_next_ms = now_ms + (uint32_t)((float)base * mods.drift_hold_mult);
  }

  // 2. Compute the *effective* target this frame. If gaze is active we blend
  //    the random target with the external gaze target by weight; the result
  //    feeds the same smoothing filter, so a face-tracking signal arrives as
  //    a smooth eye follow rather than a hard snap. weight = 0 reproduces
  //    pure micro motion exactly.
  float effective_tx = s.drift_tx;
  float effective_ty = s.drift_ty;
  if (gaze.active && gaze.weight > 0.0f) {
    const float w = clamp01(gaze.weight);
    effective_tx = lerp(s.drift_tx, gaze.target_x, w);
    effective_ty = lerp(s.drift_ty, gaze.target_y, w);
  }

  // 3. Critically-damped exponential smoothing. tau is scaled by the
  //    modulator stack — sleepy makes it bigger so the eye feels heavier.
  const float tau = DRIFT_TAU_S * mods.drift_tau_mult;
  const float k   = 1.0f - expf(-dt / tau);
  s.drift_x += (effective_tx - s.drift_x) * k;
  s.drift_y += (effective_ty - s.drift_y) * k;
}
