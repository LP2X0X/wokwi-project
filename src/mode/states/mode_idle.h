// Idle mode: random gazing, ambient emotion. The "default" state.
//
// Exits:
//   * Face detected (event) -> Interaction.
//   * No interaction for IDLE_TO_SLEEPY_MS -> Sleepy.

#pragma once

#include "../mode.h"

void mode_idle_on_enter(ModeCtx &ctx, uint32_t now_ms);
void mode_idle_on_event(ModeCtx &ctx, const Event &ev, uint32_t now_ms);
ModeId mode_idle_update(ModeCtx &ctx, uint32_t now_ms);
