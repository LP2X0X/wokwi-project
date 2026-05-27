# Susuwatori Mode FSM — Orchestration Layer

> **Status:** implemented 2026-05-27. Steps 1–4 of the original plan are live (skeleton, all five states, event queue + stub face input, display power). Step 5 ("lonely" emotion) and step 6 (real sensor drivers) are still pending — see *Implementation order* below.

## Context

The existing animation engine under `src/anim/` is **already well architected** — a 4-pass per-frame pipeline (emotion updates → modulator bus → motion behaviors → pose composition) with a pure renderer. Behaviors are file-per-concern, smooth easing everywhere, no blocking delays. **We are not refactoring it.**

What's missing is the high-level "what is the toy doing right now" layer:
**DeepSleep → WakeUp → Idle ↔ Interaction, and Idle → Sleepy → DeepSleep on inactivity timers.**

This document specifies a thin orchestration layer ON TOP of the existing engine. The new layer:
- decides which mode the toy is in
- routes sensor events (mic, face, button, battery) into mode transitions
- nudges the existing emotion API (`eyeTriggerCuriosity`, `eyeStateSetSleepy`, `eyeSetGazeTarget`, …) — never reaches into `anim/` internals
- owns power management (display sleep/wake in DeepSleep)

The chosen pattern is intentionally boring: a **flat finite state machine** with a static `ModeVTable[]` dispatch array, a small **SPSC event ring buffer**, and a `WorldState` blackboard alongside the existing per-frame `Modulators`. No HSM, no behavior tree, no classes — same C-style structs + free functions the rest of the codebase uses, so the two layers read like they were written by the same person.

## Why this pattern (and not the alternatives)

- **Flat FSM, not hierarchical (HSM).** Six modes do not share enough common behavior to earn the entry/exit cascade complexity of an HSM. The only sub-sequence (WakeUp: sleepy → attentive → curious) is a **scripted timeline of 3 timed trigger calls** — encode it as a step array inside `mode_wake_up.cpp`, not a nested FSM.
- **Not a behavior tree.** BTs shine when many small reactive policies need composition (game AI). Here we have a small fixed set of named modes with explicit transitions — a table beats a tree for readability.
- **Edges-only events + levels-on-blackboard.** Sensors emit events only on edge changes (FaceDetected, NoiseLoud, FingerSnap, BatteryPlugged…); continuous values (face_x/y, noise_floor_db) live on `WorldState`. This keeps the event queue small (16 slots) and prevents per-frame event spam.
- **Mode → emotion via existing public API only.** The FSM is a director; emotions are actors. Mode states call `eyeTrigger*`/`eyeStateSet*`/`eyeSetGazeTarget` exactly as an external sensor integration would. Layering stays clean, and ripping out the FSM tomorrow leaves the eye engine working.

## Architecture diagram

```
┌── Input Layer ──────────────────────────────────────┐
│  src/input/input_face.cpp                            │
│  src/input/input_mic.cpp     poll() each frame      │
│  src/input/input_button.cpp  → writes WorldState    │
│  src/input/input_power.cpp   → pushes edge Events   │
└─────────────────────────────────────────────────────┘
            │ Events (edges)         │ Levels (face_x, noise, battery…)
            ▼                        ▼
┌── Mode FSM ─────────────────────────────────────────┐
│  src/mode/mode.cpp        ModeVTable[] dispatch     │
│  src/mode/states/*        one file per state:       │
│    mode_deep_sleep, wake_up, idle, sleepy,          │
│    interaction                                       │
│  apply_global_transitions() for "any → X" edges     │
└─────────────────────────────────────────────────────┘
            │ calls existing public API
            ▼
┌── Animation Engine (UNCHANGED) ─────────────────────┐
│  src/anim/eye_anim.cpp     4-pass pipeline          │
│  src/anim/behaviors/*      emotions + motion        │
│  src/anim/eye_render.cpp   pure renderer            │
└─────────────────────────────────────────────────────┘
```

## File layout (as built)

