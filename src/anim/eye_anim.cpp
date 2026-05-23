#include "eye_anim.h"

#include <math.h>
#include <esp_random.h>

// ---------- Tunables (kept here, not in the header, so they're easy to find).
namespace {

// Micro motion: how far the pupil drifts and how fast it eases toward targets.
// Range is in pixels; tau is in seconds (critically-damped low-pass).
constexpr float    DRIFT_RANGE_PX   = 2.0f;
constexpr float    DRIFT_TAU_S      = 0.35f;
constexpr uint16_t DRIFT_HOLD_MIN_MS = 500;
constexpr uint16_t DRIFT_HOLD_MAX_MS = 1800;

// Blink scheduling. Long, irregular gaps so the loop never feels metric.
constexpr uint16_t BLINK_GAP_MIN_MS  = 2200;
constexpr uint16_t BLINK_GAP_MAX_MS  = 5800;
constexpr uint16_t BLINK_CLOSE_MS    = 90;     // fast snap closed
constexpr uint16_t BLINK_HOLD_MS     = 40;     // brief full-closed hold
constexpr uint16_t BLINK_OPEN_MS     = 160;    // slower lift open
constexpr uint8_t  DOUBLE_BLINK_PCT  = 18;     // % chance after a blink
constexpr uint16_t DOUBLE_GAP_MIN_MS = 90;
constexpr uint16_t DOUBLE_GAP_MAX_MS = 180;

// Eyelid bow at the most extreme positions (lid centerline at the very top or
// bottom of the eye), expressed as a fraction of sclera height. At the eye
// center the bow is 0 (flat line). Larger values = more pronounced spherical
// perspective. Shared by upper and lower lids.
constexpr float LID_CURVE_FRACTION = 0.22f;

// Lower lid travel as a fraction of the full eye height. The upper lid covers
// the full height at lid_close=1; the lower lid only rises this much in the
// same time, matching real-eye anatomy where the upper lid does ~80% of the
// closing motion.
constexpr float LOWER_LID_TRAVEL_FRACTION = 0.30f;

// ---- sleepy mode ----
// All sleepy effects scale linearly with sleepy_amount (0..1) so behavior is
// continuous. At sleepy_amount = 0 every multiplier below contributes nothing
// and the animation matches the awake baseline exactly.

// Easing tau for sleepy_amount itself — long, so transitions in/out of
// sleepiness feel like a slow mood change, not a switch.
constexpr float    SLEEPY_TAU_S                  = 1.5f;

// Autonomous re-roll cadence: how long the toy stays at one sleepy_target
// before drifting to a new one. 8–25 s gives a believable "mood" rhythm.
constexpr uint16_t SLEEPY_HOLD_MIN_MS            = 8000;
constexpr uint16_t SLEEPY_HOLD_MAX_MS            = 25000;

// Lid biases at full sleepy (sleepy_amount = 1). Expressed as fractions of
// sclera height so they scale automatically with eye size.
constexpr float    SLEEPY_UPPER_DROOP_FRACTION   = 0.4f;  // upper lid sags past mid
constexpr float    SLEEPY_LOWER_RISE_FRACTION    = 0.2f;  // lower lid puffs noticeably

// Static downward bias on the pupil at full sleepy, in pixels. Stacks on top
// of micro-motion drift_y, so the pupil still wanders but its center sinks.
constexpr float    SLEEPY_PUPIL_BIAS_PX          = 1.5f;

// Drift modulation: heavier feel when sleepy.
constexpr float    SLEEPY_DRIFT_TAU_MULT         = 1.5f;   // tau up to 2.5x slower
constexpr float    SLEEPY_DRIFT_RANGE_REDUCTION  = 0.5f;   // wander 50% less far
constexpr float    SLEEPY_DRIFT_HOLD_MULT        = 1.5f;   // hold up to 2.5x longer

// Blink modulation: slower close/hold/open + occasional very long sleepy blink.
constexpr float    SLEEPY_BLINK_DURATION_MULT    = 1.5f;   // up to 2.5x slower
constexpr uint8_t  SLEEPY_LONG_BLINK_PCT_AT_FULL = 30;     // 0..30% chance scaled by sleepy
constexpr uint16_t SLEEPY_LONG_BLINK_HOLD_MIN_MS = 300;
constexpr uint16_t SLEEPY_LONG_BLINK_HOLD_MAX_MS = 900;

// ---------- small helpers ----------

inline float frand(float lo, float hi) {
  return lo + (hi - lo) * (random(0, 10001) / 10000.0f);
}

inline uint32_t urand(uint32_t lo, uint32_t hi) {
  return random(lo, hi + 1);
}

inline float smoothstep01(float x) {
  if (x <= 0.0f) return 0.0f;
  if (x >= 1.0f) return 1.0f;
  return x * x * (3.0f - 2.0f * x);
}

// Asymmetric, three-phase blink curve in [0..1].
//   0 .. close_ms      : ease 0 -> 1   (smoothstep, fast)
//   close_ms .. +hold  : hold at 1
//   +hold .. +open_ms  : ease 1 -> 0   (smoothstep, slower)
//   beyond             : 0 (caller resets blink)
inline float blinkCurve(uint32_t elapsed_ms,
                        uint16_t close_ms, uint16_t hold_ms, uint16_t open_ms) {
  if (elapsed_ms < close_ms) {
    return smoothstep01((float)elapsed_ms / (float)close_ms);
  }
  uint32_t t = elapsed_ms - close_ms;
  if (t < hold_ms) return 1.0f;
  t -= hold_ms;
  if (t < open_ms) {
    return 1.0f - smoothstep01((float)t / (float)open_ms);
  }
  return 0.0f;
}

inline uint32_t blinkTotalMs(const EyeState &s) {
  return (uint32_t)s.blink_close_ms + s.blink_hold_ms + s.blink_open_ms;
}

}  // namespace

