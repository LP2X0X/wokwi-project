#include "mode_sleepy.h"

#include <esp_random.h>

#include "../../anim/eye_anim.h"
#include "../mode_timers.h"
#include "../world_state.h"

namespace {

// "Random dozing" cadence — every few seconds, briefly bump sleepy to 1.0
// then let it ease back. The blink behavior already produces long-holds
// when sleepy is high, so this reads as a microsleep.
constexpr uint32_t DOZE_MIN_MS = 4000;
constexpr uint32_t DOZE_MAX_MS = 9000;

uint32_t next_doze_at(uint32_t now_ms) {
  return now_ms + DOZE_MIN_MS + (esp_random() % (DOZE_MAX_MS - DOZE_MIN_MS));
}

}  // namespace

void mode_sleepy_on_enter(ModeCtx &ctx, uint32_t now_ms) {
  // Pin sleepy hard. The sleepy behavior re-rolls its own target every
  // 8-25s, so we need to drive it from outside if we want the eyes to
  // STAY droopy the whole mode. main.cpp already does this for the
  // TEST_PIN_MAX_SLEEPY case — here we just call it once on enter and
  // again from the doze loop in update().
  if (ctx.eyes) {
    eyeStateSetSleepy(*ctx.eyes, 0.85f);
    eyeClearGazeTarget(*ctx.eyes);
  }
  ctx.s.sleepy.next_doze_ms = next_doze_at(now_ms);
}

void mode_sleepy_on_exit(ModeCtx & /*ctx*/, uint32_t /*now_ms*/) {
  // No teardown — WakeUp's on_enter pins sleepy to a lower value, so we
  // don't need to clear here.
}

void mode_sleepy_on_event(ModeCtx & /*ctx*/, const Event &ev,
                          uint32_t /*now_ms*/) {
  // Any interaction-class event yanks us out via update(). The input
  // layer has already bumped g_world.last_interaction_ms; nothing to do
  // here except note that we DON'T react in-place (we want WakeUp's
  // scripted sequence to play, not an instant attentive snap).
  (void)ev;
}

ModeId mode_sleepy_update(ModeCtx &ctx, uint32_t now_ms) {
  // Any recent interaction -> back through WakeUp. We use the same
  // last_interaction_ms vs entered_ms test DeepSleep uses, so the
  // wake condition is uniform across both "asleep" modes.
  if ((int32_t)(g_world.last_interaction_ms - ctx.entered_ms) > 0) {
    return ModeId::WakeUp;
  }

  // No interaction long enough -> drop into DeepSleep. We DON'T use
  // last_interaction_ms here because that timestamp didn't move during
  // the Idle->Sleepy crossover; instead we measure dwell inside Sleepy.
  if ((int32_t)(now_ms - ctx.entered_ms) >
      (int32_t)mode_timers::SLEEPY_TO_DEEP_SLEEP_MS) {
    return ModeId::DeepSleep;
  }

  // Microsleep "doze" beat — pin sleepy to 1.0 for a moment so the blink
  // behavior produces a long hold, then schedule the next.
  if ((int32_t)(now_ms - ctx.s.sleepy.next_doze_ms) >= 0) {
    if (ctx.eyes) eyeStateSetSleepy(*ctx.eyes, 1.0f);
    ctx.s.sleepy.next_doze_ms = next_doze_at(now_ms);
  } else {
    // Outside doze beats, keep sleepy pinned at 0.85 so the sleepy
    // behavior's autonomous reroll can't pull us back to wide-awake
    // mid-mode.
    if (ctx.eyes) eyeStateSetSleepy(*ctx.eyes, 0.85f);
  }

  return ModeId::Sleepy;
}
