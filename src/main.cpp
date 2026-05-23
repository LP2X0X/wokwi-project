#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "assets/eyes_bitmaps.h"
#include "anim/eye_anim.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

#define SDA_PIN 2
#define SCL_PIN 1

// ~50 fps. The SSD1306 over I2C tops out around there; lower it if you want
// to free CPU for sensors/wifi later.
constexpr uint16_t FRAME_INTERVAL_MS = 20;

// TEST: pin sleepy_amount to 1.0 every frame to see the maximum droop.
// Set to false to restore the autonomous mood drift.
constexpr bool TEST_PIN_MAX_SLEEPY = false;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Static eye geometry built from the auto-generated bitmaps. The animation
// system never sees the bitmaps directly — it only mutates EyeState.
// Eyelid is procedural (a per-column curved arc derived from sclera geometry)
// so the Eye descriptor only carries the two physical bitmaps.
const Eye kLeftEye = {
  { eye_left_white_bmp,  eye_left_white_w,  eye_left_white_h,
    eye_left_white_x,    eye_left_white_y },
  { eye_left_pupil_bmp,  eye_left_pupil_w,  eye_left_pupil_h,
    eye_left_pupil_x,    eye_left_pupil_y },
};

const Eye kRightEye = {
  { eye_right_white_bmp, eye_right_white_w, eye_right_white_h,
    eye_right_white_x,   eye_right_white_y },
  { eye_right_pupil_bmp, eye_right_pupil_w, eye_right_pupil_h,
    eye_right_pupil_x,   eye_right_pupil_y },
};

EyeState eyes;
uint32_t next_frame_ms = 0;

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("SSD1306 allocation failed");
    for (;;);
  }

  eyeStateInit(eyes, millis());
  next_frame_ms = millis();
}

void loop() {
  uint32_t now = millis();
  if ((int32_t)(now - next_frame_ms) < 0) return;
  next_frame_ms = now + FRAME_INTERVAL_MS;

  // Pinning every frame so the autonomous re-roll inside updateSleepyState()
  // can't overwrite the test value 8–25 s in.
  if (TEST_PIN_MAX_SLEEPY) {
    eyeStateSetSleepy(eyes, 1.0f);
  }

  eyeStateUpdate(eyes, now);
  renderEyes(display, kLeftEye, kRightEye, eyes);
}
