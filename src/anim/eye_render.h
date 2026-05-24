// Eye renderer. PURE function of (Eye geometry, EyePose) -> sprite pixels.
//
// The renderer draws into a TFT_eSprite (off-screen 16-bit RGB565 buffer);
// pushing the sprite to a physical display lives in the caller. This split
// is what lets us drive multiple ST7789s on a shared SPI bus from ONE
// sprite — render once, push to each display via per-display CS toggling.
//
// The renderer does NOT include any behavior header. It cannot see
// drift_x, lid_close, sleepy_amount, etc. — only the composed pose. So
// behaviors can change without touching this file, and the renderer can
// be swapped (e.g. for a future GC9A01 driver — same TFT_eSPI API; just
// changes a build flag).

#pragma once

#include <TFT_eSPI.h>

#include "eye_pose.h"

// Identifies which physical eye this Eye instance represents. The renderer
// uses it to pick the matching per-eye extras from EyePose (see idle_gaze).
enum class EyeSide : uint8_t { Left = 0, Right = 1 };

// Static, build-time eye geometry. Both sclera and pupil are procedural
// circles — no bitmap assets. The sclera circle is allowed to overflow the
// display (the fur cutout on the physical build absorbs the overflow).
//
// Geometry fields (cx/cy/r, neutral pupil offset) are ABSOLUTE pixels.
// `scale` is a separate knob that multiplies the ANIMATED pupil drift
// coming in via EyePose so behaviors authored in "source pixel" units
// (±1.5 px etc.) read as visually proportional on displays of different
// sizes (240×240 ST7789 today, 240×240 GC9A01 round IPS later).
struct Eye {
  int16_t sclera_cx;
  int16_t sclera_cy;
  int16_t sclera_r;

  int16_t pupil_dx_neutral;
  int16_t pupil_dy_neutral;
  int16_t pupil_r;

  float   scale;
  EyeSide side;
};

// Axis-aligned region inside the sprite buffer, used for cropped push.
struct SpriteRect {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

// Draw one eye's layers INTO the sprite. Does NOT clear or push — used as
// a building block when you want to compose multiple eyes (preview screen)
// or draw extra overlays alongside the eye.
void renderEye(TFT_eSprite &spr, const Eye &eye, const EyePose &pose);

// Clear the sprite and draw one eye. Used when each eye lives on its own
// physical OLED — render, then the caller asserts that display's CS and
// calls pushSprite().
void renderEyeOn(TFT_eSprite &spr, const Eye &eye, const EyePose &pose);

// Clear the sprite and draw a binocular pair from the same pose. Used for
// the preview screen that shows both eyes together at native size.
void renderEyes(TFT_eSprite &spr,
                const Eye &left, const Eye &right,
                const EyePose &pose);

// Bounding box of one eye's drawn pixels (sclera + pupil), clipped to the
// sprite. Mirrors the geometry math in renderEye().
SpriteRect eyeBounds(const Eye &eye, const EyePose &pose,
                     int16_t clip_w, int16_t clip_h);

// Minimal axis-aligned union of two rects. Empty rects (w/h <= 0) pass
// through the other operand unchanged.
SpriteRect boundsUnion(const SpriteRect &a, const SpriteRect &b);

// Union of both preview-eye bounds — the region to push for the binocular
// preview screen instead of the full 240×240 sprite.
SpriteRect previewPushBounds(const Eye &left, const Eye &right,
                             const EyePose &pose,
                             int16_t clip_w, int16_t clip_h);