```
src/event/
  event.h           # EventType enum + Event POD (tagged union)
  event_queue.h     # static ring buffer API
  event_queue.cpp   # eventPush / eventPop, size 16, overwrite-oldest

src/mode/
  mode.h            # ModeId, ModeCtx, ModeVTable, public API
  mode.cpp          # dispatcher: kModeTable[], modeFsmTick, transition_to,
                    #             apply_global_transitions
  mode_timers.h     # all transition timeouts as constexpr (with real-value
                    # comments next to each one)
  world_state.h     # WorldState blackboard (forward-decls ModeId)
  world_state.cpp   # extern g_world definition + worldStateInit
  states/
    mode_deep_sleep.h/.cpp   # screen off, mic-listening, wake on any input event
    mode_wake_up.h/.cpp      # scripted step array (sleepy → attentive → curious)
    mode_idle.h/.cpp         # random gazing + ambient curiosity tics
    mode_sleepy.h/.cpp       # droopy lids + random doze beats
    mode_interaction.h/.cpp  # face tracking via eyeSetGazeTarget every frame

src/input/
  input_face.h/.cpp     # STUB: scripted face appears 8–12 s into each 25-s cycle
  input_mic.h/.cpp      # stub, init writes a quiet noise floor
  input_button.h/.cpp   # stub
  input_power.h/.cpp    # stub, init pretends plugged in at 100%

src/display/
  display_power.h/.cpp  # display_set_active(bool) — drives DISPOFF+SLPIN /
                        # SLPOUT+DISPON across all CS pins together
```

Everything is statically allocated. Measured cost vs. the previous baseline (`pio run -e esp32s3`): **RAM +16 B, flash +1.0 KB**.

## Key contracts

### `ModeVTable` — one row per mode, indexed by `ModeId`

```cpp
enum class ModeId : uint8_t {
  Boot, DeepSleep, WakeUp, Idle, Sleepy, Interaction, COUNT
};

struct ModeVTable {
  void   (*on_enter)(ModeCtx&, uint32_t now);
  void   (*on_exit )(ModeCtx&, uint32_t now);
  void   (*on_event)(ModeCtx&, const Event&, uint32_t now);
  ModeId (*update  )(ModeCtx&, uint32_t now);   // returns next mode (self = stay)
};

static const ModeVTable kModeTable[(int)ModeId::COUNT] = { /* 5 rows */ };
```

Per-state scratch lives in a **union inside `ModeCtx`** (e.g. `wake_up.step`, `idle.next_idle_curiosity_ms`) so the entire FSM context is one statically-allocated object. No vtables, no heap, no inheritance.

### Transition style — hybrid, not pure table

- **Normal exits**: each state's `update()` returns the next `ModeId` (returning self = stay). Each state file shows its own exits as plain `return ModeId::X;` lines.
- **Global edges** (anything → DeepSleep on critical battery; anything → Interaction on face appears): one small `apply_global_transitions()` in `mode.cpp`, called before per-state update. One file shows you every interrupt-style transition at a glance.

Pure data transition tables become unreadable once conditions get rich (`time > 5min AND battery > 20%`). Pure code-in-states buries the graph. The hybrid keeps both grep-able.

### `WorldState` blackboard (persistent)

Separate from the existing `Modulators` (per-frame, transient):

```cpp
struct WorldState {
  uint32_t boot_ms;
  uint32_t last_interaction_ms;
  uint32_t mode_entered_ms;
  ModeId   current, previous;

  bool     face_present;
  float    face_x, face_y, face_conf;
  uint32_t face_last_seen_ms;

  float    noise_floor_db, noise_peak_db;

  uint8_t  battery_pct;
  bool     battery_plugged;
};
extern WorldState g_world;
```

**Read/write contract:**
- `input/*` writes sensor levels, emits edge events.
- `mode/*` writes `current/previous/mode_entered_ms`, reads everything else.
- `anim/*` does NOT see `WorldState`. Communication stays through the existing public API.

### Wake-condition contract (DeepSleep and Sleepy)

Both `mode_deep_sleep_update()` and `mode_sleepy_update()` wake on the same one-line check:

```cpp
if ((int32_t)(g_world.last_interaction_ms - ctx.entered_ms) > 0) {
  return ModeId::WakeUp;
}
```

The input layer's half of the contract: **any input poll that pushes an interaction-class event (`FaceDetected`, `NoiseLoud`, `FingerSnap`, `ButtonPress`, `BatteryPlugged`) MUST also bump `g_world.last_interaction_ms` to `now_ms`.** That single timestamp is the canonical "user is here" signal — so adding a new wake source means writing to one field, not adding cases to every sleep-class state. Keep this contract when wiring real sensors.

### Event queue — SPSC ring, size 16, polled

```cpp
enum class EventType : uint8_t {
  None, FaceDetected, FaceLost, NoiseLoud, NoiseQuiet, FingerSnap,
  ButtonPress, ButtonHold, BatteryPlugged, BatteryUnplugged, BatteryLow,
};
struct Event { EventType type; uint32_t t_ms; union { ... } payload; };
```

