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

// Static, build-time eye geometry. One Eye per physical eye.
//
// `EyeBitmap` and `PupilBitmap` are typed identically right now, but kept as
// separate types so it stays obvious which is the sclera (drawn WHITE) and
// which is the pupil (drawn BLACK to punch a hole). A future `EyeBitmap`
// might gain things the pupil doesn't need (e.g. a soft-edge mask), so the
// split costs nothing now and saves a refactor later.
struct EyeBitmap {
  const uint8_t *bmp;
  int16_t        w;   // SOURCE bitmap dims (not scaled)
  int16_t        h;
  int16_t        x;   // dest top-left of the SCALED bitmap, in screen coords
  int16_t        y;
};

struct PupilBitmap {
  const uint8_t *bmp;
  int16_t        w;   // SOURCE bitmap dims (not scaled)
  int16_t        h;
  int16_t        x;   // dest top-left of the SCALED bitmap, at neutral gaze
  int16_t        y;
};

// Identifies which physical eye this Eye instance represents. The renderer
// uses it to pick the matching per-eye extras from EyePose (see idle_gaze).
// Eyes that don't care about asymmetry (e.g. a future single-screen render
// of just one eye) can pass Left and stay correct — both extras zero out
// when nobody writes them.
enum class EyeSide : uint8_t { Left = 0, Right = 1 };

// `scale` lets the same artwork drive screens of different visual sizes:
// the physical fur-cutout build wants a big eye (overflowing 128x64) while
// the wokwi preview wants both native eyes on one screen. The renderer
// scales the bitmap draw + eyelid bbox + pupil drift by this factor, so the
// motion looks visually proportional on every screen. Non-integer values
// are fine; clipping past the screen edges is fine (Adafruit_GFX skips them).
struct Eye {
  EyeBitmap   sclera;
  PupilBitmap pupil;
  float       scale;  // 1.0 = native art, 2.0 = doubled, etc.
  EyeSide     side;   // selects per-eye pose extras at render time
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
