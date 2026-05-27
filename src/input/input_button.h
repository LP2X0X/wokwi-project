// Physical-button input adapter (stub).
//
// Real implementation: debounce a GPIO, fire ButtonPress on edge and
// ButtonHold after a hold threshold. Bump last_interaction_ms on press.

#pragma once

#include <Arduino.h>

void inputButtonInit(uint32_t now_ms);
void inputButtonPoll(uint32_t now_ms);
