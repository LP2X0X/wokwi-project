#include "eye_render.h"

#include <math.h>

namespace {

// Eyelid bow at the most extreme positions (lid centerline at the very top
// or bottom of the eye), expressed as a fraction of sclera height. At the
// eye center the bow is 0 (flat line). Larger values = more pronounced
// spherical perspective. Shared by upper and lower lids; rendering concern
// (it controls how the curve LOOKS, not when/where the lid moves).
constexpr float LID_CURVE_FRACTION = 0.22f;

// Adafruit_GFX has no scaled-bitmap primitive, so we roll our own: for each
// "on" source pixel, paint a rectangle covering its scaled footprint. Edges
// are computed as differences of lroundf to avoid 1-px gaps between cells
// for non-integer scales. Off pixels are skipped (cheap) and clipped pixels
// are dropped silently by fillRect's bounds checks. Per-axis scale lets the
// sclera stretch vertically (curiosity) without also growing horizontally.
void drawBitmapScaled(Adafruit_SSD1306 &d,
                      int16_t x, int16_t y,
                      const uint8_t *bmp, int16_t w, int16_t h,
                      float scale_x, float scale_y, uint16_t color) {
  if (scale_x == 1.0f && scale_y == 1.0f) {
    d.drawBitmap(x, y, bmp, w, h, color);
    return;
  }
  const int16_t byteWidth = (w + 7) / 8;
  for (int16_t j = 0; j < h; ++j) {
    const int16_t y0 = y + (int16_t)lroundf(j * scale_y);
    const int16_t y1 = y + (int16_t)lroundf((j + 1) * scale_y);
    for (int16_t i = 0; i < w; ++i) {
      const uint8_t b = pgm_read_byte(bmp + j * byteWidth + (i >> 3));
      if (!(b & (0x80 >> (i & 7)))) continue;
      const int16_t x0 = x + (int16_t)lroundf(i * scale_x);
      const int16_t x1 = x + (int16_t)lroundf((i + 1) * scale_x);
      d.fillRect(x0, y0, x1 - x0, y1 - y0, color);
    }
  }
}

// Draws a single procedural eyelid as a curved arc clipped to the eye bbox.
//
//   center_y       Lid centerline y at the column x = eye_center_x. The bow
//                  per column is added to this.
//   curvature      Sign + magnitude of the bow. Convention matches the
//                  spherical-perspective rule:
//                    curvature < 0  -> ∩ (lid above eye center)
//                    curvature == 0 -> flat
//                    curvature > 0  -> ∪ (lid below eye center)
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
void drawCurvedLid(Adafruit_SSD1306 &d,
                   int16_t sclera_x, int16_t sclera_y,
                   int16_t sclera_w_src, int16_t sclera_h_src,
                   float sclera_scale_x, float sclera_scale_y,
                   float center_y, float curvature, bool fill_from_top) {
  // Eyelid bbox follows the VISIBLE sclera box (post eye_open_amount). The
  // caller passes the already-shifted top-left + per-axis scales, so this
  // function is fully decoupled from Eye::scale / pose.eye_open_amount.
  const float w_s = (float)sclera_w_src * sclera_scale_x;
  const float h_s = (float)sclera_h_src * sclera_scale_y;
  const float a   = w_s * 0.5f;
  const float cx  = (float)sclera_x + a;
  const float dip = h_s * LID_CURVE_FRACTION;

  const int16_t y_top = sclera_y;
  const int16_t y_bot = sclera_y + (int16_t)lroundf(h_s) - 1;
  const int16_t x_lo  = sclera_x;
  const int16_t x_hi  = sclera_x + (int16_t)lroundf(w_s);

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
  // Three scale knobs sit on top of Eye::scale:
  //   * pose.eye_open_amount → multiplies the SCLERA HEIGHT only. 1.0 =
  //     baseline, ~2.0 = curiosity peak (tall stretched eye like a
  //     Ghibli soot creature noticing something). Width is untouched so
  //     the eye gets *taller*, not bigger overall.
  //   * pose.pupil_scale     → multiplies the PUPIL scale uniformly. 1.0 =
  //     baseline, <1.0 = smaller / more focused pupil.
  //   * eye.scale            → static per-display zoom (PHYS_SCALE on the
  //     physical OLEDs, 1.0 on the wokwi preview).
  //
  // All scales are applied so the bitmap stays CENTERED on its baseline
  // anchor point. The pupil's drift offset uses the BASE scale so gaze
  // motion amplitude doesn't grow when the eye opens wider.
  const float base_scale     = eye.scale;
  const float open_amt       = pose.eye_open_amount > 0.0f ? pose.eye_open_amount : 1.0f;
  const float pupil_amt      = pose.pupil_scale     > 0.0f ? pose.pupil_scale     : 1.0f;
  const float sclera_scale_x = base_scale;                  // width unchanged
  const float sclera_scale_y = base_scale * open_amt;       // height stretches
  const float pupil_scale    = base_scale * pupil_amt;

  // Sclera bbox (height stretched, width preserved), centered on baseline.
  const float base_sw = (float)eye.sclera.w * base_scale;
  const float base_sh = (float)eye.sclera.h * base_scale;
  const float new_sw  = (float)eye.sclera.w * sclera_scale_x;
  const float new_sh  = (float)eye.sclera.h * sclera_scale_y;
  const int16_t sx = eye.sclera.x - (int16_t)lroundf((new_sw - base_sw) * 0.5f);
  const int16_t sy = eye.sclera.y - (int16_t)lroundf((new_sh - base_sh) * 0.5f);

  // 1. Sclera (static white blob — stretched vertically by eye_open_amount).
  drawBitmapScaled(d, sx, sy, eye.sclera.bmp, eye.sclera.w, eye.sclera.h,
                   sclera_scale_x, sclera_scale_y, SSD1306_WHITE);

  // 2. Pupil — drawn in BLACK so it punches a hole through the sclera.
  //    Uniform scale (eye_open_amount does NOT stretch the pupil — it'd
  //    look weird if the iris was a tall oval too). Centered on baseline
  //    anchor so a smaller pupil stays where the artwork intended.
  const float base_pw = (float)eye.pupil.w * base_scale;
  const float base_ph = (float)eye.pupil.h * base_scale;
  const float new_pw  = (float)eye.pupil.w * pupil_scale;
  const float new_ph  = (float)eye.pupil.h * pupil_scale;
  const int16_t pup_cx_off = -(int16_t)lroundf((new_pw - base_pw) * 0.5f);
  const int16_t pup_cy_off = -(int16_t)lroundf((new_ph - base_ph) * 0.5f);

  const float extra_x = (eye.side == EyeSide::Left)
      ? pose.pupil_dx_l_extra : pose.pupil_dx_r_extra;
  const float extra_y = (eye.side == EyeSide::Left)
      ? pose.pupil_dy_l_extra : pose.pupil_dy_r_extra;
  const int16_t px = eye.pupil.x + pup_cx_off +
                     (int16_t)lroundf((pose.pupil_dx + extra_x) * base_scale);
  const int16_t py = eye.pupil.y + pup_cy_off +
                     (int16_t)lroundf((pose.pupil_dy + extra_y) * base_scale);
  drawBitmapScaled(d, px, py, eye.pupil.bmp, eye.pupil.w, eye.pupil.h,
                   pupil_scale, pupil_scale, SSD1306_BLACK);

  // 3. & 4. Upper + lower eyelids — pose carries the final, clamped lid
  //    amounts (composePose has already added blink + emotion droop +
  //    curiosity retract). They hug the VISIBLE sclera box, so a stretched
  //    sclera also gets a stretched lid-travel range automatically.
  //
  //    Curvature falls out of the unified perspective rule
  //      curvature = (centerline_y - eye_cy) / b
  //    so a sleepy droop alone (no blink) automatically gets a soft ∩ shape,
  //    and combined with a partial blink the curvature smoothly slides
  //    toward ∪ as the lid passes the eye center — no special cases.
  const float lid_b  = new_sh * 0.5f;
  const float lid_cy = (float)sy + lid_b;
  const float upper_off = pose.upper_lid_amount * new_sh;
  const float lower_off = pose.lower_lid_amount * new_sh;

  if (upper_off > 0.0f) {
    const float upper_y = (float)sy + upper_off;
    drawCurvedLid(d, sx, sy, eye.sclera.w, eye.sclera.h,
                  sclera_scale_x, sclera_scale_y,
                  upper_y, (upper_y - lid_cy) / lid_b, /*fill_from_top=*/true);
  }
  if (lower_off > 0.0f) {
    const float lower_y = (float)sy + new_sh - lower_off;
    drawCurvedLid(d, sx, sy, eye.sclera.w, eye.sclera.h,
                  sclera_scale_x, sclera_scale_y,
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
