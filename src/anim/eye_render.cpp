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
// are dropped silently by fillRect's bounds checks.
void drawBitmapScaled(Adafruit_SSD1306 &d,
                      int16_t x, int16_t y,
                      const uint8_t *bmp, int16_t w, int16_t h,
                      float scale, uint16_t color) {
  if (scale == 1.0f) {
    d.drawBitmap(x, y, bmp, w, h, color);
    return;
  }
  const int16_t byteWidth = (w + 7) / 8;
  for (int16_t j = 0; j < h; ++j) {
    const int16_t y0 = y + (int16_t)lroundf(j * scale);
    const int16_t y1 = y + (int16_t)lroundf((j + 1) * scale);
    for (int16_t i = 0; i < w; ++i) {
      const uint8_t b = pgm_read_byte(bmp + j * byteWidth + (i >> 3));
      if (!(b & (0x80 >> (i & 7)))) continue;
      const int16_t x0 = x + (int16_t)lroundf(i * scale);
      const int16_t x1 = x + (int16_t)lroundf((i + 1) * scale);
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
void drawCurvedLid(Adafruit_SSD1306 &d, const Eye &eye,
                   float center_y, float curvature, bool fill_from_top) {
  // Eyelid bbox follows the SCALED sclera. center_y is already in screen
  // pixels (renderEye scales it), so curvature math just needs the scaled
  // half-height as its normalization base.
  const float w_s = (float)eye.sclera.w * eye.scale;
  const float h_s = (float)eye.sclera.h * eye.scale;
  const float a   = w_s * 0.5f;
  const float cx  = (float)eye.sclera.x + a;
  const float dip = h_s * LID_CURVE_FRACTION;

  const int16_t y_top = eye.sclera.y;
  const int16_t y_bot = eye.sclera.y + (int16_t)lroundf(h_s) - 1;
  const int16_t x_lo  = eye.sclera.x;
  const int16_t x_hi  = eye.sclera.x + (int16_t)lroundf(w_s);

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
  // All on-screen geometry is the SCALED sclera. The renderer is the only
  // place that mixes scale with the EyePose, so behavior code can stay
  // ignorant of how big any given screen happens to be.
  const float scale = eye.scale;
  const float h  = (float)eye.sclera.h * scale;
  const float b  = h * 0.5f;
  const float cy = (float)eye.sclera.y + b;

  // 1. Sclera (static white blob).
  drawBitmapScaled(d, eye.sclera.x, eye.sclera.y,
                   eye.sclera.bmp, eye.sclera.w, eye.sclera.h,
                   scale, SSD1306_WHITE);

  // 2. Pupil — drawn in BLACK so it punches a hole through the sclera.
  //    Pose carries the composed offset in NATIVE pixels; scale it so the
  //    visual amplitude of drift is the same on every screen.
  const int16_t px = eye.pupil.x + (int16_t)lroundf(pose.pupil_dx * scale);
  const int16_t py = eye.pupil.y + (int16_t)lroundf(pose.pupil_dy * scale);
  drawBitmapScaled(d, px, py, eye.pupil.bmp, eye.pupil.w, eye.pupil.h,
                   scale, SSD1306_BLACK);

  // 3. & 4. Upper + lower eyelids — pose carries the final, clamped lid
  //    amounts (composePose has already added blink + emotion droop). Per-
  //    eye height varies between left and right eyes so we scale at draw
  //    time; this is what makes the pose eye-size-agnostic.
  //
  //    Curvature falls out of the unified perspective rule
  //      curvature = (centerline_y - eye_cy) / b
  //    so a sleepy droop alone (no blink) automatically gets a soft ∩ shape,
  //    and combined with a partial blink the curvature smoothly slides
  //    toward ∪ as the lid passes the eye center — no special cases.
  const float upper_off = pose.upper_lid_amount * h;
  const float lower_off = pose.lower_lid_amount * h;

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