// ---------- public API ----------

void eyeStateInit(EyeState &s, uint32_t now_ms) {
  randomSeed(esp_random());

  s.drift_x = s.drift_y = 0.0f;
  s.drift_tx = s.drift_ty = 0.0f;
  s.drift_next_ms = now_ms;

  s.lid_close = 0.0f;
  s.blink_start_ms = 0;
  s.blink_close_ms = BLINK_CLOSE_MS;
  s.blink_hold_ms  = BLINK_HOLD_MS;
  s.blink_open_ms  = BLINK_OPEN_MS;
  s.blink_next_ms  = now_ms + urand(BLINK_GAP_MIN_MS, BLINK_GAP_MAX_MS);
  s.double_pending = false;

  s.sleepy_amount  = 0.0f;
  s.sleepy_target  = 0.0f;
  // First re-roll comes early so the autonomous mood starts varying soon
  // after boot rather than holding a flat "awake" baseline for 25s.
  s.sleepy_next_ms = now_ms + urand(2000, 6000);

  s.last_update_ms = now_ms;
}

void eyeStateSetSleepy(EyeState &s, float target) {
  if (target < 0.0f) target = 0.0f;
  if (target > 1.0f) target = 1.0f;
  s.sleepy_target = target;
}

void updateSleepyState(EyeState &s, uint32_t now_ms, float dt) {
  // Autonomous mood drift: re-roll a new target every 8–25 s using a soft
  // distribution. Most of the time the toy is awake/lightly drowsy; rarely
  // it gets very sleepy — same shape as how a real little creature drifts.
  if ((int32_t)(now_ms - s.sleepy_next_ms) >= 0) {
    const float bucket = frand(0.0f, 1.0f);
    if (bucket < 0.55f) {
      s.sleepy_target = frand(0.00f, 0.20f);    // bright, alert
    } else if (bucket < 0.90f) {
      s.sleepy_target = frand(0.20f, 0.55f);    // mildly drowsy
    } else {
      s.sleepy_target = frand(0.65f, 1.00f);    // very sleepy
    }
    s.sleepy_next_ms = now_ms + urand(SLEEPY_HOLD_MIN_MS, SLEEPY_HOLD_MAX_MS);
  }

  // Critically-damped easing toward target with a long tau — gives the soft,
  // organic transition the user asked for.
  const float k = 1.0f - expf(-dt / SLEEPY_TAU_S);
  s.sleepy_amount += (s.sleepy_target - s.sleepy_amount) * k;
  if (s.sleepy_amount < 0.0f) s.sleepy_amount = 0.0f;
  if (s.sleepy_amount > 1.0f) s.sleepy_amount = 1.0f;
}

