// Interaction mode: excited + face tracking.
//
// Exits:
//   * No face for INTERACTION_IDLE_TIMEOUT_MS -> Idle.

#pragma once

#include "../mode.h"

void mode_interaction_on_enter(ModeCtx &ctx, uint32_t now_ms);
void mode_interaction_on_exit (ModeCtx &ctx, uint32_t now_ms);
void mode_interaction_on_event(ModeCtx &ctx, const Event &ev, uint32_t now_ms);
ModeId mode_interaction_update(ModeCtx &ctx, uint32_t now_ms);
