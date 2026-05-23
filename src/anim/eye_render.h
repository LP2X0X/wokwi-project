// Eye renderer. PURE function of (Eye geometry, EyePose) -> pixels.
//
// Importantly the renderer does NOT include any behavior header. It cannot
// see drift_x, lid_close, sleepy_amount, etc. — only the composed pose.
// This is what lets you drop in completely new behaviors (face tracking,
// surprise, wink) without touching this file, and what lets you replace the
// renderer (e.g. for an LCD with grayscale, or a host-side simulator)
// without touching any behavior code.

#pragma once

#include <Adafruit_SSD1306.h>

#include "eye_pose.h"

// Identifies which physical eye this Eye instance represents. The renderer
// uses it to pick the matching per-eye extras from EyePose (see idle_gaze).
// Eyes that don't care about asymmetry can pass Left and stay correct —
// both extras zero out when nobody writes them.
enum class EyeSide : uint8_t { Left = 0, Right = 1 };

// Static, build-time eye geometry. Both sclera and pupil are procedural
// circles — no bitmap assets. The sclera circle is allowed to overflow the
// display (the fur cutout on the physical build absorbs the overflow); the
// renderer relies on Adafruit_GFX clipping for off-screen pixels.
//
// Geometry fields (cx/cy/r, neutral pupil offset) are ABSOLUTE pixels — they
// already encode the per-display visual size. `scale` is a separate knob
// that multiplies the ANIMATED pupil drift coming in via EyePose, so motion
// behaviors authored in "source pixel" units (±1.5 px etc.) read as visually
// proportional regardless of which display this Eye is bound to.
struct Eye {
  // Sclera (white area) as a circle in screen coords. Width of the visible
  // area is `2 * sclera_r`. Curiosity stretches the circle into an ellipse
  // VERTICALLY at draw time (pose.eye_open_amount); width never changes.
  int16_t sclera_cx;
  int16_t sclera_cy;
  int16_t sclera_r;

  // Pupil neutral offset from the sclera center, in screen pixels. This is
  // the "where the iris sits in the artwork" bias — e.g. a slight inward
  // tilt for the susuwatari look. Animated drift (pose.pupil_dx/dy + per-eye
  // extras) adds on top of this, multiplied by `scale`.
  int16_t pupil_dx_neutral;
  int16_t pupil_dy_neutral;
  int16_t pupil_r;

  // Per-display zoom for ANIMATION amplitudes only. 1.0 = source units
  // unchanged, 2.0 = doubled drift range. Geometry radii above are absolute
  // and are NOT multiplied by this.
  float   scale;
  EyeSide side;
};

// Render one eye's layers into the current framebuffer. Does NOT clear or
// push the display — useful when you want to compose more elements on
// screen alongside the eyes.
void renderEye(Adafruit_SSD1306 &d, const Eye &eye, const EyePose &pose);

// Render a binocular pair to the same pose. Clears + pushes for you.
void renderEyes(Adafruit_SSD1306 &d,
                const Eye &left, const Eye &right,
                const EyePose &pose);

// Render a single eye on its own display: clears + draws + pushes. Use this
// when each eye lives on a separate physical OLED (e.g. two SSD1306 modules
// driven by Wire and Wire1, one per screen). Loop becomes:
//   renderEyeOn(displayL, kLeftEye,  pose);
//   renderEyeOn(displayR, kRightEye, pose);
void renderEyeOn(Adafruit_SSD1306 &d, const Eye &eye, const EyePose &pose);
