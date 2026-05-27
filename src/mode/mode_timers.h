// All transition timeouts for the Mode FSM in one place.
//
// Test values are intentionally short so the full diagram (Idle -> Sleepy
// -> DeepSleep -> WakeUp -> ...) can be exercised in a single Wokwi session
// without waiting tens of minutes. The "real" comments next to each
// constant are the production values the diagram specifies; flip them when
// you're done tuning behavior.

#pragma once

#include <Arduino.h>

namespace mode_timers {

// Idle -> Sleepy after no interaction for this long.
constexpr uint32_t IDLE_TO_SLEEPY_MS           = 30UL * 1000;       // real: 5 * 60 * 1000

// Sleepy -> DeepSleep after this long with the eyes already drooping.
constexpr uint32_t SLEEPY_TO_DEEP_SLEEP_MS     = 30UL * 1000;       // real: 5 * 60 * 1000

// Interaction -> Idle once the face / sound has been gone this long.
constexpr uint32_t INTERACTION_IDLE_TIMEOUT_MS = 20UL * 1000;       // real: 5 * 60 * 1000

// Ambient "lonely" tint shows up inside Idle after this long with nothing
// happening. Independent of the Idle -> Sleepy timer; just a vibe shift.
constexpr uint32_t IDLE_LONELY_AFTER_MS        = 15UL * 1000;       // real: 90 * 1000

// Total length of the scripted WakeUp sequence (sleepy -> attentive ->
// curious). The script in mode_wake_up.cpp lists per-beat timestamps;
// this is the duration after which control hands off to Idle.
constexpr uint32_t WAKE_UP_TOTAL_MS            = 3000;

// Low-battery threshold (percent). Anywhere below this AND not plugged in
// forces an immediate jump to DeepSleep — the global edge in mode.cpp.
constexpr uint8_t  BATTERY_LOW_PCT             = 5;

}  // namespace mode_timers
