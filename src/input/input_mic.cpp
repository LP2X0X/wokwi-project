#include "input_mic.h"

#include "../mode/world_state.h"

// No-op stub. Real implementation samples I2S mic, computes RMS, applies
// hysteresis, detects snaps. Until the hardware is wired in we leave
// the world quiet so the FSM is exercised purely by the face stub.
void inputMicInit(uint32_t /*now_ms*/) {
  g_world.noise_floor_db = -60.0f;
  g_world.noise_peak_db  = -60.0f;
}

void inputMicPoll(uint32_t /*now_ms*/) {
  // No work yet. When wired in, this is the place to:
  //   1. Read latest mic sample(s).
  //   2. Update g_world.noise_floor_db / noise_peak_db.
  //   3. eventPush NoiseLoud / NoiseQuiet / FingerSnap on edges.
  //   4. Bump g_world.last_interaction_ms on interaction-class events.
}
