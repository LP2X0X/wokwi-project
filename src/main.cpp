#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "assets/eyes_bitmaps.h"
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

// ~50 fps. The SSD1306 over I2C tops out around there; lower it if you want
// to free CPU for sensors/wifi later.
constexpr uint16_t FRAME_INTERVAL_MS = 20;

// TEST: pin sleepy_amount to 1.0 every frame to see the maximum droop.
// Set to false to restore the autonomous mood drift.
constexpr bool TEST_PIN_MAX_SLEEPY = false;

// Physical eye scale. The fur-cutout build has eye holes larger than the
// OLED active area, so we render the eye big enough to overflow the 128×64
// screen — the visible portion fills the cutout. Tune to taste; 1.0 = native
// 36×44 sclera (fits comfortably), 2.0 = 72×88 (clips top/bottom by ~12 px
// each), >2 = more clipping.
constexpr float PHYS_SCALE = 2.0f;

Adafruit_SSD1306 displayL(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire,  OLED_RESET);
Adafruit_SSD1306 displayR(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire1, OLED_RESET);
Adafruit_SSD1306 displayP(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire,  OLED_RESET);
bool has_preview = false;

// Helper: dest top-left of a scaled bitmap that should sit at a specific
// pupil-offset within the scaled sclera, with the sclera itself centered on
// a 128×64 screen. Keeping these inline keeps the per-eye Eye literal short
// while preserving the original artwork's slight inward pupil bias (which is
// part of the susuwatari look).
constexpr int16_t centeredScleraX(int16_t src_w, float scale) {
  return (int16_t)((SCREEN_WIDTH  - (int16_t)(src_w * scale)) / 2);
}
constexpr int16_t centeredScleraY(int16_t src_h, float scale) {
  return (int16_t)((SCREEN_HEIGHT - (int16_t)(src_h * scale)) / 2);
}

// --- Per-physical-eye configs: big, overflowing 128×64 ---
// Each eye centered on its own screen. Pupil keeps its source-relative
// offset within the sclera, just scaled.
constexpr int16_t BIG_L_SX = centeredScleraX(eye_left_white_w,  PHYS_SCALE);
constexpr int16_t BIG_L_SY = centeredScleraY(eye_left_white_h,  PHYS_SCALE);
constexpr int16_t BIG_R_SX = centeredScleraX(eye_right_white_w, PHYS_SCALE);
constexpr int16_t BIG_R_SY = centeredScleraY(eye_right_white_h, PHYS_SCALE);

const Eye kBigLeftEye = {
  { eye_left_white_bmp, eye_left_white_w, eye_left_white_h, BIG_L_SX, BIG_L_SY },
  { eye_left_pupil_bmp, eye_left_pupil_w, eye_left_pupil_h,
    (int16_t)(BIG_L_SX + (int16_t)((eye_left_pupil_x - eye_left_white_x) * PHYS_SCALE)),
    (int16_t)(BIG_L_SY + (int16_t)((eye_left_pupil_y - eye_left_white_y) * PHYS_SCALE)) },
  PHYS_SCALE,
};

const Eye kBigRightEye = {
  { eye_right_white_bmp, eye_right_white_w, eye_right_white_h, BIG_R_SX, BIG_R_SY },
  { eye_right_pupil_bmp, eye_right_pupil_w, eye_right_pupil_h,
    (int16_t)(BIG_R_SX + (int16_t)((eye_right_pupil_x - eye_right_white_x) * PHYS_SCALE)),
    (int16_t)(BIG_R_SY + (int16_t)((eye_right_pupil_y - eye_right_white_y) * PHYS_SCALE)) },
  PHYS_SCALE,
};

// --- Preview config: both eyes at native size on one 128×64 screen ---
// Uses the artwork's original positions — exactly what the bitmaps were
// authored for. This is the "how do they look together" view in wokwi.
const Eye kPreviewLeft = {
  { eye_left_white_bmp, eye_left_white_w, eye_left_white_h,
    eye_left_white_x, eye_left_white_y },
  { eye_left_pupil_bmp, eye_left_pupil_w, eye_left_pupil_h,
    eye_left_pupil_x, eye_left_pupil_y },
  1.0f,
};

const Eye kPreviewRight = {
  { eye_right_white_bmp, eye_right_white_w, eye_right_white_h,
    eye_right_white_x, eye_right_white_y },
  { eye_right_pupil_bmp, eye_right_pupil_w, eye_right_pupil_h,
    eye_right_pupil_x, eye_right_pupil_y },
  1.0f,
};

EyeState eyes;
uint32_t next_frame_ms = 0;

void setup() {
  Serial.begin(115200);

  // Two independent I2C buses, one per display.
  Wire.begin (LEFT_SDA_PIN,  LEFT_SCL_PIN);
  Wire1.begin(RIGHT_SDA_PIN, RIGHT_SCL_PIN);

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
