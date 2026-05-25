#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>

#include "anim/eye_anim.h"
#include "anim/eye_render.h"

// Sprite framebuffer dimensions — fixed 240×240 regardless of panel.
// On ST7789/GC9A01 (240×240) this fills the panel exactly. On ILI9341
// (240×320, Wokwi default) the sprite sits centered, with the top/
// bottom 40 px of the panel staying at the initial-fill BG color.
// Keeping the sprite 240² shaves ~25% of SPI bandwidth on the ILI9341
// build, which translates directly to higher framerate and tighter
// display-to-display sync.
#define SCREEN_WIDTH   240
#define SCREEN_HEIGHT  240

// Where the 240×240 sprite lands on the actual TFT panel. Centered, so
// any panel of width >= 240 works.
constexpr int16_t SPRITE_X = (TFT_WIDTH  - SCREEN_WIDTH ) / 2;
constexpr int16_t SPRITE_Y = (TFT_HEIGHT - SCREEN_HEIGHT) / 2;

// Per-display chip-select pins. Three ST7789 modules share ONE SPI bus
// (MOSI/SCLK/DC/RST/BL configured in platformio.ini build flags); each
// has its own CS so we pick which one receives a given pushSprite() by
// asserting only that pin LOW. TFT_eSPI's library-managed CS is disabled
// (TFT_CS=-1 in build flags) so we own the timing here.
//
// Future GC9A01 round-IPS migration: same pins, same wiring; only the
// build flag changes (ST7789_DRIVER -> GC9A01_DRIVER) and the Wokwi part.
#define CS_LEFT     10
#define CS_RIGHT     9
#define CS_PREVIEW   8

// ~60 fps target. TFT pushes at 40 MHz SPI take ~23 ms for a full 240×240
// frame, so three displays per frame is ~70 ms / ~14 fps without DMA. The
// loop is paced at the FRAME_INTERVAL_MS target, so if pushes take
// longer, frames just stretch — easing is dt-based and stays smooth.
// Bump SPI_FREQUENCY to 80 MHz in platformio.ini for ~30 fps on real
// hardware with good wiring.
constexpr uint16_t FRAME_INTERVAL_MS = 16;

// TEST: pin sleepy_amount to 1.0 every frame to see the maximum droop.
constexpr bool TEST_PIN_MAX_SLEEPY = false;

// TEST: cancel micro motion's contribution to the pose so only idle gaze
// drives the pupil position — useful for tuning the gaze behavior in
// isolation.
constexpr bool TEST_DISABLE_MICRO_MOTION = false;

// TEST: bypass the sprite / animation pipeline entirely and just cycle
// solid colors (RED → GREEN → BLUE → WHITE) directly via tft.fillScreen.
// Use this to confirm panel + wiring + init independent of the renderer.
constexpr bool TEST_DIRECT_FILL = false;

// Per-display enables. Each render + push spends real time per frame
// (sprite re-render + SPI bus time) even when no panel is wired to that
// CS, so disabling a display in diagram.json without flipping these to
// false leaves the work in the loop. Set to false for any panel you're
// not actually using in the current test config.
// Defaults auto-select per build env so we don't have to flip flags
// when switching between Wokwi and real hardware:
//   * GC9A01 hardware build (esp32s3_gc9a01) → single LEFT panel only.
//   * Wokwi sim build (esp32s3, ILI9341 driver) → all three panels
//     (left eye, right eye, both-eye preview) so we see the full layout.
#if defined(GC9A01_DRIVER)
constexpr bool ENABLE_LEFT_DISPLAY    = true;
constexpr bool ENABLE_RIGHT_DISPLAY   = false;
constexpr bool ENABLE_PREVIEW_DISPLAY = false;
#else
constexpr bool ENABLE_LEFT_DISPLAY    = true;
constexpr bool ENABLE_RIGHT_DISPLAY   = true;
constexpr bool ENABLE_PREVIEW_DISPLAY = true;
#endif

// Preview-only: push the union of both eye bboxes instead of the full
// 240×240 sprite (~37% less SPI data on preview). Physical eyes clip to
// the full sprite anyway, so left/right keep full pushSprite().
constexpr bool ENABLE_PREVIEW_DIRTY_PUSH = true;

// Per-frame heartbeat (rich state dump every 250 ms). USEFUL FOR DEBUG,
// EXPENSIVE: Serial output on Wokwi's simulated UART can drain slowly
// and dominate per-loop time. Off by default; flip true to inspect
// drift/lid/gaze values when something looks wrong.
constexpr bool ENABLE_HEARTBEAT       = false;