void updateMicroMotion(EyeState &s, uint32_t now_ms, float dt) {
  const float sleepiness = s.sleepy_amount;

  // Pick a new drift target when the hold timer elapses. Range shrinks and
  // hold duration grows when sleepy — wandering becomes less frequent and
  // less far, but never stops entirely.
  if ((int32_t)(now_ms - s.drift_next_ms) >= 0) {
    const float range = DRIFT_RANGE_PX *
                        (1.0f - sleepiness * SLEEPY_DRIFT_RANGE_REDUCTION);
    s.drift_tx = frand(-range, range);
    s.drift_ty = frand(-range, range);

    const float    hold_mult = 1.0f + sleepiness * SLEEPY_DRIFT_HOLD_MULT;
    const uint32_t base      = urand(DRIFT_HOLD_MIN_MS, DRIFT_HOLD_MAX_MS);
    s.drift_next_ms = now_ms + (uint32_t)((float)base * hold_mult);
  }

  // Critically-damped exponential smoothing toward target. Tau grows with
  // sleepiness, so movement feels heavier without ever locking up.
  const float tau = DRIFT_TAU_S * (1.0f + sleepiness * SLEEPY_DRIFT_TAU_MULT);
  const float k   = 1.0f - expf(-dt / tau);
  s.drift_x += (s.drift_tx - s.drift_x) * k;
  s.drift_y += (s.drift_ty - s.drift_y) * k;
}

