// Persistent blackboard shared across the Mode FSM, input layer, and (read
// only) main.cpp.
//
// This is a separate concept from `Modulators` in the animation engine:
//   * `Modulators` is per-frame, rebuilt every tick, transient. Lives in
//     anim/eye_pose.h.
//   * `WorldState` is persistent across frames. Continuous sensor levels
//     (face_x, noise floor, battery pct) live here; edge events go through
//     the event queue.
//
// Read/write contract:
//   * src/input/*    -> writes sensor levels, pushes edge events.
//   * src/mode/*     -> writes current/previous/mode_entered_ms; reads
//                       everything else.
//   * src/anim/*     -> does NOT see WorldState. Communication stays
//                       through the public eyeXxx*() API in anim/eye_anim.h.
//   * src/main.cpp   -> may read for diagnostics (e.g. log mode changes).
//
// One global `g_world` defined in world_state.cpp. Static allocation, no
// heap.

#pragma once

#include <Arduino.h>

// Forward decl — full enum in mode.h, kept out of this header to avoid a
// circular include (mode.h includes world_state.h for transition bookkeeping).
enum class ModeId : uint8_t;

struct WorldState {
  // Monotonic timing.
  uint32_t boot_ms;

  // Updated by ANY interaction signal (face, sound, snap, button). The Idle
  // -> Sleepy timer and the "lonely" tint both read this.
  uint32_t last_interaction_ms;

  // When did the current mode become active. Used for "have we been here
  // long enough to leave?" checks inside per-state update().
  uint32_t mode_entered_ms;

  // ---- Face detector levels (continuous; written by input_face). ----
  bool     face_present;
  float    face_x;        // pixels of pupil offset from neutral, same units as drift
  float    face_y;
  float    face_conf;     // 0..1; consumer can threshold as needed
  uint32_t face_last_seen_ms;

  // ---- Microphone levels (continuous; written by input_mic). ----
  float    noise_floor_db;
  float    noise_peak_db;

  // ---- Power (continuous; written by input_power). ----
  uint8_t  battery_pct;
  bool     battery_plugged;

  // ---- Mode bookkeeping (written by mode.cpp::transition_to). ----
  ModeId   current;
  ModeId   previous;
};

extern WorldState g_world;

// One-shot init: zero everything, set boot_ms, current = Boot.
void worldStateInit(uint32_t now_ms);
