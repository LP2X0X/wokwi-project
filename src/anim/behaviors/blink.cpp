#include "blink.h"

#include "../anim_util.h"

namespace {

// Tunables.
constexpr uint16_t BLINK_GAP_MIN_MS  = 2200;
constexpr uint16_t BLINK_GAP_MAX_MS  = 5800;
constexpr uint16_t BLINK_CLOSE_MS    = 90;     // fast snap closed
constexpr uint16_t BLINK_HOLD_MS     = 40;     // brief full-closed hold
constexpr uint16_t BLINK_OPEN_MS     = 160;    // slower lift open
constexpr uint8_t  DOUBLE_BLINK_PCT  = 18;     // % chance after a blink
constexpr uint16_t DOUBLE_GAP_MIN_MS = 90;
constexpr uint16_t DOUBLE_GAP_MAX_MS = 180;

// Asymmetric, three-phase blink curve in [0..1].
//   0 .. close_ms      : ease 0 -> 1   (smoothstep, fast)
//   close_ms .. +hold  : hold at 1
//   +hold .. +open_ms  : ease 1 -> 0   (smoothstep, slower)
//   beyond             : 0 (caller resets blink)
inline float blinkCurve(uint32_t elapsed_ms,
                        uint16_t close_ms, uint16_t hold_ms, uint16_t open_ms) {
  if (elapsed_ms < close_ms) {
    return anim_util::smoothstep01((float)elapsed_ms / (float)close_ms);
  }
  uint32_t t = elapsed_ms - close_ms;
  if (t < hold_ms) return 1.0f;
  t -= hold_ms;
  if (t < open_ms) {
    return 1.0f - anim_util::smoothstep01((float)t / (float)open_ms);
  }
  return 0.0f;
}

inline uint32_t blinkTotalMs(const BlinkState &s) {
  return (uint32_t)s.blink_close_ms + s.blink_hold_ms + s.blink_open_ms;
}

}  // namespace

void blinkInit(BlinkState &s, uint32_t now_ms) {
  s.lid_close       = 0.0f;
  s.blink_start_ms  = 0;
  s.blink_close_ms  = BLINK_CLOSE_MS;
  s.blink_hold_ms   = BLINK_HOLD_MS;
  s.blink_open_ms   = BLINK_OPEN_MS;
  s.blink_next_ms   = now_ms + anim_util::urand(BLINK_GAP_MIN_MS, BLINK_GAP_MAX_MS);
  s.double_pending  = false;
}

void blinkUpdate(BlinkState &s, const Modulators &mods, uint32_t now_ms) {
  using namespace anim_util;

  // 1. Kick off a scheduled blink. Sample the per-blink phase durations
  //    once here, scaled by the emotion modulator. Sampling at start (not
  //    every frame) means a single blink keeps consistent timing even if
  //    sleepy_amount changes during it.
  //
  //    Soft inhibit: if an emotion (attentive listening, etc.) is currently
  //    vetoing blinks, defer this attempt by a short re-check window
  //    instead of starting. When the inhibit fades the next attempt within
  //    ~500 ms goes through, so blinks "pause and resume" rather than fire
  //    at the wrong moment.
  if (s.blink_start_ms == 0 && (int32_t)(now_ms - s.blink_next_ms) >= 0 &&
      mods.blink_inhibit > 0.3f) {
    s.blink_next_ms = now_ms + 500;
  } else if (s.blink_start_ms == 0 && (int32_t)(now_ms - s.blink_next_ms) >= 0) {
    s.blink_start_ms = now_ms;

    const float dur_mult = mods.blink_duration_mult;
    s.blink_close_ms = (uint16_t)((float)BLINK_CLOSE_MS * dur_mult);
    s.blink_hold_ms  = (uint16_t)((float)BLINK_HOLD_MS  * dur_mult);
    s.blink_open_ms  = (uint16_t)((float)BLINK_OPEN_MS  * dur_mult);

    // Optionally extend hold for a "long blink" — chance and range are set
    // by emotion modulators (sleepy contributes both today). Awake stack
    // has chance == 0 so this never fires.
    if (mods.long_blink_chance > 0.0f &&
        frand(0.0f, 1.0f) < mods.long_blink_chance &&
        mods.long_blink_extra_max_ms >= mods.long_blink_extra_min_ms) {
      s.blink_hold_ms = (uint16_t)(s.blink_hold_ms +
                                   urand(mods.long_blink_extra_min_ms,
                                         mods.long_blink_extra_max_ms));
    }
  }

  // 2. No blink in flight: drive lid_close to 0 and bail.
  if (s.blink_start_ms == 0) {
    s.lid_close = 0.0f;
    return;
  }

  uint32_t elapsed = now_ms - s.blink_start_ms;
  uint32_t total   = blinkTotalMs(s);

  if (elapsed < total) {
    s.lid_close = blinkCurve(elapsed, s.blink_close_ms,
                             s.blink_hold_ms, s.blink_open_ms);
    return;
  }

  // 3. Blink finished: reset, schedule the next, maybe queue a double.
  s.lid_close = 0.0f;
  s.blink_start_ms = 0;

  if (s.double_pending) {
    s.double_pending = false;
    s.blink_next_ms = now_ms + urand(DOUBLE_GAP_MIN_MS, DOUBLE_GAP_MAX_MS);
  } else {
    s.blink_next_ms = now_ms + urand(BLINK_GAP_MIN_MS, BLINK_GAP_MAX_MS);
    if ((uint8_t)random(0, 100) < DOUBLE_BLINK_PCT) {
      s.double_pending = true;
    }
  }
}