// Cheap FPS counter — ONE printf per second, regardless of frame rate.
// Always on so we can confirm the loop is healthy and see whether
// other changes (sprite size, SPI speed, display count) actually help.
constexpr bool ENABLE_FPS_COUNTER     = true;

// Physical sclera / pupil radii on the 240×240 panel.
//   PHYS_SCLERA_R = 135 → diameter 270, overflows the 240-wide screen by
//   15 px per side (fur cutout absorbs the rest). Same intent as the
//   previous 128-OLED setup.
//   PHYS_PUPIL_R  = 30  → keeps the ~1/4.5 pupil-to-sclera diameter
//   ratio that matches the Ghibli reference.
constexpr int16_t PHYS_SCLERA_R = 135;
constexpr int16_t PHYS_PUPIL_R  = 30;

// Animation amplitude multiplier — drift values from behaviors are in
// "source pixel" units (e.g. ±1.5 px); PHYS_SCALE = 4.0 maps that to
// ±6 px on the bigger 240 panel, preserving the perceived motion ratio
// from the 128 OLED build (which used PHYS_SCALE = 2.0).
constexpr float PHYS_SCALE = 4.0f;

TFT_eSPI    tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);

bool has_preview = true;  // set false if you build a 2-display variant.

// --- Per-physical-eye configs: sclera overflows the screen on all sides ---
const Eye kBigLeftEye = {
  /*sclera_cx=*/SCREEN_WIDTH  / 2,
  /*sclera_cy=*/SCREEN_HEIGHT / 2,
  /*sclera_r =*/PHYS_SCLERA_R,
  /*pupil_dx_neutral=*/(int16_t)( 8 * PHYS_SCALE),
  /*pupil_dy_neutral=*/(int16_t)(-1 * PHYS_SCALE / 2),
  /*pupil_r        =*/PHYS_PUPIL_R,
  PHYS_SCALE,
  EyeSide::Left,
};

const Eye kBigRightEye = {
  /*sclera_cx=*/SCREEN_WIDTH  / 2,
  /*sclera_cy=*/SCREEN_HEIGHT / 2,
  /*sclera_r =*/PHYS_SCLERA_R,
  /*pupil_dx_neutral=*/(int16_t)(-8 * PHYS_SCALE),
  /*pupil_dy_neutral=*/0,
  /*pupil_r        =*/PHYS_PUPIL_R,
  PHYS_SCALE,
  EyeSide::Right,
};

// --- Preview config: both eyes side-by-side on one 240×240 screen ---
// Half-size vs the previous preview (r=50) to cut render + dirty-push
// cost in Wokwi. PREVIEW_SCALE multiplies source drift so motion still
// reads proportionally at this smaller geometry.
constexpr float PREVIEW_SCALE = 0.75f;

const Eye kPreviewLeft = {
  /*sclera_cx=*/88,
  /*sclera_cy=*/120,
  /*sclera_r =*/25,
  /*pupil_dx_neutral=*/6,
  /*pupil_dy_neutral=*/ 0,
  /*pupil_r        =*/6,
  PREVIEW_SCALE,
  EyeSide::Left,
};

const Eye kPreviewRight = {
  /*sclera_cx=*/152,
  /*sclera_cy=*/120,
  /*sclera_r =*/25,
  /*pupil_dx_neutral=*/-6,
  /*pupil_dy_neutral=*/ 0,
  /*pupil_r        =*/6,
  PREVIEW_SCALE,
  EyeSide::Right,
};

EyeState eyes;
uint32_t next_frame_ms = 0;

// Assert one display's CS LOW (and all others HIGH) so the next SPI
// transaction lands on that display only. The shared MOSI/SCLK/DC/RST
// pins are seen by every panel; CS is what gates them.
inline void selectDisplay(int cs_pin) {
  digitalWrite(CS_LEFT,    cs_pin == CS_LEFT    ? LOW : HIGH);
  digitalWrite(CS_RIGHT,   cs_pin == CS_RIGHT   ? LOW : HIGH);
  digitalWrite(CS_PREVIEW, cs_pin == CS_PREVIEW ? LOW : HIGH);
}

inline void deselectAllDisplays() {
  digitalWrite(CS_LEFT,    HIGH);
  digitalWrite(CS_RIGHT,   HIGH);
  digitalWrite(CS_PREVIEW, HIGH);
}

