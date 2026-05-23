#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "anim/eye_anim.h"

#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define SCREEN_ADDRESS 0x3C

// Two physical OLEDs on two separate I2C buses. SSD1306 modules are hard-
// wired to address 0x3C, so they can't share a bus — Wire drives the left
// eye, Wire1 drives the right eye. Both buses run at default 100 kHz.
// A third OLED (wokwi-only preview) sits on Wire at 0x3D — same bus as the
// left eye but a different address so the two coexist. On real hardware
// the preview module is simply absent and we skip it.
#define LEFT_SDA_PIN   2
#define LEFT_SCL_PIN   1
#define RIGHT_SDA_PIN  6
#define RIGHT_SCL_PIN  5
#define SCREEN_ADDRESS_PREVIEW 0x3D

// ~60 fps — matches the SSD1306's ~62 Hz internal refresh. Lower if you
// want to free CPU for sensors/wifi later.
constexpr uint16_t FRAME_INTERVAL_MS = 16;

// TEST: pin sleepy_amount to 1.0 every frame to see the maximum droop.
// Set to false to restore the autonomous mood drift.
constexpr bool TEST_PIN_MAX_SLEEPY = false;

// TEST: cancel micro motion's contribution to the pose so only idle gaze
// drives the pupil position — useful for tuning the gaze behavior in
// isolation. Sleepy y-bias and idle gaze (per-eye extras) still apply.
// Set to false to restore the always-on micro drift.
constexpr bool TEST_DISABLE_MICRO_MOTION = false;

// Physical eye scale. The fur-cutout build has eye holes larger than the
// OLED active area, so we render the eye big enough to overflow the 128×64
// screen — the visible portion fills the cutout. Multiplies the ANIMATED
// drift only; the sclera/pupil radii below are absolute pixels.
constexpr float PHYS_SCALE = 2.0f;

// Physical sclera / pupil radii.
//   PHYS_SCLERA_R = 72 → diameter 144, overflows the 128-wide screen by 8 px
//   per side. The fur cutout absorbs the overflow horizontally as well as
//   vertically, so the visible eye reads as a slice of an even larger ball.
//   PHYS_PUPIL_R  = 16 → pupil_diameter / sclera_diameter = 1/4.5 (smaller,
//   more focused pupil than the source artwork's 1/3 — matches the Ghibli
//   reference better).
constexpr int16_t PHYS_SCLERA_R = 64;
constexpr int16_t PHYS_PUPIL_R  = 16;

Adafruit_SSD1306 displayL(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire,  OLED_RESET);
Adafruit_SSD1306 displayR(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire1, OLED_RESET);
Adafruit_SSD1306 displayP(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire,  OLED_RESET);
bool has_preview = false;

// --- Per-physical-eye configs: sclera overflows the screen on all sides ---
// Centered on the screen → the circle overflows by (PHYS_SCLERA_R - 64) px
// horizontally and (PHYS_SCLERA_R - 32) px vertically (the fur cutout
// absorbs the overflow). Pupil neutral offset preserves the slight inward
// bias from the original artwork (±8 source px × PHYS_SCALE = ±16), which
// is part of the susuwatari look.
const Eye kBigLeftEye = {
  /*sclera_cx=*/SCREEN_WIDTH  / 2,
  /*sclera_cy=*/SCREEN_HEIGHT / 2,
  /*sclera_r =*/PHYS_SCLERA_R,
  /*pupil_dx_neutral=*/(int16_t)( 16 * PHYS_SCALE),
  /*pupil_dy_neutral=*/(int16_t)(-1),
  /*pupil_r        =*/PHYS_PUPIL_R,
  PHYS_SCALE,
  EyeSide::Left,
};

const Eye kBigRightEye = {
  /*sclera_cx=*/SCREEN_WIDTH  / 2,
  /*sclera_cy=*/SCREEN_HEIGHT / 2,
  /*sclera_r =*/PHYS_SCLERA_R,
  /*pupil_dx_neutral=*/(int16_t)(-16 * PHYS_SCALE),
  /*pupil_dy_neutral=*/0,
  /*pupil_r        =*/PHYS_PUPIL_R,
  PHYS_SCALE,
  EyeSide::Right,
};

