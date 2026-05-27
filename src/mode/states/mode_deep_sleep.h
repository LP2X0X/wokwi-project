// DeepSleep mode: black screen, mic still listening, awaits a wake event.
//
// Hooks own:
//   on_enter -> put displays to sleep, clamp emotion targets.
//   on_event -> any wake event (noise loud, finger snap, button press,
//               battery plugged in) returns to WakeUp via a request.
//   update   -> nothing per-frame; just stays.
//   on_exit  -> bring displays back up.

#pragma once

#include "../mode.h"

void mode_deep_sleep_on_enter(ModeCtx &ctx, uint32_t now_ms);
void mode_deep_sleep_on_exit (ModeCtx &ctx, uint32_t now_ms);
void mode_deep_sleep_on_event(ModeCtx &ctx, const Event &ev, uint32_t now_ms);
ModeId mode_deep_sleep_update(ModeCtx &ctx, uint32_t now_ms);
