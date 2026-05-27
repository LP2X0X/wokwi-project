#include "mode_wake_up.h"

#include "../../anim/eye_anim.h"
#include "../mode_timers.h"

namespace {

// Scripted "boot" sequence. Each entry's t_ms is relative to the moment
// we entered WakeUp. Lambdas have no captures so they decay to function
// pointers and the whole table sits in .rodata.
//
// The diagram describes WakeUp as "sleepy face -> attentive listening ->
// curious." This script encodes exactly that beat sequence and is the
// reason WakeUp is a single mode rather than three nested ones.
struct WakeUpStep {
  uint32_t t_ms;
  void   (*action)(EyeState &);
};

const WakeUpStep kWakeUpScript[] = {
  // Open the eyes by easing sleepy back from 1.0 (clamped in DeepSleep)
  // down to ~0.6 — still droopy, but visibly waking.
  {    0, [](EyeState &e) {
            eyeStateSetSleepy(e, 0.6f);
          }},

  // Attentive beat: head tilts, ears prick. Direction is forward (0, 0)
  // since we don't yet know where the wake-up signal came from.
  {  500, [](EyeState &e) {
            eyeStateSetSleepy(e, 0.3f);
            eyeTriggerAttentive(e, 0.7f, 1500, 0.0f, 0.0f);
          }},

  // Curious beat: wide eyes, "what was that?"
  { 1500, [](EyeState &e) {
            eyeStateSetSleepy(e, 0.0f);
            eyeTriggerCuriosity(e, 0.8f, 1500);
          }},
};
constexpr uint8_t kWakeUpScriptLen =
    sizeof(kWakeUpScript) / sizeof(kWakeUpScript[0]);

}  // namespace

void mode_wake_up_on_enter(ModeCtx &ctx, uint32_t /*now_ms*/) {
  ctx.s.wake_up.step             = 0;
  ctx.s.wake_up.step_started_ms  = 0;
}

ModeId mode_wake_up_update(ModeCtx &ctx, uint32_t now_ms) {
  const uint32_t elapsed = now_ms - ctx.entered_ms;

  // Walk the script: fire any beats whose t_ms is <= elapsed.
  while (ctx.s.wake_up.step < kWakeUpScriptLen &&
         kWakeUpScript[ctx.s.wake_up.step].t_ms <= elapsed) {
    if (ctx.eyes) {
      kWakeUpScript[ctx.s.wake_up.step].action(*ctx.eyes);
    }
    ++ctx.s.wake_up.step;
  }

  // Done when the whole script has played out AND we've held the last
  // beat long enough that the toy reads as awake, not mid-yawn.
  if (elapsed >= mode_timers::WAKE_UP_TOTAL_MS) {
    return ModeId::Idle;
  }
  return ModeId::WakeUp;
}