Sensors `eventPush(...)` on edges only. `modeFsmTick()` drains the queue and routes each event to the current state's `on_event` hook. Subscription is a `switch` inside each state's `on_event` — shorter and more grep-able than registered handlers for six states.

## WakeUp sub-sequence: scripted timeline, not nested FSM

Inside `mode_wake_up.cpp`:

```cpp
struct WakeUpStep { uint32_t t_ms; void (*action)(EyeState&); };
static const WakeUpStep kWakeUpScript[] = {
  {    0, [](EyeState &e){ eyeStateSetSleepy(e, 1.0f); }},
  {  600, [](EyeState &e){ eyeStateSetSleepy(e, 0.3f);
                            eyeTriggerAttentive(e, 0.6f, 1200, 0, 0); }},
  { 1500, [](EyeState &e){ eyeTriggerCuriosity(e, 0.8f, 1500); }},
  { 3000, nullptr },  // sentinel → return ModeId::Idle
};
```

Captureless lambdas decay to function pointers; the table lives in `.rodata`.

## Power management

Three layers, decoupled:

1. **Display** — `src/display/display_power.cpp` exposes `display_set_active(bool)`. False sends `TFT_DISPOFF` + `TFT_SLPIN` (works on ILI9341 sim and GC9A01 real hardware). Called only from `mode_deep_sleep_on_enter` / `_on_exit`. No power code in `anim/`.

2. **Frame loop** — `main.cpp` checks `if (modeFsmCurrent() == ModeId::DeepSleep) return;` and skips render + push entirely. The mode FSM still ticks cheaply so it can detect a wake event.

3. **CPU sleep** — explicitly **out of scope for v1**. Skipping render alone removes the ~70 ms/frame of SPI push, which is most of the power draw. `esp_light_sleep_start()` + GPIO wakeup + mic-wake comparator is a v2 step once real hardware lands.

## Main loop after the change (concrete shape)

```cpp
void loop() {
  uint32_t now = millis();
  if ((int32_t)(now - next_frame_ms) < 0) return;
  next_frame_ms = now + FRAME_INTERVAL_MS;

  // 1. Sample sensors (each cheap; edges → eventPush, levels → g_world).
  inputFacePoll(now);
  inputMicPoll(now);
  inputButtonPoll(now);
  inputPowerPoll(now);

  // 2. Run the mode FSM (drains events, runs current state).
  modeFsmTick(now);

  // 3. In DeepSleep, skip rendering. Display already in sleep from
  //    mode_deep_sleep_on_enter.
  if (modeFsmCurrent() == ModeId::DeepSleep) return;

  // 4. Existing animation tick — UNCHANGED.
  eyeStateUpdate(eyes, now);

  // 5. Existing render + push to all CS targets — UNCHANGED.
  // ...
}
```

## Worked examples (the "easy to extend" check)

### Adding a "lonely" emotion

1. New `src/anim/behaviors/lonely.h/.cpp` (state struct + `lonelyInit/Update/Modulate/Trigger`).
2. Add `LonelyState lonely;` to `EyeState`.
3. Wire one line per existing pass in `eye_anim.cpp`'s 4-pass pipeline. Add `eyeTriggerLonely(...)` to `eye_anim.h`.
4. In `mode_idle.cpp::update()`: `if (now - g_world.last_interaction_ms > IDLE_LONELY_AFTER_MS) eyeTriggerLonely(eyes, 0.5f, 4000);`

**Files outside `anim/`: one** (`mode_idle.cpp`).

### Adding a "startled" mode

1. New `src/mode/states/mode_startled.h/.cpp` (POD scratch + the 4 hooks).
2. Add `Startled` to `ModeId`, plus its row in `kModeTable`.
3. In `apply_global_transitions()`: on `NoiseLoud > LOUD_THRESHOLD`, force `Startled`. `mode_startled_update` returns `previous` after `end_ms`.

**Files: 3.** No animation code changes — `mode_startled_on_enter` calls existing `eyeTrigger*` to produce the wide-eye reaction.

## Timer values

All transition timeouts live in one header `src/mode/mode_timers.h` as named `constexpr` so the test-vs-real swap is a one-line change. Each line carries its production value as a comment:

