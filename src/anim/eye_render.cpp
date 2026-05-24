#include "eye_render.h"

#include <math.h>

namespace {

// Eyelid bow at the most extreme positions, expressed as a fraction of
// sclera bbox height. At the eye center the bow is 0 (flat line).
constexpr float LID_CURVE_FRACTION = 0.22f;

// Filled axis-aligned ellipse via TFT_eSprite. Plain fillCircle /
// fillEllipse — anti-aliasing was tried (fillSmoothCircle) but on the
// Wokwi simulator the per-pixel coverage math doubled per-frame CPU
// cost without a visible quality win at this resolution. Real-hardware
// builds can swap back to fillSmoothCircle if the slight jaggies show.
void fillEyeBlob(TFT_eSprite &spr,
                 int16_t cx, int16_t cy, int16_t rx, int16_t ry,
                 uint16_t fg_color, uint16_t /*bg_color*/) {
  if (rx <= 0 || ry <= 0) return;
  if (rx == ry) {
    spr.fillCircle(cx, cy, rx, fg_color);
  } else {
    spr.fillEllipse(cx, cy, rx, ry, fg_color);
  }
}

// Procedural eyelid as a parabolic arc clipped to the sclera bbox by
// painting BACKGROUND-colored columns. Painting BG over BG pixels is a
// no-op, so the visible lid edge naturally follows the sclera silhouette
// (circle or ellipse) — same trick as the SSD1306 era, just over RGB565.
void drawCurvedLid(TFT_eSprite &spr,
                   int16_t bbox_x, int16_t bbox_y,
                   int16_t bbox_w, int16_t bbox_h,
                   float center_y, float curvature, bool fill_from_top,
                   uint16_t bg_color) {
  const float a   = (float)bbox_w * 0.5f;
  const float cx  = (float)bbox_x + a;
  const float dip = (float)bbox_h * LID_CURVE_FRACTION;

  const int16_t y_top = bbox_y;
  const int16_t y_bot = bbox_y + bbox_h - 1;
  const int16_t x_lo  = bbox_x;
  const int16_t x_hi  = bbox_x + bbox_w;

  for (int16_t x = x_lo; x < x_hi; ++x) {
    const float xn    = ((float)x - cx) / a;
    const float bow   = curvature * dip * (1.0f - xn * xn);
    const float y_lid = center_y + bow;
    int16_t     y     = (int16_t)lroundf(y_lid);

    if (fill_from_top) {
      if (y < y_top) continue;
      if (y > y_bot) y = y_bot;
      spr.drawFastVLine(x, y_top, (int16_t)(y - y_top + 1), bg_color);
    } else {
      if (y > y_bot) continue;
      if (y < y_top) y = y_top;
      spr.drawFastVLine(x, y, (int16_t)(y_bot - y + 1), bg_color);
    }
  }
}

// Colors. Kept in one place so the future GC9A01 swap (or a "blue eye"
// variant, etc.) can be done by changing two constants instead of hunting
// through the file.
constexpr uint16_t COLOR_BG     = TFT_BLACK;
constexpr uint16_t COLOR_SCLERA = TFT_WHITE;
constexpr uint16_t COLOR_PUPIL  = TFT_BLACK;

constexpr int16_t BOUNDS_MARGIN = 1;

inline int16_t clampCoord(int32_t v, int16_t lo, int16_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return (int16_t)v;
}

inline bool rectValid(const SpriteRect &r) {
  return r.w > 0 && r.h > 0;
}

SpriteRect clipRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                    int16_t clip_w, int16_t clip_h) {
  if (x0 > x1 || y0 > y1) return {0, 0, 0, 0};

  x0 = clampCoord(x0, 0, (int16_t)(clip_w - 1));
  y0 = clampCoord(y0, 0, (int16_t)(clip_h - 1));
  x1 = clampCoord(x1, 0, (int16_t)(clip_w - 1));
  y1 = clampCoord(y1, 0, (int16_t)(clip_h - 1));

  return {
    x0,
    y0,
    (int16_t)(x1 - x0 + 1),
    (int16_t)(y1 - y0 + 1),
  };
}

}  // namespace

