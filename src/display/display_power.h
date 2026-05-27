// Display sleep / wake helper for the Mode FSM's DeepSleep state.
//
// Owns the TFT_eSPI sleep + display-off command sequence and the CS-mux
// dance needed to fire it at every connected panel at once. Lives outside
// the anim layer because power is a system concern, not an animation one.
//
// Lifecycle:
//   * setup() calls displayPowerInit(&tft, cs_pins, n) once after tft.init().
//   * Mode states call display_set_active(true|false) — true on wake, false
//     on DeepSleep entry.

#pragma once

#include <Arduino.h>

class TFT_eSPI;

// One-shot init. Stash the TFT_eSPI instance and the list of CS pins so
// display_set_active() can address every panel without main.cpp having to
// pass them in every time. The pointer is non-owning — caller must keep
// `tft` alive for the program lifetime (always true today, since it's a
// global in main.cpp).
void displayPowerInit(TFT_eSPI *tft, const int *cs_pins, uint8_t n_cs);

// Put every panel to sleep (active=false) or wake them (active=true).
// Idempotent — calling with the same value twice is harmless.
void display_set_active(bool active);

// Diagnostic — current state.
bool display_is_active();
