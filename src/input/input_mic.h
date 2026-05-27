// Microphone input adapter (stub).
//
// Contract for the real implementation:
//   * Sample noise level continuously, write g_world.noise_floor_db /
//     noise_peak_db.
//   * Edge events:
//       - NoiseLoud  when peak crosses an upper threshold,
//       - NoiseQuiet when it falls back below a lower threshold,
//       - FingerSnap from a transient detector (high peak, short).
//   * Bump g_world.last_interaction_ms on NoiseLoud / FingerSnap.

#pragma once

#include <Arduino.h>

void inputMicInit(uint32_t now_ms);
void inputMicPoll(uint32_t now_ms);
