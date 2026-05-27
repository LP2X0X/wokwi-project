#include "mode_interaction.h"

#include "../../anim/eye_anim.h"
#include "../mode_timers.h"
#include "../world_state.h"

void mode_interaction_on_enter(ModeCtx &ctx, uint32_t /*now_ms*/) {
  // Excited! Wake any sleepy bias, fire the curiosity expression as the
  // "ah, a friend!" beat.
  if (ctx.eyes) {
    eyeStateSetSleepy(*ctx.eyes, 0.0f);
    eyeTriggerCuriosity(*ctx.eyes, 0.9f, 1500);
    eyeTriggerAttentive(*ctx.eyes, 0.7f, 1500,
                        g_world.face_x, g_world.face_y);
  }
  ctx.s.interaction.face_lost_ms = 0;
}

void mode_interaction_on_exit(ModeCtx &ctx, uint32_t /*now_ms*/) {
  // Hand gaze control back to the autonomous system. Idle re-confirms
  // this in its own on_enter, but doing it here keeps the layering clean
  // for any future transitions that skip Idle.
  if (ctx.eyes) eyeClearGazeTarget(*ctx.eyes);
}

void mode_interaction_on_event(ModeCtx &ctx, const Event &ev,
                               uint32_t now_ms) {
  switch (ev.type) {
    case EventType::FaceLost:
      // Don't transition immediately on a single FaceLost — face
      // trackers can drop a frame and recover. update() applies the
      // INTERACTION_IDLE_TIMEOUT_MS grace period.
      ctx.s.interaction.face_lost_ms = now_ms;
      break;
    case EventType::FaceDetected:
      // Face came back (or a different face arrived) — clear the grace
      // timer so we keep tracking.
      ctx.s.interaction.face_lost_ms = 0;
      break;
    case EventType::NoiseLoud:
    case EventType::FingerSnap:
      // Extra interaction during tracking — small attentive nudge.
      if (ctx.eyes) {
        eyeTriggerAttentive(*ctx.eyes, 0.6f, 1000,
                            g_world.face_x, g_world.face_y);
      }
      break;
    default:
      break;
  }
}

ModeId mode_interaction_update(ModeCtx &ctx, uint32_t now_ms) {
  // Drive face tracking every frame while the face is present. The
  // micro_motion behavior blends this into smooth eye-follow.
  if (g_world.face_present && ctx.eyes) {
    eyeSetGazeTarget(*ctx.eyes, g_world.face_x, g_world.face_y, 1.0f);
    ctx.s.interaction.face_lost_ms = 0;  // continuous face = no grace
  }

  // Face has been missing long enough -> back to Idle. face_lost_ms = 0
  // means "face is currently present (or transient drop already healed)."
  if (ctx.s.interaction.face_lost_ms != 0 &&
      (int32_t)(now_ms - ctx.s.interaction.face_lost_ms) >
          (int32_t)mode_timers::INTERACTION_IDLE_TIMEOUT_MS) {
    return ModeId::Idle;
  }

  return ModeId::Interaction;
}
