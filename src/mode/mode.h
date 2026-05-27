// Top-level Mode FSM dispatcher.
//
// Architecture in one paragraph:
//   * A flat FSM, NOT hierarchical. Each ModeId picks one row in a static
//     `kModeTable[]` of ModeVTable function pointers.
//   * Per-mode scratch state lives in a union inside `ModeCtx`, so the
//     entire FSM context is one statically allocated object. No heap, no
//     vtables, no inheritance.
//   * Mode states call into the existing animation engine through the
//     public eyeXxx*() API (anim/eye_anim.h). They do NOT reach into
//     behavior internals. Layering stays clean.
//   * Normal transitions come from each state's update() return value.
//     Global "any -> X" edges live in apply_global_transitions() in
//     mode.cpp — one place to grep for interrupt-style transitions.
//
// Adding a new mode: add a row to ModeId, write src/mode/states/mode_xxx.h
// + .cpp with the four hooks, add a row to kModeTable in mode.cpp.

#pragma once

#include <Arduino.h>

#include "../event/event.h"

// Forward decl so mode states can hold a pointer to the existing EyeState
// without dragging the full anim header into every state .h. The .cpp
// files that actually call eyeXxx*() include "anim/eye_anim.h" themselves.
struct EyeState;

enum class ModeId : uint8_t {
  Boot = 0,
  DeepSleep,
  WakeUp,
  Idle,
  Sleepy,
  Interaction,
  Count_,  // sentinel; not a real mode.
};

// Per-mode scratch. The union keeps all five states' working memory inside
// one ModeCtx so the FSM is exactly one statically allocated object.
//
// Adding a new mode that needs scratch: add a new member to the union.
// Modes with no scratch don't need an entry.
struct ModeCtx {
  EyeState *eyes;          // non-owning; set once in modeFsmInit.
  uint32_t  entered_ms;    // mirrors g_world.mode_entered_ms; convenience.

  union {
    struct {
      uint8_t  step;             // index into kWakeUpScript
      uint32_t step_started_ms;
    } wake_up;

    struct {
      uint32_t next_idle_curiosity_ms;
      bool     lonely_fired;     // edge-trigger lonely tint once per Idle stay
    } idle;

    struct {
      uint32_t next_doze_ms;     // next "random dozing" beat
    } sleepy;

    struct {
      uint32_t face_lost_ms;     // 0 = face currently present
    } interaction;
  } s;
};

struct ModeVTable {
  void   (*on_enter)(ModeCtx &ctx, uint32_t now_ms);
  void   (*on_exit )(ModeCtx &ctx, uint32_t now_ms);
  void   (*on_event)(ModeCtx &ctx, const Event &ev, uint32_t now_ms);
  // Return the next ModeId. Returning the current mode means "stay."
  ModeId (*update  )(ModeCtx &ctx, uint32_t now_ms);
};

// One-shot init. Wires the FSM to the existing EyeState, seeds WorldState,
// and lands the system in DeepSleep (the canonical starting mode per the
// diagram: the toy boots into a "battery saving" state and only wakes on
// the first input).
void modeFsmInit(EyeState &eyes, uint32_t now_ms);

// Per-frame tick. Drains the event queue into the current state, then
// runs the current state's update(). Cheap — sub-microsecond steady state.
void modeFsmTick(uint32_t now_ms);

// Diagnostic / control.
ModeId modeFsmCurrent();
const char *modeIdName(ModeId id);

// Programmatic transition request. Mode states normally use their update()
// return value, but external code (debug commands, tests) can force a
// transition through this entry point. Idempotent if `next == current`.
void modeFsmRequest(ModeId next, uint32_t now_ms);
