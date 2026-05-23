#include "eye_render.h"

#include <math.h>

namespace {

// Eyelid bow at the most extreme positions (lid centerline at the very top
// or bottom of the eye), expressed as a fraction of sclera bbox height. At
// the eye center the bow is 0 (flat line). Larger values = more pronounced
// spherical perspective.
constexpr float LID_CURVE_FRACTION = 0.22f;

// Filled axis-aligned ellipse via horizontal scanlines. For rx == ry we
// dispatch to Adafruit_GFX::fillCircle which uses the midpoint algorithm
// and is faster. drawFastHLine handles off-screen clipping silently, so the
// circle / ellipse can overflow the display without explicit bounds checks.
void fillEllipse(Adafruit_SSD1306 &d, int16_t cx, int16_t cy,
                 int16_t rx, int16_t ry, uint16_t color) {
  if (rx <= 0 || ry <= 0) return;
  if (rx == ry) { d.fillCircle(cx, cy, rx, color); return; }
  const float ryf = (float)ry;
  for (int16_t dy = -ry; dy <= ry; ++dy) {
    const float t = (float)dy / ryf;
    const float dx_max = (float)rx * sqrtf(1.0f - t * t);
    const int16_t dx = (int16_t)lroundf(dx_max);
    if (dx <= 0) continue;
    d.drawFastHLine(cx - dx, cy + dy, (int16_t)(2 * dx + 1), color);
  }
}

// Draws a single procedural eyelid as a parabolic arc clipped to the sclera
// bbox by painting BLACK columns. Painting BLACK on already-off pixels is a
// no-op, so the visible lid edge naturally follows the sclera silhouette
// (circle, ellipse — anything bounded by the bbox).
//
//   center_y       Lid centerline y at the column x = bbox_cx. The per-
//                  column bow is added to this.
//   curvature      Sign + magnitude of the bow:
//                    < 0  -> ∩ (lid above eye center)
//                    == 0 -> flat
//                    > 0  -> ∪ (lid below eye center)
//                  We always pass `(centerline_y - eye_cy) / ry`, which is
//                  in [-1, 1] for any reachable lid position.
//   fill_from_top  true  -> upper lid: paint BLACK from the top of the bbox
//                            down to the lid line.
//                  false -> lower lid: paint BLACK from the lid line down
//                            to the bottom of the bbox.
void drawCurvedLid(Adafruit_SSD1306 &d,
                   int16_t bbox_x, int16_t bbox_y,
                   int16_t bbox_w, int16_t bbox_h,
                   float center_y, float curvature, bool fill_from_top) {
  const float a   = (float)bbox_w * 0.5f;
  const float cx  = (float)bbox_x + a;
  const float dip = (float)bbox_h * LID_CURVE_FRACTION;

  const int16_t y_top = bbox_y;
  const int16_t y_bot = bbox_y + bbox_h - 1;
  const int16_t x_lo  = bbox_x;
  const int16_t x_hi  = bbox_x + bbox_w;

  for (int16_t x = x_lo; x < x_hi; ++x) {
    const float xn    = ((float)x - cx) / a;        // -1 .. +1
    const float bow   = curvature * dip * (1.0f - xn * xn);
    const float y_lid = center_y + bow;
    int16_t     y     = (int16_t)lroundf(y_lid);

    if (fill_from_top) {
      if (y < y_top) continue;
      if (y > y_bot) y = y_bot;
      d.drawFastVLine(x, y_top, (int16_t)(y - y_top + 1), SSD1306_BLACK);
    } else {
      if (y > y_bot) continue;
      if (y < y_top) y = y_top;
      d.drawFastVLine(x, y, (int16_t)(y_bot - y + 1), SSD1306_BLACK);
    }
  }
}

}  // namespace

void renderEye(Adafruit_SSD1306 &d, const Eye &eye, const EyePose &pose) {
  // Three scale knobs on top of the static geometry:
  //   pose.eye_open_amount → stretches the sclera VERTICALLY. 1.0 = circle,
  //                          ~2.0 = curiosity peak (tall ellipse like a
  //                          Ghibli soot creature noticing something).
  //                          Width is untouched so the eye gets taller,
  //                          not larger overall.
  //   pose.pupil_scale     → uniform multiplier on the pupil radius.
  //   eye.scale            → per-display zoom for the ANIMATED drift only
  //                          (geometry radii are absolute).
  const float open_amt  = pose.eye_open_amount > 0.0f ? pose.eye_open_amount : 1.0f;
  const float pupil_amt = pose.pupil_scale     > 0.0f ? pose.pupil_scale     : 1.0f;

  const int16_t sclera_rx = eye.sclera_r;
  const int16_t sclera_ry = (int16_t)lroundf((float)eye.sclera_r * open_amt);

  // 1. Sclera — white filled circle (or vertical ellipse when stretched).
  fillEllipse(d, eye.sclera_cx, eye.sclera_cy,
              sclera_rx, sclera_ry, SSD1306_WHITE);

  // 2. Pupil — black filled circle. Punches a hole through the sclera.
  //    Position = sclera center + per-eye neutral offset + scaled drift.
  //    Drift is scaled by eye.scale so behaviors tuned in source-pixel
  //    units (e.g. ±1.5 px) keep their perceived amplitude on bigger
  //    physical eyes.
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
    d.fillCircle(pupil_cx, pupil_cy, pupil_r, SSD1306_BLACK);
  }

  // 3. & 4. Upper + lower eyelids — pose carries the final, clamped lid
  //    amounts (composePose has already added blink + emotion droop +
  //    curiosity retract). They hug the VISIBLE sclera bbox, so a stretched
  //    sclera also gets a stretched lid-travel range automatically.
  //
  //    Curvature falls out of the unified perspective rule
  //      curvature = (centerline_y - eye_cy) / ry
  //    so a sleepy droop alone (no blink) automatically gets a soft ∩
  //    shape, and combined with a partial blink the curvature smoothly
  //    slides toward ∪ as the lid passes the eye center — no special cases.
  // bbox_w/h are 2*r + 1 (NOT 2*r) because fillCircle / fillEllipse paint an
  // inclusive [-r, +r] range = 2*r + 1 pixels. With bbox = 2*r, the lid loop
  // would miss the rightmost column and the bottommost row at full close
  // (parabolic bow = 0 at the lateral edges) — leaving a 1-px white sliver
  // of sclera visible during blink.
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
    drawCurvedLid(d, bbox_x, bbox_y, bbox_w, bbox_h,
                  upper_y, (upper_y - lid_cy) / lid_b, /*fill_from_top=*/true);
  }
  if (lower_off > 0.0f) {
    const float lower_y = (float)bbox_y + (float)bbox_h - lower_off;
    drawCurvedLid(d, bbox_x, bbox_y, bbox_w, bbox_h,
                  lower_y, (lower_y - lid_cy) / lid_b, /*fill_from_top=*/false);
  }
}

void renderEyes(Adafruit_SSD1306 &d,
                const Eye &left, const Eye &right,
                const EyePose &pose) {
  d.clearDisplay();
  renderEye(d, left,  pose);
  renderEye(d, right, pose);
  d.display();
}

void renderEyeOn(Adafruit_SSD1306 &d, const Eye &eye, const EyePose &pose) {
  d.clearDisplay();
  renderEye(d, eye, pose);
  d.display();
}
