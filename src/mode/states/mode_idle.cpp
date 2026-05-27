#include "mode_idle.h"

#include <esp_random.h>

#include "../../anim/eye_anim.h"
#include "../mode_timers.h"
#include "../world_state.h"

namespace {

// Ambient curiosity cadence in Idle — a small "what was that?" tic every
// few seconds keeps the toy feeling alive even with nothing happening.
constexpr uint32_t IDLE_CURIOSITY_MIN_MS = 6000;
constexpr uint32_t IDLE_CURIOSITY_MAX_MS = 12000;

uint32_t next_curiosity_at(uint32_t now_ms) {
  const uint32_t span = IDLE_CURIOSITY_MAX_MS - IDLE_CURIOSITY_MIN_MS;
  return now_ms + IDLE_CURIOSITY_MIN_MS + (esp_random() % span);
}

}  // namespace

void mode_idle_on_enter(ModeCtx &ctx, uint32_t now_ms) {
  if (ctx.eyes) {
    eyeStateSetSleepy(*ctx.eyes, 0.0f);
    eyeClearGazeTarget(*ctx.eyes);
  }
  ctx.s.idle.next_idle_curiosity_ms = next_curiosity_at(now_ms);
  ctx.s.idle.lonely_fired           = false;
}

void mode_idle_on_event(ModeCtx &ctx, const Event &ev, uint32_t /*now_ms*/) {
  switch (ev.type) {
    case EventType::FingerSnap:
      // Surprising / playful interaction — small curiosity burst.
      if (ctx.eyes) eyeTriggerCuriosity(*ctx.eyes, 0.85f, 1200);
      break;
    case EventType::NoiseLoud:
      // Something happened nearby — attentive (not curious) since we
      // don't know the source direction yet.
      if (ctx.eyes) eyeTriggerAttentive(*ctx.eyes, 0.6f, 1500, 0.0f, 0.0f);
      break;
    default:
      // FaceDetected -> handled in update() (transitions to Interaction).
      // ButtonPress / Battery* -> not Idle's job to react to.
      break;
  }
}

ModeId mode_idle_update(ModeCtx &ctx, uint32_t now_ms) {
  // Face -> Interaction. We let input_face latch face_present on
  // WorldState; one read here is enough.
  if (g_world.face_present) {
    return ModeId::Interaction;
  }

  // No interaction in a long time -> Sleepy.
  if ((int32_t)(now_ms - g_world.last_interaction_ms) >
      (int32_t)mode_timers::IDLE_TO_SLEEPY_MS) {
    return ModeId::Sleepy;
  }

  // Ambient "lonely" tint after a quieter spell. Fires once per Idle stay
  // so it doesn't spam the curiosity behavior. (Real "lonely" emotion is
  // a separate behavior file you'd add later; for now we just nudge an
  // attentive look-around as a placeholder so the timing is correct when
  // the real behavior lands.)
  if (!ctx.s.idle.lonely_fired &&
      (int32_t)(now_ms - g_world.last_interaction_ms) >
      (int32_t)mode_timers::IDLE_LONELY_AFTER_MS) {
    if (ctx.eyes) {
      eyeTriggerAttentive(*ctx.eyes, 0.3f, 2500, 0.0f, 0.2f);
    }
    ctx.s.idle.lonely_fired = true;
  }

  // Ambient curiosity tic.
  if ((int32_t)(now_ms - ctx.s.idle.next_idle_curiosity_ms) >= 0) {
    if (ctx.eyes) {
      eyeTriggerCuriosity(*ctx.eyes, 0.35f, 900);
    }
    ctx.s.idle.next_idle_curiosity_ms = next_curiosity_at(now_ms);
  }

  return ModeId::Idle;
}