// Push only the preview eye region to the panel. Falls back to a full
// sprite push when dirty-rect is disabled or bounds cover ≥95% of pixels.
void pushPreviewSprite(TFT_eSprite &sprite,
                       const Eye &left, const Eye &right,
                       const EyePose &pose) {
  if (!ENABLE_PREVIEW_DIRTY_PUSH) {
    sprite.pushSprite(SPRITE_X, SPRITE_Y);
    return;
  }

  const SpriteRect bounds = previewPushBounds(left, right, pose,
                                              SCREEN_WIDTH, SCREEN_HEIGHT);
  const int32_t sprite_pixels = (int32_t)SCREEN_WIDTH * SCREEN_HEIGHT;
  if (bounds.w <= 0 || bounds.h <= 0 ||
      (int32_t)bounds.w * bounds.h >= sprite_pixels * 95 / 100) {
    sprite.pushSprite(SPRITE_X, SPRITE_Y);
    return;
  }

  sprite.pushSprite(SPRITE_X + bounds.x, SPRITE_Y + bounds.y,
                    bounds.x, bounds.y, bounds.w, bounds.h);
}

void setup() {
  Serial.begin(115200);
  // USB-CDC needs a moment to enumerate on ESP32-S3 — without this any
  // setup() prints before ~100 ms get dropped, so a crash here would
  // boot-loop silently.
  delay(200);
  Serial.println("[boot] setup() entered");

  // Per-display CS pins. Start deselected so the first tft.init() below
  // doesn't accidentally hit the wrong panel.
  pinMode(CS_LEFT,    OUTPUT);
  pinMode(CS_RIGHT,   OUTPUT);
  pinMode(CS_PREVIEW, OUTPUT);
  deselectAllDisplays();
  Serial.println("[boot] CS pins configured");

  // ONLY init displays that are enabled — tft.init() pulses the SHARED
  // RST line every call, so iterating over disabled panels would reset
  // (but not re-init) the enabled panels later in the loop, leaving
  // them in a DISPLAY-OFF state. Symptom of that bug: setup finishes,
  // screen shows the reset flash → black, then later draw calls have
  // no visible effect.
  auto init_panel = [](int cs_pin) {
    digitalWrite(cs_pin, LOW);
    tft.init();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);
    digitalWrite(cs_pin, HIGH);
    Serial.printf("[boot] display init OK on CS=%d\n", cs_pin);
  };
  if (ENABLE_LEFT_DISPLAY)    init_panel(CS_LEFT);
  if (ENABLE_RIGHT_DISPLAY)   init_panel(CS_RIGHT);
  if (ENABLE_PREVIEW_DISPLAY) init_panel(CS_PREVIEW);

  // Sprite framebuffer — one 16-bit RGB565 buffer the size of the
  // configured panel (240×320 in Wokwi/ILI9341, 240×240 on ST7789/
  // GC9A01). Re-used per display: each frame we render into the
  // sprite up to three times and pushSprite() to the matching CS.
  // setSwapBytes(true) flips RGB565 byte order to match what the
  // panel expects.
  if (spr.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT) == nullptr) {
    Serial.printf("[boot] Sprite alloc failed — %d x %d x 2 bytes too big.\n",
                  SCREEN_WIDTH, SCREEN_HEIGHT);
    for (;;) { delay(1000); }
  }
  spr.setSwapBytes(true);
  Serial.printf("[boot] sprite allocated: %d x %d\n",
                SCREEN_WIDTH, SCREEN_HEIGHT);

  eyeStateInit(eyes, millis());
  next_frame_ms = millis();
  Serial.println("[boot] setup() complete, entering loop");
}