// --- Preview config: both eyes side-by-side on one 128×64 screen ---
// Native sizes preserved (sclera_r = 18, pupil_r = 6) so the wokwi preview
// shows the eyes at their authored proportions. Both eyes have the inward
// pupil bias (left +8, right -8) just like the physical config — same look,
// smaller scale.
const Eye kPreviewLeft = {
  /*sclera_cx=*/43,
  /*sclera_cy=*/31,
  /*sclera_r =*/18,
  /*pupil_dx_neutral=*/ 8,
  /*pupil_dy_neutral=*/ 0,
  /*pupil_r        =*/ 6,
  1.0f,
  EyeSide::Left,
};

const Eye kPreviewRight = {
  /*sclera_cx=*/87,
  /*sclera_cy=*/31,
  /*sclera_r =*/18,
  /*pupil_dx_neutral=*/-8,
  /*pupil_dy_neutral=*/ 0,
  /*pupil_r        =*/ 6,
  1.0f,
  EyeSide::Right,
};

EyeState eyes;
uint32_t next_frame_ms = 0;

void setup() {
  Serial.begin(115200);

  // Two independent I2C buses, one per display.
  Wire.begin (LEFT_SDA_PIN,  LEFT_SCL_PIN);
  Wire1.begin(RIGHT_SDA_PIN, RIGHT_SCL_PIN);

  // Default Wire clock is 100 kHz — at ~92 ms per 1 KB SSD1306 frame that
  // drops us to ~3–4 fps with three displays and makes the L/R update gap
  // visible. 1 MHz is well within SSD1306 module tolerance and brings us
  // back to the targeted ~50 fps. Drop to 400 000 if you ever see corruption
  // on long wires / cheap modules on real hardware.
  Wire.setClock (1000000);
  Wire1.setClock(1000000);

  if (!displayL.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("Left SSD1306 allocation failed");
    for (;;);
  }
  if (!displayR.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("Right SSD1306 allocation failed");
    for (;;);
  }

  // Preview screen is wokwi-only — on real hardware it's absent, so we
  // don't bomb; we just skip rendering to it.
  has_preview = displayP.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS_PREVIEW);
  if (!has_preview) {
    Serial.println("Preview SSD1306 not detected (wokwi-only); skipping");
  }

  eyeStateInit(eyes, millis());
  next_frame_ms = millis();
}

void loop() {
  uint32_t now = millis();
  if ((int32_t)(now - next_frame_ms) < 0) return;
  next_frame_ms = now + FRAME_INTERVAL_MS;

  // Pinning every frame so the autonomous re-roll inside sleepyUpdate()
  // can't overwrite the test value 8–25 s in.
  if (TEST_PIN_MAX_SLEEPY) {
    eyeStateSetSleepy(eyes, 1.0f);
  }

  // Future gaze-tracking integration would call:
  //   eyeSetGazeTarget(eyes, face_dx_px, face_dy_px, /*weight=*/1.0f);
  // every frame from a face detector. Micro motion smooths the signal, so
  // even noisy detections turn into organic eye follow.

  eyeStateUpdate(eyes, now);

  // Cancel micro motion's contribution AFTER composePose has run, so the
  // micro_motion state itself still evolves (cheap, and lets us flip the
  // flag back to false without any other side effects). Idle gaze writes
  // its contribution into the per-eye `*_extra` fields, which we leave
  // untouched — those are what the renderer adds at draw time.
  if (TEST_DISABLE_MICRO_MOTION) {
    eyes.pose.pupil_dx -= eyes.micro.drift_x;
    eyes.pose.pupil_dy -= eyes.micro.drift_y;
  }

  // Both eyes share one EyePose (binocular pair: shared drift, shared
  // blink), but they render in two different geometries:
  //   - displayL/displayR: big, screen-filling eyes — match the fur-cutout build.
  //   - displayP        : both eyes at native size on one screen, for the
  //                       wokwi-only "together" preview.
  renderEyeOn(displayL, kBigLeftEye,  eyes);
  renderEyeOn(displayR, kBigRightEye, eyes);
  if (has_preview) {
    renderEyes(displayP, kPreviewLeft, kPreviewRight, eyes);
  }
}
