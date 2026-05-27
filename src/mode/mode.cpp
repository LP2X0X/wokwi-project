#include "mode.h"

#include "../event/event_queue.h"
#include "mode_timers.h"
#include "world_state.h"
#include "states/mode_deep_sleep.h"
#include "states/mode_wake_up.h"
#include "states/mode_idle.h"
#include "states/mode_sleepy.h"
#include "states/mode_interaction.h"

namespace {

// Boot mode has no real hooks — it exists only so g_world.current is valid
// before the first transition into DeepSleep happens during modeFsmInit.
ModeId boot_update(ModeCtx &, uint32_t) {
  return ModeId::DeepSleep;
}

// Dispatch table indexed by ModeId. Static const, lives in .rodata.
// Every row must be in declaration order with ModeId. The static_assert at
// the bottom guards against forgetting a row when a new mode is added.
const ModeVTable kModeTable[(int)ModeId::Count_] = {
  /* Boot        */ { nullptr,                    nullptr,
                       nullptr,                    boot_update },
  /* DeepSleep   */ { mode_deep_sleep_on_enter,   mode_deep_sleep_on_exit,
                       mode_deep_sleep_on_event,   mode_deep_sleep_update },
  /* WakeUp      */ { mode_wake_up_on_enter,      nullptr,
                       nullptr,                    mode_wake_up_update },
  /* Idle        */ { mode_idle_on_enter,         nullptr,
                       mode_idle_on_event,         mode_idle_update },
  /* Sleepy      */ { mode_sleepy_on_enter,       mode_sleepy_on_exit,
                       mode_sleepy_on_event,       mode_sleepy_update },
  /* Interaction */ { mode_interaction_on_enter,  mode_interaction_on_exit,
                       mode_interaction_on_event,  mode_interaction_update },
};

static_assert(sizeof(kModeTable) / sizeof(kModeTable[0]) ==
              (int)ModeId::Count_,
              "kModeTable size must match ModeId::Count_");

const char *kModeNames[(int)ModeId::Count_] = {
  "Boot", "DeepSleep", "WakeUp", "Idle", "Sleepy", "Interaction",
};

ModeCtx g_ctx{};

void transition_to(ModeId next, uint32_t now_ms) {
  if (next == g_world.current) return;

  const ModeVTable &from = kModeTable[(int)g_world.current];
  if (from.on_exit) from.on_exit(g_ctx, now_ms);

  Serial.printf("[mode] %s -> %s @ %lums\n",
                kModeNames[(int)g_world.current],
                kModeNames[(int)next],
                (unsigned long)now_ms);

  g_world.previous        = g_world.current;
  g_world.current         = next;
  g_world.mode_entered_ms = now_ms;
  g_ctx.entered_ms        = now_ms;

  // Wipe any events buffered during the previous mode so they don't fire
  // in the new one — events are edges, semantics belong to the state that
  // was running when they occurred.
  eventQueueClear();

  const ModeVTable &to = kModeTable[(int)next];
  if (to.on_enter) to.on_enter(g_ctx, now_ms);
}

// Global "any -> X" edges. Runs once per tick BEFORE the per-state update.
// Keep this short — only put things here that should preempt every state.
ModeId apply_global_transitions(uint32_t /*now_ms*/) {
  if (g_world.battery_pct < mode_timers::BATTERY_LOW_PCT &&
      !g_world.battery_plugged &&
      g_world.current != ModeId::DeepSleep) {
    return ModeId::DeepSleep;
  }
  return g_world.current;
}

}  // namespace

void modeFsmInit(EyeState &eyes, uint32_t now_ms) {
  worldStateInit(now_ms);
  g_ctx = ModeCtx{};
  g_ctx.eyes       = &eyes;
  g_ctx.entered_ms = now_ms;

  // The toy boots into DeepSleep — diagram says this is where battery-plug
  // / noise / finger-snap brings us out of. Calling transition_to lets
  // mode_deep_sleep_on_enter put displays to sleep correctly.
  transition_to(ModeId::DeepSleep, now_ms);
}

void modeFsmTick(uint32_t now_ms) {
  // 1. Drain events into the current state.
  Event ev;
  while (eventPop(ev)) {
    const ModeVTable &vt = kModeTable[(int)g_world.current];
    if (vt.on_event) vt.on_event(g_ctx, ev, now_ms);
  }

  // 2. Global edges (catastrophic battery, etc).
  ModeId next = apply_global_transitions(now_ms);
  if (next != g_world.current) {
    transition_to(next, now_ms);
    return;  // skip update() this tick so the new state's on_enter runs
             // cleanly with a fresh ctx and zero entered-time delta.
  }

  // 3. Per-state tick. The state requests a transition by returning a
  // different ModeId.
  const ModeVTable &vt = kModeTable[(int)g_world.current];
  if (vt.update) {
    ModeId asked = vt.update(g_ctx, now_ms);
    if (asked != g_world.current) {
      transition_to(asked, now_ms);
    }
  }
}

ModeId modeFsmCurrent() {
  return g_world.current;
}

const char *modeIdName(ModeId id) {
  const int i = (int)id;
  if (i < 0 || i >= (int)ModeId::Count_) return "?";
  return kModeNames[i];
}

void modeFsmRequest(ModeId next, uint32_t now_ms) {
  transition_to(next, now_ms);
}
