#include "mode_deep_sleep.h"

#include "../../anim/eye_anim.h"
#include "../../display/display_power.h"
#include "../world_state.h"

void mode_deep_sleep_on_enter(ModeCtx &ctx, uint32_t /*now_ms*/) {
  // Park the eyes at full droop so if a frame ever sneaks through before
  // the display sleep command lands, what flashes briefly is "asleep" not
  // "wide awake."
  if (ctx.eyes) {
    eyeStateSetSleepy(*ctx.eyes, 1.0f);
    eyeClearGazeTarget(*ctx.eyes);
  }

  // Skip the SPI sleep command on the very first (Boot -> DeepSleep)
  // entry. The displays were just initialized in setup(); slamming
  // DISPOFF + SLPIN at them immediately can wedge the Wokwi ILI9341
  // simulation (and on real hardware, it interleaves badly with the
  // panel's post-init wake settling). The next DeepSleep entry — which
  // arrives via Sleepy -> DeepSleep, well after the display has been
  // happily driven for tens of seconds — runs the command normally.
  if (g_world.previous != ModeId::Boot) {
    display_set_active(false);
  }
}

void mode_deep_sleep_on_exit(ModeCtx & /*ctx*/, uint32_t /*now_ms*/) {
  display_set_active(true);
}

void mode_deep_sleep_on_event(ModeCtx & /*ctx*/, const Event & /*ev*/,
                              uint32_t /*now_ms*/) {
  // No work here — the input layer that pushed the event also bumps
  // g_world.last_interaction_ms, and update() below uses that as the
  // single wake condition. Keeping the wake decision in one place
  // (update) means one grep answers "what wakes the toy?"
}

ModeId mode_deep_sleep_update(ModeCtx &ctx, uint32_t /*now_ms*/) {
  // Wake when ANY input has been observed since we entered DeepSleep.
  // Input polls update last_interaction_ms on every interaction-class
  // event (NoiseLoud, FingerSnap, ButtonPress, BatteryPlugged,
  // FaceDetected). One condition covers them all.
  if ((int32_t)(g_world.last_interaction_ms - ctx.entered_ms) > 0) {
    return ModeId::WakeUp;
  }
  return ModeId::DeepSleep;
}