void renderEye(TFT_eSprite &spr, const Eye &eye, const EyePose &pose) {
  // Scale knobs on top of the static geometry:
  //   pose.eye_open_amount → vertical sclera stretch (curiosity ~2.0).
  //                          Width is untouched so the eye gets TALLER,
  //                          not larger overall.
  //   pose.pupil_scale     → uniform multiplier on the pupil radius.
  //   eye.scale            → per-display zoom for the ANIMATED drift only
  //                          (geometry radii are absolute).
  const float open_amt  = pose.eye_open_amount > 0.0f ? pose.eye_open_amount : 1.0f;
  const float pupil_amt = pose.pupil_scale     > 0.0f ? pose.pupil_scale     : 1.0f;

  const int16_t sclera_rx = eye.sclera_r;
  const int16_t sclera_ry = (int16_t)lroundf((float)eye.sclera_r * open_amt);

  // 1. Sclera. Anti-aliased when not stretched; ellipse fallback otherwise.
  fillEyeBlob(spr, eye.sclera_cx, eye.sclera_cy,
              sclera_rx, sclera_ry, COLOR_SCLERA, COLOR_BG);

  // 2. Pupil. Anti-aliased against the sclera color so the iris edge
  //    blends cleanly into the white instead of stair-stepping.
  const float extra_x = (eye.side == EyeSide::Left)
      ? pose.pupil_dx_l_extra : pose.pupil_dx_r_extra;
  const float extra_y = (eye.side == EyeSide::Left)
      ? pose.pupil_dy_l_extra : pose.pupil_dy_r_extra;
  const int16_t pupil_cx = eye.sclera_cx + eye.pupil_dx_neutral +
                           (int16_t)lroundf((pose.pupil_dx + extra_x) * eye.scale);
  const int16_t pupil_cy = eye.sclera_cy + eye.pupil_dy_neutral +
                           (int16_t)lroundf((pose.pupil_dy + extra_y) * eye.scale);
  const int16_t pupil_r  = (int16_t)lroundf((float)eye.pupil_r * pupil_amt);
  if (pupil_r > 0) {
    spr.fillCircle(pupil_cx, pupil_cy, pupil_r, COLOR_PUPIL);
  }

  // 3. & 4. Upper + lower eyelids. bbox_w/h = 2*r + 1 (NOT 2*r) so the
  //         lid loop covers the inclusive [-r, +r] range that
  //         fillSmoothCircle / fillEllipse paint — otherwise a 1-px
  //         sliver of sclera stays visible at the lateral edges during a
  //         full blink (the parabolic bow goes to 0 at xn = ±1).
  const int16_t bbox_x = eye.sclera_cx - sclera_rx;
  const int16_t bbox_y = eye.sclera_cy - sclera_ry;
  const int16_t bbox_w = (int16_t)(2 * sclera_rx + 1);
  const int16_t bbox_h = (int16_t)(2 * sclera_ry + 1);
  const float   lid_b  = (float)sclera_ry;
  const float   lid_cy = (float)eye.sclera_cy;
  const float upper_off = pose.upper_lid_amount * (float)bbox_h;
  const float lower_off = pose.lower_lid_amount * (float)bbox_h;

  if (upper_off > 0.0f) {
    const float upper_y = (float)bbox_y + upper_off;
    drawCurvedLid(spr, bbox_x, bbox_y, bbox_w, bbox_h,
                  upper_y, (upper_y - lid_cy) / lid_b,
                  /*fill_from_top=*/true, COLOR_BG);
  }
  if (lower_off > 0.0f) {
    const float lower_y = (float)bbox_y + (float)bbox_h - lower_off;
    drawCurvedLid(spr, bbox_x, bbox_y, bbox_w, bbox_h,
                  lower_y, (lower_y - lid_cy) / lid_b,
                  /*fill_from_top=*/false, COLOR_BG);
  }
}

