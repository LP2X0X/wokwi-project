// WakeUp mode: scripted "boot up" sequence sleepy -> attentive -> curious,
// then hands off to Idle. The script is a static step array in the .cpp.

#pragma once

#include "../mode.h"

void mode_wake_up_on_enter(ModeCtx &ctx, uint32_t now_ms);
ModeId mode_wake_up_update(ModeCtx &ctx, uint32_t now_ms);