```cpp
namespace mode_timers {
  constexpr uint32_t IDLE_TO_SLEEPY_MS           = 30UL * 1000;   // real: 5 * 60 * 1000
  constexpr uint32_t SLEEPY_TO_DEEP_SLEEP_MS     = 30UL * 1000;   // real: 5 * 60 * 1000
  constexpr uint32_t INTERACTION_IDLE_TIMEOUT_MS = 20UL * 1000;   // real: 5 * 60 * 1000
  constexpr uint32_t IDLE_LONELY_AFTER_MS        = 15UL * 1000;   // real: 90 * 1000
  constexpr uint32_t WAKE_UP_TOTAL_MS            = 3000;
  constexpr uint8_t  BATTERY_LOW_PCT             = 5;             // global edge → DeepSleep
}
```

States read these constants — never inline literals. Flip to real values by editing one file.

## Implementation order

1. ✅ **Skeleton + Boot/Idle + dispatcher.** `event/`, `mode/`, `world_state`, `mode_timers`, all five state files, `main.cpp` wiring.
2. ✅ **Sleepy + WakeUp + DeepSleep states** with the scripted WakeUp step array.
3. ✅ **Event queue + Interaction state + stub `inputFacePoll`** (scripted face cycle so Idle↔Interaction is observable in Wokwi).
4. ✅ **Display power.** `display_set_active(true|false)` wired into `mode_deep_sleep_on_{enter,exit}`. Uses `DISPOFF+SLPIN` / `SLPOUT+DISPON` command pairs across all CS pins together.
5. ⏳ **Add the `lonely` emotion** as the first new behavior. The Idle state currently calls a soft `eyeTriggerAttentive` as a placeholder — replace with `eyeTriggerLonely(...)` once the behavior file exists. The placeholder lives at `ctx.s.idle.lonely_fired` in `mode_idle.cpp`.
6. ⏳ **Real sensors.** Replace the stub bodies in `src/input/input_*.cpp`. Contract stays: write continuous values to `g_world`, push edge `Event`s, bump `g_world.last_interaction_ms` on any interaction signal (see *Wake-condition contract* above).

## Verification

Already verified at build time:

- ✅ **Compiles clean** on both `esp32s3` (Wokwi, ILI9341) and `esp32s3_gc9a01` (real-hardware GC9A01) environments.
- ✅ **Memory cost**: RAM +16 B, flash +1.0 KB vs. pre-refactor baseline. Comfortably under the 1 KB budget targeted in the plan.
- ✅ **State-trace logging**: `transition_to()` in `mode.cpp` prints `[mode] <from> -> <to> @ <ms>ms` on every transition. Exercise the stub face script in Wokwi to see the full graph.

Still to verify in sim / on hardware:

- **End-to-end transition trace.** Boot → DeepSleep, then `inputFacePoll` stub fires `FaceDetected` at T+8 s and `FaceLost` at T+12 s on a 25 s cycle. Expected log sequence on the first cycle: `Boot → DeepSleep → WakeUp → Idle → Interaction → Idle → Sleepy → DeepSleep`.
- **Latency check**: wrap `modeFsmTick()` in `micros()` (parallel to the existing `update/render/push` profiler in `main.cpp`). Expect <50 µs typical.
- **Display sleep**: confirm panel actually blanks in DeepSleep (not just frozen on the last frame) and re-lights cleanly on wake. Wokwi's TFT_eSPI driver honors `DISPOFF/SLPIN` so the sim is sufficient.
- **Extension check**: implementing the "lonely" emotion (step 5) should touch exactly the files listed in the worked example. If it doesn't, the layering is wrong — file a follow-up.

## Known gotcha: USB-CDC vs. Wokwi

The Wokwi sim (web and VS Code extension) hangs inside `HWCDC::begin()` when `-DARDUINO_USB_CDC_ON_BOOT=1` is set — the first USB-CDC ISR fire after `esp_intr_alloc()` never returns, the Interrupt Watchdog trips, and the panic handler then crashes trying to print through the same broken HWCDC. The `esp32s3` env therefore leaves USB-CDC off (Serial → UART0, which Wokwi simulates cleanly); `esp32s3_gc9a01` re-enables it for real hardware where USB-C is the only wire. Don't put `ARDUINO_USB_CDC_ON_BOOT=1` back into the Wokwi env without testing the VS Code extension specifically.

## What this design deliberately leaves out

- Refactoring any file under `src/anim/` — out of scope; it's already clean.
- `esp_light_sleep_start()`, mic-wake comparators, real battery ADC — v2 when hardware exists.
- Multi-core sensor sampling — single-thread is fine until profiling says otherwise.
- A general-purpose event bus or pub/sub framework — six states + a switch beats it.
