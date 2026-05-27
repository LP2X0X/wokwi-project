// Sleepy mode: droopy eyelids, occasional dozing (microsleep blinks).
//
// Exits:
//   * SLEEPY_TO_DEEP_SLEEP_MS without interaction -> DeepSleep.
//   * Any interaction event -> WakeUp (via on_event request).

#pragma once

#include "../mode.h"

void mode_sleepy_on_enter(ModeCtx &ctx, uint32_t now_ms);
void mode_sleepy_on_exit (ModeCtx &ctx, uint32_t now_ms);
void mode_sleepy_on_event(ModeCtx &ctx, const Event &ev, uint32_t now_ms);
ModeId mode_sleepy_update(ModeCtx &ctx, uint32_t now_ms);