void updateBlink(EyeState &s, uint32_t now_ms) {
  // Kick off a scheduled blink.
  if (s.blink_start_ms == 0 && (int32_t)(now_ms - s.blink_next_ms) >= 0) {
    s.blink_start_ms = now_ms;

    // Sample sleepy-modulated phase durations once at the start of each
    // blink, so individual blinks have consistent timing all the way through
    // even if sleepy_amount changes mid-blink.
    const float dur_mult = 1.0f + s.sleepy_amount * SLEEPY_BLINK_DURATION_MULT;
    s.blink_close_ms = (uint16_t)((float)BLINK_CLOSE_MS * dur_mult);
    s.blink_hold_ms  = (uint16_t)((float)BLINK_HOLD_MS  * dur_mult);
    s.blink_open_ms  = (uint16_t)((float)BLINK_OPEN_MS  * dur_mult);

    // Occasionally: a "really tired" blink with extra hold. Probability scales
    // with sleepiness, so awake eyes never produce one and very sleepy eyes
    // produce them ~30% of blinks.
    const uint8_t long_pct = (uint8_t)(s.sleepy_amount *
                                       (float)SLEEPY_LONG_BLINK_PCT_AT_FULL);
    if (long_pct > 0 && (uint8_t)random(0, 100) < long_pct) {
      s.blink_hold_ms = (uint16_t)(s.blink_hold_ms +
                                   urand(SLEEPY_LONG_BLINK_HOLD_MIN_MS,
                                         SLEEPY_LONG_BLINK_HOLD_MAX_MS));
    }
  }

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

  // Blink finished — reset and schedule the next one.
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

void eyeStateUpdate(EyeState &s, uint32_t now_ms) {
  uint32_t prev = s.last_update_ms ? s.last_update_ms : now_ms;
  float dt = (now_ms - prev) / 1000.0f;
  if (dt < 0.0f)   dt = 0.0f;
  if (dt > 0.1f)   dt = 0.1f;     // clamp big dt (paused tab, etc.)
  s.last_update_ms = now_ms;

  // Layered behaviors — order matters because later layers READ what earlier
  // layers wrote into EyeState (no layer ever reads its own output, so it
  // stays cycle-free):
  //   1. sleepy   — produces sleepy_amount, consumed by 2 + 3 and by render.
  //   2. drift    — uses sleepy_amount to scale tau / range / hold.
  //   3. blink    — uses sleepy_amount when sampling per-blink durations.
  updateSleepyState(s, now_ms, dt);
  updateMicroMotion(s, now_ms, dt);
  updateBlink(s, now_ms);
}

// ---------- rendering ----------

namespace {

// Draws a single procedural eyelid as a curved arc clipped to the eye bbox.
//
//   center_y       Lid centerline y at the column x = eye_center_x. The bow
//                  per column is added to this.
//   curvature      Sign + magnitude of the bow. Convention matches the
//                  spherical-perspective rule: curvature < 0 -> ∩, > 0 -> ∪.
//                  Magnitude is clamped to [-1, 1] in practice (we always
//                  pass `(centerline_y - eye_cy) / b`).
//   fill_from_top  true  -> upper lid: paint BLACK from the top of the eye
//                            down to the lid line.
//                  false -> lower lid: paint BLACK from the lid line down to
//                            the bottom of the eye.
//
// Painting BLACK on already-off pixels is a no-op, so columns where the lid
// is entirely past the eye on the wrong side are simply skipped, and the
// visible lid edge is naturally clipped to the eye silhouette.
void drawCurvedLid(Adafruit_SSD1306 &d, const Eye &eye,
                   float center_y, float curvature, bool fill_from_top) {
  const float a   = (float)eye.sclera.w * 0.5f;
  const float cx  = (float)eye.sclera.x + a;
  const float dip = (float)eye.sclera.h * LID_CURVE_FRACTION;

  const int16_t y_top = eye.sclera.y;
  const int16_t y_bot = eye.sclera.y + eye.sclera.h - 1;
  const int16_t x_lo  = eye.sclera.x;
  const int16_t x_hi  = eye.sclera.x + eye.sclera.w;

  for (int16_t x = x_lo; x < x_hi; ++x) {
    const float xn    = ((float)x - cx) / a;        // -1 .. +1
    const float bow   = curvature * dip * (1.0f - xn * xn);
    const float y_lid = center_y + bow;
    int16_t     y     = (int16_t)lroundf(y_lid);

    if (fill_from_top) {
      if (y < y_top) continue;       // lid still above this column
      if (y > y_bot) y = y_bot;
      d.drawFastVLine(x, y_top, (int16_t)(y - y_top + 1), SSD1306_BLACK);
    } else {
      if (y > y_bot) continue;       // lid still below this column
      if (y < y_top) y = y_top;
      d.drawFastVLine(x, y, (int16_t)(y_bot - y + 1), SSD1306_BLACK);
    }
  }
}

}  // namespace

void renderEye(Adafruit_SSD1306 &d, const Eye &eye, const EyeState &s) {
  const float h  = (float)eye.sclera.h;
  const float b  = h * 0.5f;
  const float cy = (float)eye.sclera.y + b;

  // 1. Sclera (static white blob).
  d.drawBitmap(eye.sclera.x, eye.sclera.y,
               eye.sclera.bmp, eye.sclera.w, eye.sclera.h,
               SSD1306_WHITE);

  // 2. Pupil — drawn in BLACK so it punches a hole through the sclera.
  //    Final pupil offset = micro-motion drift + sleepy downward bias.
  //    Both stack additively; the bias just shifts the wander center down.
  const float sleepy_pupil_dy = s.sleepy_amount * SLEEPY_PUPIL_BIAS_PX;
  const int16_t px = eye.pupil.x + (int16_t)lroundf(s.drift_x);
  const int16_t py = eye.pupil.y + (int16_t)lroundf(s.drift_y + sleepy_pupil_dy);
  d.drawBitmap(px, py, eye.pupil.bmp, eye.pupil.w, eye.pupil.h,
               SSD1306_BLACK);

  // 3. & 4. Upper + lower eyelids — additive blend of blink + sleepy.
  //
  //   Each lid's "offset" is measured in pixels from its resting edge:
  //     upper_offset = how far the upper lid centerline has descended from
  //                    the top of the eye.
  //     lower_offset = how far the lower lid centerline has risen from the
  //                    bottom of the eye.
  //
  //   Per the user's blending rule:
  //     upper_offset = blink_upper_offset + sleepy_droop_offset       (clamp)
  //     lower_offset = blink_lower_offset + sleepy_rise_offset        (clamp)
  //
  //   Curvature still falls out of the unified perspective rule
  //     curvature = (centerline_y - eye_cy) / b
  //   so a sleepy droop alone (no blink) automatically gets a soft ∩ shape,
  //   and combined with a partial blink the curvature smoothly slides toward
  //   ∪ as the lid passes the eye center — no special cases.
  const float blink_upper_off  = s.lid_close * h;
  const float blink_lower_off  = s.lid_close * LOWER_LID_TRAVEL_FRACTION * h;
  const float sleepy_upper_off = s.sleepy_amount * SLEEPY_UPPER_DROOP_FRACTION * h;
  const float sleepy_lower_off = s.sleepy_amount * SLEEPY_LOWER_RISE_FRACTION  * h;

  float upper_off = blink_upper_off + sleepy_upper_off;
  float lower_off = blink_lower_off + sleepy_lower_off;
  if (upper_off < 0.0f) upper_off = 0.0f;
  if (upper_off > h)    upper_off = h;
  if (lower_off < 0.0f) lower_off = 0.0f;
  if (lower_off > h)    lower_off = h;

  if (upper_off > 0.0f) {
    const float upper_y = (float)eye.sclera.y + upper_off;
    drawCurvedLid(d, eye, upper_y, (upper_y - cy) / b, /*fill_from_top=*/true);
  }
  if (lower_off > 0.0f) {
    const float lower_y = (float)eye.sclera.y + h - lower_off;
    drawCurvedLid(d, eye, lower_y, (lower_y - cy) / b, /*fill_from_top=*/false);
  }
}

void renderEyes(Adafruit_SSD1306 &d,
                const Eye &left, const Eye &right,
                const EyeState &s) {
  d.clearDisplay();
  renderEye(d, left,  s);
  renderEye(d, right, s);
  d.display();
}