void loop() {
  uint32_t now = millis();
  if ((int32_t)(now - next_frame_ms) < 0) return;
  next_frame_ms = now + FRAME_INTERVAL_MS;

  // TEST diagnostic: bypass everything and cycle solid colors via
  // tft.fillScreen. With the GC9A01 env's TFT_CS=10 build flag the
  // library now drives CS itself — no manual select/deselect needed.
  if (TEST_DIRECT_FILL) {
    static uint32_t next_fill_ms = 0;
    static uint8_t  idx          = 0;
    if ((int32_t)(now - next_fill_ms) >= 0) {
      next_fill_ms = now + 1000;
      const uint16_t colors[] = { TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE };
      const char*    names[]  = { "RED", "GREEN", "BLUE", "WHITE" };
      tft.fillScreen(colors[idx % 4]);
      Serial.printf("[test] fillScreen %s\n", names[idx % 4]);
      ++idx;
    }
    return;
  }

  // Pinning every frame so the autonomous re-roll inside sleepyUpdate()
  // can't overwrite the test value 8–25 s in.
  if (TEST_PIN_MAX_SLEEPY) {
    eyeStateSetSleepy(eyes, 1.0f);
  }

  // Future gaze-tracking integration would call:
  //   eyeSetGazeTarget(eyes, face_dx_px, face_dy_px, /*weight=*/1.0f);
  // every frame from a face detector. Micro motion smooths the signal,
  // so even noisy detections turn into organic eye follow.

  const uint32_t t_update_start = micros();
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
  const uint32_t t_update_end = micros();

  // Render once per display into the shared sprite, then pushSprite()
  // with the matching CS asserted. Each push is gated so disabling a
  // panel for testing actually frees its SPI slot instead of just
  // hiding the output. Per-phase micros() let the FPS counter below
  // break down loop time into update / render / push so we can target
  // optimizations to the actual bottleneck.
  uint32_t render_us = 0;
  uint32_t push_us   = 0;

  if (ENABLE_LEFT_DISPLAY) {
    const uint32_t r0 = micros();
    renderEyeOn(spr, kBigLeftEye, eyes);
    const uint32_t r1 = micros();
    selectDisplay(CS_LEFT);
    spr.pushSprite(SPRITE_X, SPRITE_Y);
    const uint32_t r2 = micros();
    render_us += (r1 - r0);
    push_us   += (r2 - r1);
  }

  if (ENABLE_RIGHT_DISPLAY) {
    const uint32_t r0 = micros();
    renderEyeOn(spr, kBigRightEye, eyes);
    const uint32_t r1 = micros();
    selectDisplay(CS_RIGHT);
    spr.pushSprite(SPRITE_X, SPRITE_Y);
    const uint32_t r2 = micros();
    render_us += (r1 - r0);
    push_us   += (r2 - r1);
  }

  if (ENABLE_PREVIEW_DISPLAY && has_preview) {
    const uint32_t r0 = micros();
    renderEyes(spr, kPreviewLeft, kPreviewRight, eyes);
    const uint32_t r1 = micros();
    selectDisplay(CS_PREVIEW);
    pushPreviewSprite(spr, kPreviewLeft, kPreviewRight, eyes.pose);
    const uint32_t r2 = micros();
    render_us += (r1 - r0);
    push_us   += (r2 - r1);
  }

  deselectAllDisplays();
  const uint32_t update_us = t_update_end - t_update_start;

  // Debug heartbeat — gated by ENABLE_HEARTBEAT so it's a no-op when
  // disabled (no Serial.printf cost at all, not even argument eval).
  if (ENABLE_HEARTBEAT) {
    static uint32_t next_hb_ms = 0;
    if ((int32_t)(now - next_hb_ms) >= 0) {
      next_hb_ms = now + 250;
      Serial.printf("[hb] t=%lu drift=(%.2f,%.2f) lid=%.2f next_blink_in=%ld "
                    "gaze_l=(%.2f,%.2f) gaze_r=(%.2f,%.2f)\n",
                    (unsigned long)now,
                    eyes.pose.pupil_dx, eyes.pose.pupil_dy,
                    eyes.blink.lid_close,
                    (long)((int32_t)(eyes.blink.blink_next_ms - now)),
                    eyes.idle_gaze.gx_l, eyes.idle_gaze.gy_l,
                    eyes.idle_gaze.gx_r, eyes.idle_gaze.gy_r);
    }
  }

  // FPS + phase-time breakdown — one printf per second tells us the
  // true loop rate plus where the time went (update / render / push).
  // Counter cost is a few increments + one compare per loop, below
  // noise.
  if (ENABLE_FPS_COUNTER) {
    static uint32_t loop_count   = 0;
    static uint32_t fps_next_ms  = 1000;
    static uint32_t sum_upd_us   = 0;
    static uint32_t sum_render_us= 0;
    static uint32_t sum_push_us  = 0;
    ++loop_count;
    sum_upd_us    += update_us;
    sum_render_us += render_us;
    sum_push_us   += push_us;
    if ((int32_t)(now - fps_next_ms) >= 0) {
      fps_next_ms = now + 1000;
      const uint32_t n = loop_count ? loop_count : 1;
      Serial.printf("[fps] %lu  upd=%luus  render=%luus  push=%luus  (total ~%luus/loop)\n",
                    (unsigned long)loop_count,
                    (unsigned long)(sum_upd_us    / n),
                    (unsigned long)(sum_render_us / n),
                    (unsigned long)(sum_push_us   / n),
                    (unsigned long)((sum_upd_us + sum_render_us + sum_push_us) / n));
      loop_count = sum_upd_us = sum_render_us = sum_push_us = 0;
    }
  }
}