void renderEyeOn(TFT_eSprite &spr, const Eye &eye, const EyePose &pose) {
  spr.fillSprite(COLOR_BG);
  renderEye(spr, eye, pose);
  // Caller pushes the sprite to whichever physical display they want by
  // asserting that display's CS pin around spr.pushSprite(0, 0).
}

void renderEyes(TFT_eSprite &spr,
                const Eye &left, const Eye &right,
                const EyePose &pose) {
  spr.fillSprite(COLOR_BG);
  renderEye(spr, left,  pose);
  renderEye(spr, right, pose);
  // Caller pushes the sprite. See renderEyeOn().
}

SpriteRect eyeBounds(const Eye &eye, const EyePose &pose,
                     int16_t clip_w, int16_t clip_h) {
  const float open_amt  = pose.eye_open_amount > 0.0f ? pose.eye_open_amount : 1.0f;
  const float pupil_amt = pose.pupil_scale     > 0.0f ? pose.pupil_scale     : 1.0f;

  const int16_t sclera_rx = eye.sclera_r;
  const int16_t sclera_ry = (int16_t)lroundf((float)eye.sclera_r * open_amt);

  const float extra_x = (eye.side == EyeSide::Left)
      ? pose.pupil_dx_l_extra : pose.pupil_dx_r_extra;
  const float extra_y = (eye.side == EyeSide::Left)
      ? pose.pupil_dy_l_extra : pose.pupil_dy_r_extra;
  const int16_t pupil_cx = eye.sclera_cx + eye.pupil_dx_neutral +
                           (int16_t)lroundf((pose.pupil_dx + extra_x) * eye.scale);
  const int16_t pupil_cy = eye.sclera_cy + eye.pupil_dy_neutral +
                           (int16_t)lroundf((pose.pupil_dy + extra_y) * eye.scale);
  const int16_t pupil_r  = (int16_t)lroundf((float)eye.pupil_r * pupil_amt);

  int16_t x0 = (int16_t)(eye.sclera_cx - sclera_rx - BOUNDS_MARGIN);
  int16_t y0 = (int16_t)(eye.sclera_cy - sclera_ry - BOUNDS_MARGIN);
  int16_t x1 = (int16_t)(eye.sclera_cx + sclera_rx + BOUNDS_MARGIN);
  int16_t y1 = (int16_t)(eye.sclera_cy + sclera_ry + BOUNDS_MARGIN);

  if (pupil_r > 0) {
    x0 = (int16_t)min((int32_t)x0, (int32_t)(pupil_cx - pupil_r - BOUNDS_MARGIN));
    y0 = (int16_t)min((int32_t)y0, (int32_t)(pupil_cy - pupil_r - BOUNDS_MARGIN));
    x1 = (int16_t)max((int32_t)x1, (int32_t)(pupil_cx + pupil_r + BOUNDS_MARGIN));
    y1 = (int16_t)max((int32_t)y1, (int32_t)(pupil_cy + pupil_r + BOUNDS_MARGIN));
  }

  return clipRect(x0, y0, x1, y1, clip_w, clip_h);
}

SpriteRect boundsUnion(const SpriteRect &a, const SpriteRect &b) {
  if (!rectValid(a)) return b;
  if (!rectValid(b)) return a;

  const int16_t x0 = (int16_t)min((int32_t)a.x, (int32_t)b.x);
  const int16_t y0 = (int16_t)min((int32_t)a.y, (int32_t)b.y);
  const int16_t x1 = (int16_t)max((int32_t)(a.x + a.w - 1), (int32_t)(b.x + b.w - 1));
  const int16_t y1 = (int16_t)max((int32_t)(a.y + a.h - 1), (int32_t)(b.y + b.h - 1));

  return {x0, y0, (int16_t)(x1 - x0 + 1), (int16_t)(y1 - y0 + 1)};
}

SpriteRect previewPushBounds(const Eye &left, const Eye &right,
                             const EyePose &pose,
                             int16_t clip_w, int16_t clip_h) {
  return boundsUnion(
      eyeBounds(left, pose, clip_w, clip_h),
      eyeBounds(right, pose, clip_w, clip_h));
}
