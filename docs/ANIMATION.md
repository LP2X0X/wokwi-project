# Procedural Eye Animation Guide

Deep reference for the animation system in `src/anim/`. The
[README](../README.md) covers project setup, asset pipeline, and how to build
+ run in Wokwi — this doc is just the animation internals.

## Design philosophy

One rule, applied everywhere:

> **Behaviors are independent layers. They never read each other directly.
> Emotions write into a shared `Modulators` bus; motion behaviors read it.
> A single `composePose()` step assembles the final `EyePose`. Rendering is
> a pure function of `(Eye geometry, EyePose)`.**

Concretely that means:

- **Parameter-driven, not frame-based.** Nothing is a sprite sheet. Every
  visible motion is a function `f(time, state) → numbers`.
- **Additive composition, no state machines.** Each emotion writes
  contributions into `Modulators`; motion behaviors apply them. There is no
  "isBlinking" or "isSleepy" branch anywhere.
- **Continuous transitions.** A behavior with `amount = 0` produces output
  identical to that behavior being absent — disable-able by easing a scalar.
- **Frame-rate independent easing.** All smoothing is done with
  `pos += (target − pos) · (1 − exp(−dt / τ))`, so 30 fps and 60 fps look
  identical.
- **Binocular pair, not two independent eyes.** A single `EyeState` drives
  both eyes (shared gaze + synchronized blink). Independent eyes look
  broken. Per-eye poses (wink, asymmetric expressions) are an explicit
  future extension; the data model is ready for it.
- **DAG, not graph.** Behaviors never read each other's state directly — the
  `Modulators` bus is the only coupling, so the system stays cycle-free.

These constraints are why adding sleepy mode took ~80 lines and zero
refactors of the existing blink / micro motion code, and why face-tracking
gaze landed as a single new field + a smoothing line in micro motion.

## File layout

```
src/anim/
  anim_util.h             # frand, urand, smoothstep01, lerp, clamp helpers
  eye_pose.h              # EyePose, Modulators, GazeIntent — data contracts
  eye_anim.h / .cpp       # public API + composePose() orchestrator
  eye_render.h / .cpp     # pure (Eye, EyePose) -> pixels renderer
  behaviors/
    sleepy.h / .cpp       # mood scalar -> Modulators (an "emotion")
    curiosity.h / .cpp    # wide-eye attention -> Modulators (an "emotion")
    attentive.h / .cpp    # "huh?" listening reaction -> Modulators (an "emotion")
    blink.h / .cpp        # blink schedule + lid_close (a "motion")
    micro_motion.h / .cpp # pupil jitter + gaze blending (a "motion")
    idle_gaze.h / .cpp    # autonomous "looking around" glances (a "motion")
```

Each behavior owns its own state struct, its own constants (private to the
.cpp), and exposes `xxxInit()` / `xxxUpdate()` / `xxxModulate()`. The
orchestrator is the only file that knows about the whole `EyeState`.

## Data model

The data model is split across three files: `eye_pose.h` for the contracts
shared between layers, behavior headers for each behavior's private state,
and `eye_anim.h` for the bag of bags that hosts them all.

### Contracts (eye_pose.h)

```cpp
// Final per-frame description that the renderer consumes. Pure pose -> pixels.
struct EyePose {
  float pupil_dx, pupil_dy;        // shared pupil offset, pixels (micro motion + sleepy bias)
  // Per-eye additive offsets on top of pupil_dx/dy. Idle gaze writes these
  // to inject tiny natural asymmetry (per-eye smoothing taus + small target
  // jitter); the renderer picks the matching side via Eye::side.
  float pupil_dx_l_extra, pupil_dy_l_extra;
  float pupil_dx_r_extra, pupil_dy_r_extra;
  float pupil_scale;               // 1.0 = baseline; <1.0 shrinks pupil (curiosity)
  float upper_lid_amount;          // 0..1 fraction of eye height (0 = open)
  float lower_lid_amount;          // 0..1
  float eye_open_amount;           // 1.0 baseline; >1.0 stretches sclera HEIGHT only (curiosity)
};

// Bus written by emotions, read by motion behaviors and composePose().
// Multipliers stack multiplicatively; offsets stack additively.
struct Modulators {
  float    drift_tau_mult;         // >1 = slower easing
  float    drift_range_mult;       // <1 = wander less far
  float    drift_hold_mult;        // >1 = hold each target longer
  float    blink_duration_mult;    // >1 = slower blink
  float    lid_upper_droop;        // additive lid offset (0..1 normalized)
  float    lid_lower_rise;         // additive lid offset (0..1 normalized)
  float    pupil_y_bias_px;        // additive pupil y bias
  float    pupil_scale_mult;       // pupil scale multiplier
  float    long_blink_chance;      // 0..1 probability per blink
  uint16_t long_blink_extra_min_ms;
  uint16_t long_blink_extra_max_ms;
  float    blink_inhibit;          // soft blink gate (attentive listening, ...)
  float    eye_open_add;           // additive on eye_open_amount baseline

  static Modulators neutral();     // every multiplier 1, every offset 0
};

// External gaze input — face detector / IMU / scripted scene.
// target_x/y are pupil-offset pixels (same units as drift).
// weight blends with random drift: 0 = ignored, 1 = full follow.
struct GazeIntent {
  bool  active;
  float target_x, target_y;
  float weight;
};
```

### Behavior states

Each behavior owns a small struct in its own header.

```cpp
// behaviors/micro_motion.h
struct MicroMotionState {
  float    drift_x, drift_y;       // current smoothed offset, pixels
  float    drift_tx, drift_ty;     // target the smoother is easing toward
  uint32_t drift_next_ms;          // when to roll a new target
};

// behaviors/blink.h
struct BlinkState {
  float    lid_close;              // 0 open .. 1 closed
  uint32_t blink_next_ms;          // start of next blink (when idle)
  uint32_t blink_start_ms;         // 0 if no blink in flight
  uint16_t blink_close_ms;         // close phase length (sampled per blink)
  uint16_t blink_hold_ms;          // hold length (sampled per blink)
  uint16_t blink_open_ms;          // open phase length (sampled per blink)
  bool     double_pending;         // queued double blink
};

// behaviors/sleepy.h
struct SleepyState {
  float    amount;                 // smoothed 0..1 sleepy scalar
  float    target;                 // what amount is easing toward
  uint32_t next_ms;                // autonomous mood re-roll
};

// behaviors/curiosity.h
struct CuriosityState {
  float    amount;                 // smoothed 0..1 curious scalar
  float    target;                 // what amount is easing toward
  uint32_t hold_until_ms;          // when active trigger ends (drops target to 0)
  uint32_t next_autoroll_ms;       // autonomous trigger check
};

// behaviors/idle_gaze.h
struct IdleGazeState {
  enum Phase : uint8_t { Holding = 0, Moving = 1 };
  Phase    phase;
  uint32_t phase_until_ms;         // when current phase ends
  // Per-eye smoothed offsets + per-eye targets + per-eye taus —
  // identical schedule across both eyes, tiny asymmetry in HOW they
  // get to the same target.
  float    gx_l, gy_l, gx_r, gy_r;
  float    tx_l, ty_l, tx_r, ty_r;
  float    tau_l, tau_r;
};

// behaviors/attentive.h
struct AttentiveState {
  enum Phase : uint8_t { Idle, Freeze, Glance, Hold, Verify, Relax };
  Phase    phase;
  uint32_t phase_until_ms;         // when current phase ends
  uint32_t end_ms;                 // when to drop into Relax mid-phase
  bool     verify_done;            // we only do ONE verify per activation
  float    amount, target_amount;  // smoothed scalar drives modulators
  float    dir_x, dir_y;           // commanded direction (pixels)
  // Smoothed shared pupil offset, read directly by composePose() and added
  // to pose.pupil_dx/dy. Same pattern as idle_gaze's per-eye extras — the
  // Modulators bus is for INFLUENCE, this is direct spatial output.
  float    gaze_x,  gaze_y;
  float    gaze_tx, gaze_ty;
};
```

### Container (eye_anim.h)

```cpp
struct EyeState {
  MicroMotionState micro;
  BlinkState       blink;
  SleepyState      sleepy;
  CuriosityState   curiosity;
  IdleGazeState    idle_gaze;
  AttentiveState   attentive;
  GazeIntent       gaze;
  EyePose          pose;           // composed output of last update
  uint32_t         last_update_ms;
};
```

The renderer reads only `s.pose`. Behaviors read only their own sub-struct
+ `Modulators` + `GazeIntent`. The orchestrator is the only thing that
touches the whole `EyeState`.

## Update pipeline

```
eyeStateUpdate(now):
    dt = clamp(now - last_update_ms, 0, 100ms)

    # Pass 1: emotions update internal scalars.
    sleepyUpdate   (s.sleepy,    now, dt)
    curiosityUpdate(s.curiosity, now, dt)
    attentiveUpdate(s.attentive, now, dt)
    # happyUpdate(s.happy, now, dt)         # future

    # Pass 2: emotions write into a fresh Modulators bus.
    mods = Modulators::neutral()
    sleepyModulate   (s.sleepy,    mods)
    curiosityModulate(s.curiosity, mods)
    attentiveModulate(s.attentive, mods)
    # happyModulate(s.happy, mods)          # future

    # Pass 3: motion behaviors consume modulators + gaze.
    microMotionUpdate(s.micro,     mods, s.gaze, now, dt)
    blinkUpdate      (s.blink,     mods, now)
    idleGazeUpdate   (s.idle_gaze, mods, s.gaze, now, dt)

    # Pass 4: assemble the final pose.
    composePose(s, mods, s.pose)
```

The strict order is what keeps the system a DAG: emotions never read motion
state, motion behaviors never read each other, and `composePose()` is the
single bridge between behavior states and the renderer.

## composePose()

The single function that assembles the final pose:

```cpp
void composePose(const EyeState &s, const Modulators &mods, EyePose &out) {
  // Shared pupil offset (both eyes follow this). Attentive contributes a
  // smoothed directional offset; per-eye asymmetry still comes from
  // idle_gaze extras below.
  out.pupil_dx    = s.micro.drift_x + s.attentive.gaze_x;
  out.pupil_dy    = s.micro.drift_y + mods.pupil_y_bias_px + s.attentive.gaze_y;
  out.pupil_scale = mods.pupil_scale_mult;

  // Per-eye additive offsets — idle gaze writes these to inject subtle
  // asymmetry. Renderer picks the side via Eye::side at draw time.
  out.pupil_dx_l_extra = s.idle_gaze.gx_l;
  out.pupil_dy_l_extra = s.idle_gaze.gy_l;
  out.pupil_dx_r_extra = s.idle_gaze.gx_r;
  out.pupil_dy_r_extra = s.idle_gaze.gy_r;

  // Lid amounts (normalized; renderer scales by per-eye height)
  float upper = s.blink.lid_close + mods.lid_upper_droop;
  float lower = s.blink.lid_close * blink::LOWER_LID_TRAVEL_FRACTION
                + mods.lid_lower_rise;
  out.upper_lid_amount = clamp01(upper);
  out.lower_lid_amount = clamp01(lower);

  // Sclera Y-stretch (curiosity raises this above 1.0; future squint < 1.0)
  out.eye_open_amount = 1.0f + mods.eye_open_add;
}
```

When you ask "why is the eye drawn the way it is?", this is the only place
to look. Behaviors write into structs; rendering reads only `EyePose`;
`composePose` is the bridge.

## Per-layer math

### Micro motion (motion behavior)

Picks a new random drift target every 0.5–1.8 s and exponentially eases
toward it. Optionally blends in an external gaze target.

```
target re-roll: every random(DRIFT_HOLD_MIN_MS, DRIFT_HOLD_MAX_MS)
                       · mods.drift_hold_mult
target value:   uniform(±DRIFT_RANGE_PX · mods.drift_range_mult)

# Blend with gaze if active:
if gaze.active and gaze.weight > 0:
    effective_target = lerp(random_target, gaze_target, gaze.weight)
else:
    effective_target = random_target

easing:         pos += (effective_target − pos) · (1 − exp(−dt / τ))
                τ  = DRIFT_TAU_S · mods.drift_tau_mult
```

Critically-damped exponential easing — never overshoots, frame-rate
independent. The same smoother is used for autonomous drift and for gaze
follow, so a noisy face-detection signal becomes organic motion for free.

### Blink (motion behavior)

Three-phase asymmetric `smoothstep` curve in `[0, 1]`:

```
blinkCurve(t):
    0 .. close_ms      : smoothstep(t / close_ms)               # ease 0 → 1
    + .. hold_ms       : 1                                       # full closure
    + .. open_ms       : 1 − smoothstep((t − close − hold) / open)  # ease 1 → 0
```

Default phase mix: 90 ms close → 40 ms hold → 160 ms open. The asymmetry
(close fast, open slower) is the single biggest tell between "alive" and
"robotic".

Phase lengths are sampled **once at the start of each blink**, scaled by
`mods.blink_duration_mult`, so each blink stays consistent even if a slow
emotion modulator wobbles mid-blink.

**Double blinks** — after ~18% of blinks, the next blink is scheduled
90–180 ms later, producing a natural "blink-blink" occasionally.

**Long blink** (driven by emotion modulators) — at blink start, with
probability `mods.long_blink_chance`, an extra
`urand(long_blink_extra_min_ms, long_blink_extra_max_ms)` is added to
`blink_hold_ms`. Awake stack has chance == 0; sleepy contributes
`amount · 30%` and a 300–900 ms range.

**Soft blink inhibit** — when `mods.blink_inhibit > 0.3` (attentive
listening, future "focused stare" emotions, ...), a queued blink defers
itself by 500 ms and re-checks instead of firing. When the inhibit fades
to below the threshold, blinks resume on their normal schedule — so this
is a "pause and resume" rather than a hard veto. The first blink lands
within ~500 ms of the inhibit dropping.

### Idle gaze (motion behavior)

Where micro motion provides the constant 1-px jitter that keeps eyes from
looking dead, idle gaze schedules occasional **larger glances with long
quiet pauses** — the difference between vibration and attention. They
compose additively in the renderer, so blinking and sleepy lids stay in
charge of their own layers; idle gaze only ever moves the pupils.

Two-phase state machine:

| phase   | what's happening                                                                                     |
|---------|------------------------------------------------------------------------------------------------------|
| HOLDING | Target sits still; smoother eases the eye onto it; long pause (1.5–4.5 s before sleepy modulators). |
| MOVING  | A new target was picked; smoother is in flight; short window (0.28–1.1 s before sleepy modulators). |

Target picking is weighted so most movement reads as a subtle drift and
only rarely as a definite look:

| bucket  | probability | horizontal range | vertical (× weight)  |
|---------|------------:|-----------------:|----------------------|
| small   |         60% | ±1.5 px          | × 0.4 — mostly flat  |
| medium  |         30% | ±3.5 px          | × 0.5 — slight bias  |
| large   |         10% | ±6.0 px          | × 0.7 — real glances |

**Asymmetry (very subtle on purpose):**

1. Per-eye **target jitter** (±`ASYMMETRY_PX`, default 0.45 px) — both
   eyes share a chosen point but each side gets a sub-pixel offset.
2. Per-eye **smoothing tau** (±`ASYMMETRY_TAU_FRAC`, default 18%) — eyes
   arrive at the target a beat apart. At rest both converge to the same
   point and asymmetry visually disappears.

Both eyes share the **same schedule** (real eyes are coordinated); the
asymmetry is in HOW they get there, not whether they move.

**Reads from Modulators:**

| field             | effect on idle gaze                                  |
|-------------------|------------------------------------------------------|
| `drift_range_mult`| Sleepy / curious shrinks the glance amplitude.       |
| `drift_hold_mult` | Sleepy / curious stretches the HOLDING duration.    |
| `drift_tau_mult`  | Sleepy slows, curious snaps — same knob, both ways.  |

**Reads from GazeIntent:** if `gaze.active && gaze.weight > 0.5`, idle
behavior fades its offset toward 0 (freezing the phase machine) so the
external signal becomes the dominant gaze source. When the override
releases, the next phase tick resumes.

### Sleepy mode (emotion behavior)

Owns one scalar `amount ∈ [0, 1]`. Its `update()` evolves the scalar; its
`modulate()` writes contributions into `Modulators`.

**Autonomous mood drift** (re-rolled every 8–25 s):

| bucket    | probability | resulting `target` |
|-----------|------------:|---------------------------|
| `< 0.55`  |         55% | uniform `[0.00, 0.20]` — bright |
| `< 0.90`  |         35% | uniform `[0.20, 0.55]` — drowsy |
| `≥ 0.90`  |         10% | uniform `[0.65, 1.00]` — very sleepy |

**Easing** toward the target with `SLEEPY_TAU_S ≈ 1.5 s` so transitions
feel like a slow mood change, not a switch.

**External hook** — call `eyeStateSetSleepy(state, target)` from any sensor
(ambient light, time-of-day, touch) to nudge `target`. The autonomous
re-roll inside `sleepyUpdate()` will eventually overwrite your value, so
call it on every tick if you want it to stick.

#### What sleepy contributes to Modulators

Everything scales linearly with `amount`. At `amount = 0`, `sleepyModulate`
is a no-op, so the animation is identical to fully awake.

| Modulator field             | Contribution at full sleepy                                    |
|-----------------------------|----------------------------------------------------------------|
| `drift_tau_mult` (×=)       | `× (1 + a · 1.5)` — heavier feel, never locks up               |
| `drift_range_mult` (×=)     | `× (1 − a · 0.5)` — wanders less far                           |
| `drift_hold_mult` (×=)      | `× (1 + a · 1.5)` — stays put longer between targets           |
| `blink_duration_mult` (×=)  | `× (1 + a · 1.5)` — close + hold + open all stretched          |
| `lid_upper_droop` (+=)      | `+ a · 0.4` (fraction of eye height) — upper lid sags          |
| `lid_lower_rise` (+=)       | `+ a · 0.2` — lower lid puffs                                  |
| `pupil_y_bias_px` (+=)      | `+ a · 1.5 px` downward, on top of micro-motion drift          |
| `long_blink_chance` (+=)    | `+ a · 0.3` per-blink probability                              |
| `long_blink_extra_*_ms`     | 300–900 ms extra hold range                                    |

Two key properties that come for free from this design:

1. **The lid arc curvature still works.** The unified perspective rule
   `curvature = (centerline_y − eye_cy) / b` means a sleepy droop alone
   (no blink) automatically gets a soft ∩ shape, and combined with a
   partial blink the curvature smoothly slides toward ∪ as the lid passes
   the eye center. No special cases needed.
2. **Micro motion never stops.** Drift and pupil bias are independent
   contributions to the same final position. Even at `amount = 1` the
   pupil keeps wandering — just slowly, in a smaller range, around a
   slightly lower center.

Mood reference:

```
amount = 0.0     awake — identical to disabled
       = 0.3     light drowsy — small lid sag, pupil sinks, drift slightly slower
       = 0.7     visibly sleepy — heavy lids, slow blinks, occasional long holds
       = 1.0     "Ghibli soot creature about to nap" — barely-open eyes, big slow blinks
```

### Curiosity (emotion behavior)

A "wide-eye, focused attention" expression. When active the character
looks engaged — sclera stretches vertically, lids retract toward fully
open, pupil shrinks slightly, gaze focus tightens (smaller drift range,
longer holds, snappier easing). Reads like a tiny creature noticing
something interesting; not anime shock, not robotic alert.

Same shape as sleepy:

- Owns one scalar `amount ∈ [0, 1]` plus a target + hold timer.
- `update()` evolves amount with **asymmetric tau** — fast on the way up
  (`TAU_IN_S ≈ 0.10 s`), softer on the way down (`TAU_OUT_S ≈ 0.50 s`).
  Noticing snaps; relaxing relaxes.
- `modulate()` writes contributions into `Modulators`; never touches
  any other behavior directly.

**Two trigger paths:**

1. **Autonomous** — every 8–25 s a re-roll checks `AUTO_TRIGGER_CHANCE`
   (default ~30%). On fire: random intensity 0.55–0.95, random duration
   1.3–1.7 s. Keeps the toy feeling alive even with no external input.
2. **External** — call `eyeTriggerCuriosity(state, intensity, duration_ms)`
   from any sensor / scripted scene / future AI hook. Re-callable any
   time; the smoother retargets without hard switching.

```cpp
// Public API on eye_anim.h
void eyeTriggerCuriosity(EyeState &s, float intensity, uint32_t duration_ms);

// Typical wiring:
if (touch_pin.fell())            eyeTriggerCuriosity(eyes, 0.8f, 1500);
if (sound_amp > THRESHOLD)       eyeTriggerCuriosity(eyes, 0.9f, 1200);
if (face_detector.new_face())    eyeTriggerCuriosity(eyes, 0.7f, 2000);
```

**Timing profile** at `intensity = 0.8`, `duration_ms = 1500`:

```
amount
 0.8 ┤      ┌──────────────┐
     │     /                \____
 0.0 ┤___/                        \____
     └─────────────────────────────────→ time
       ramp ~0.3 s   hold ~1.0 s    decay ~0.5 s
```

**What curiosity contributes to Modulators** (linear in `amount`, no-op at 0):

| Modulator field             | Contribution at full curiosity                                  |
|-----------------------------|-----------------------------------------------------------------|
| `eye_open_add` (+=)         | `+ a · EYE_OPEN_BOOST` — sclera HEIGHT multiplier (Y-only)      |
| `pupil_scale_mult` (×=)     | `× (1 − a · PUPIL_SHRINK)` — slightly smaller pupil             |
| `lid_upper_droop` (-=)      | `− a · 0.30` — retracts upper lid toward fully open             |
| `lid_lower_rise` (-=)       | `− a · 0.15` — retracts lower lid                               |
| `drift_range_mult` (×=)     | `× (1 − a · 0.30)` — tighter focus                              |
| `drift_hold_mult` (×=)      | `× (1 + a · 1.50)` — longer holds on target                     |
| `drift_tau_mult` (×=)       | `× (1 − a · 0.15)` — slightly snappier easing                   |

**Side effects worth knowing:**

- **Blink attenuation.** During peak curiosity, `lid_close = 1` plus
  `lid_upper_droop = −0.3` clamps to 0.7 → blinks visibly close to ~70%
  instead of 100%. Reads as "alert creature barely blinks" — feature,
  not bug.
- **Sleepy + curious.** Lid retract neutralizes sleepy droop additively.
  Drift hold stacks (sleepy ×2.5 × curious ×2.5 = 6.25× baseline) so the
  combo reads as "drowsy but suddenly attentive on something."

### Attentive / Listening (emotion behavior)

A "huh? did I hear something?" reaction. Externally triggered (sound,
touch, face appearance, finger snap) with a direction toward the
suspected source. The eyes run a small phase sequence —
**freeze → glance → hold → verify → relax** — while writing modulator
contributions for eye widening, pupil shrink, lid retract, drift focus,
and soft blink suppression.

Unlike sleepy / curiosity, attentive has **no autonomous trigger** —
curiosity already handles spontaneous noticing, and tying attentive to a
timer would just compete for the same animation slot. Attentive is
purely sensor-driven.

**Phase sequence** (typical 1.5–2 s activation):

| phase  | typical duration | gaze target                     | what reads on screen          |
|--------|------------------|---------------------------------|-------------------------------|
| Freeze | 80–180 ms        | 0 (no movement)                 | brief pause — "what was that?" |
| Glance | 180–280 ms       | direction (full)                | snappy dart toward source     |
| Hold   | remainder        | direction (full)                | listening — wider, focused    |
| Verify | 220–360 ms       | direction × 0.55 + ≤ 1.4 px jitter | tiny double-check glance     |
| Hold   | until end_ms     | direction (full)                | resumes listening             |
| Relax  | 500–750 ms       | 0 (eases back)                  | settles to baseline           |

Verify fires automatically once per activation, scheduled randomly
within the first half of the hold. It's skipped on activations too short
to fit a clean verify cycle (< ~600 ms of hold remaining).

**External trigger:**

```cpp
void eyeTriggerAttentive(EyeState &s,
                         float intensity, uint32_t duration_ms,
                         float direction_x, float direction_y);

// Typical wiring:
if (mic_right_peak)         eyeTriggerAttentive(eyes, 0.7f, 1800,  1.0f, 0.0f);
if (touch_top_of_head)      eyeTriggerAttentive(eyes, 0.6f, 1500,  0.0f,-1.0f);
if (face_just_appeared)     eyeTriggerAttentive(eyes, 0.5f, 2000,  0.0f, 0.0f);
if (finger_snap_detected)   eyeTriggerAttentive(eyes, 0.9f, 1200,  0.7f,-0.3f);
```

`direction_x` / `direction_y` are normalized `[-1, +1]` (negative = left
/ up, positive = right / down). Scaled internally to **±5 px horizontal,
±3 px vertical** — real eye movement is more horizontal than vertical so
a "look down" reaction is more subtle than a "look right." Intensity
scales the FACIAL expression only; the gaze movement is full-magnitude
even at low intensity (a quiet listening read still snaps the pupils
toward the source).

Re-callable mid-sequence: the smoother retargets without re-doing the
freeze beat, so a burst of close-together sound events reads as ONE
sustained listening moment rather than several twitches.

**Smoothing:**

- `amount` uses asymmetric tau — `TAU_IN_S = 0.12 s` (fast attack, "huh?"
  reads instantly), `TAU_OUT_S = 0.40 s` (soft release, relaxes rather
  than snaps back).
- Gaze offset uses two phase-dependent taus —
  `TAU_GAZE_SNAP_S = 0.08 s` during Glance / Verify (animal-quick darts),
  `TAU_GAZE_HOLD_S = 0.30 s` during Hold (steady, settled).

**What attentive contributes to Modulators** (linear in `amount`, no-op at 0):

| Modulator field             | Contribution at full attentive                                    |
|-----------------------------|-------------------------------------------------------------------|
| `eye_open_add` (+=)         | `+ a · 0.20` — eyes slightly wider                                |
| `pupil_scale_mult` (×=)     | `× (1 − a · 0.18)` — slightly smaller pupil                       |
| `lid_upper_droop` (-=)      | `− a · 0.15` — small lid retract (cancels mild sleepy droop)      |
| `drift_range_mult` (×=)     | `× (1 − a · 0.60)` — narrower wandering (focused listening)       |
| `drift_tau_mult` (×=)       | `× (1 + a · 0.20)` — drift slightly smoother                      |
| `drift_hold_mult` (×=)      | `× (1 + a · 1.50)` — long holds on target                         |
| `blink_inhibit` (+=)        | `+ a · 1.00` — soft blink gate (see Blink section)                |

**Gaze offset (not via Modulators)** — attentive writes its smoothed
directional output (`s.attentive.gaze_x` / `gaze_y`) directly into
`composePose()`'s pupil offset. The Modulators bus is for INFLUENCE on
existing knobs; spatial state goes through state structs the way
idle_gaze does for per-eye extras.

**Composition with other behaviors:**

- `sleepy + attentive` — `lid_upper_droop` is additive, so attentive's
  `−0.15` first cancels mild sleepy droop and then pushes the lid
  slightly open. Reads as "drowsy creature suddenly alert at something."
- `curiosity + attentive` — both contribute to widening / pupil shrink /
  drift focus; meant for "noticed AND attentive" moments triggered
  together. Lid retract sums cleanly (curiosity `−0.30` + attentive
  `−0.15` = `−0.45`); the `composePose()` clamp keeps it sane.
- `idle_gaze` — keeps running; its drift_range × 0.4 and drift_hold ×
  2.5 effectively pin its wandering. Per-eye extras still apply so the
  pupils have tiny independent asymmetry even during the listening hold.
- `micro_motion` — same treatment; wobble narrows but never freezes,
  reading as "alive but holding still."
- `blink` — soft-gated by `blink_inhibit` (described in the Blink
  section).

### Gaze input (external)

Not a behavior — just a struct populated from outside. The micro motion
smoother does the work.

```cpp
// every frame from your face detector / IMU / scripted scene:
eyeSetGazeTarget(eyes, face_dx_px, face_dy_px, /*weight=*/1.0f);

// or to disable:
eyeClearGazeTarget(eyes);
```

`weight` blends between random drift and the gaze target (see Micro motion
math above). Use `0.5` for "gaze-biased wander" or `1.0` for "lock onto
the face but stay smooth". Even a noisy detection signal turns into
organic eye follow because micro motion's smoother runs unchanged.

## Eye shapes & render pipeline

### Eye geometry (procedural circles, no bitmaps)

The sclera and pupil are both **procedural circles** — no sprite assets,
no bitmap files. Each Eye describes its geometry in absolute screen
pixels:

```cpp
struct Eye {
  // Sclera (white area) as a circle. Curiosity stretches it vertically
  // into an ellipse at draw time; width never changes.
  int16_t sclera_cx, sclera_cy;
  int16_t sclera_r;

  // Pupil neutral offset from sclera center (the "iris sits here" bias —
  // e.g. slight inward tilt for the susuwatari look). Animated drift
  // adds on top of this, multiplied by `scale`.
  int16_t pupil_dx_neutral;
  int16_t pupil_dy_neutral;
  int16_t pupil_r;

  // Animation amplitude multiplier (NOT applied to radii). 1.0 = source
  // units, 2.0 = doubled drift range. Lets behaviors stay tuned in
  // "source pixel" units while different displays render at different
  // visual sizes.
  float   scale;
  EyeSide side;       // Left / Right — picks per-eye pose extras at draw
};
```

The fur-cutout build wires two physical OLEDs (one eye each) with a
sclera radius larger than the screen — the circle deliberately overflows
the panel, and the cutout in the fur absorbs the overflow so the visible
portion fills the eye hole. The wokwi preview screen shows both eyes at
native size on one panel.

In `main.cpp`:

```cpp
// Physical: sclera overflows the screen by (R − 64) px horizontally and
// (R − 32) px vertically.
constexpr int16_t PHYS_SCLERA_R = 72;        // diameter 144 — fills + spills
constexpr int16_t PHYS_PUPIL_R  = 16;        // ratio ≈ 1/4.5 of sclera diameter
constexpr float   PHYS_SCALE    = 2.0f;      // doubles ANIMATION amplitude only
```

### Render pipeline

```
renderEyes(d, left, right, pose):
    clearDisplay()
    renderEye(d, left,  pose)
    renderEye(d, right, pose)
    display()

renderEye(d, eye, pose):
    # Three scale knobs sit on top of the static circle geometry:
    #   pose.eye_open_amount → stretches sclera HEIGHT only (curiosity peak
    #                          ~2.0 = ellipse). Width never changes.
    #   pose.pupil_scale     → uniform multiplier on the pupil radius.
    #   eye.scale            → per-display zoom for the ANIMATED drift only.
    sclera_rx = eye.sclera_r
    sclera_ry = eye.sclera_r * pose.eye_open_amount

    fillEllipse(d, eye.sclera_cx, eye.sclera_cy,
                sclera_rx, sclera_ry, WHITE)                            # 1

    # Pupil position: sclera center + neutral offset + scaled drift.
    # Pick per-eye additive offset based on Eye::side.
    extra_x = (eye.side == Left) ? pose.pupil_dx_l_extra : pose.pupil_dx_r_extra
    extra_y = (eye.side == Left) ? pose.pupil_dy_l_extra : pose.pupil_dy_r_extra
    pupil_cx = eye.sclera_cx + eye.pupil_dx_neutral
               + (pose.pupil_dx + extra_x) * eye.scale
    pupil_cy = eye.sclera_cy + eye.pupil_dy_neutral
               + (pose.pupil_dy + extra_y) * eye.scale
    pupil_r  = eye.pupil_r * pose.pupil_scale
    fillCircle(d, pupil_cx, pupil_cy, pupil_r, BLACK)                   # 2 (hole)

    # Eyelid bbox follows the (possibly stretched) sclera ellipse. We
    # use 2*r + 1 (NOT 2*r) so the loop covers the inclusive [-r, +r]
    # range that fillCircle / fillEllipse paint — otherwise a 1-px
    # sliver of sclera stays visible at the lateral edges during a full
    # blink (the parabolic bow goes to 0 at xn = ±1).
    bbox_w = 2 * sclera_rx + 1
    bbox_h = 2 * sclera_ry + 1
    drawCurvedLid(d, bbox, upper_lid_y, curvature, fill_from_top=true)  # 3
    drawCurvedLid(d, bbox, lower_lid_y, curvature, fill_from_top=false) # 4
```

`fillEllipse` is a scanline implementation built on top of
`drawFastHLine`; when `rx == ry` it dispatches to `Adafruit_GFX::fillCircle`
which uses the midpoint algorithm and is faster. Off-screen pixels clip
silently so circles that overflow the panel work without any explicit
bounds checks.

`Eye::side` (`Left` / `Right`) decides which `pupil_d*_extra` the
renderer reads — this is how the same `EyePose` produces visibly
different positions on the two physical OLEDs (idle gaze's per-eye
asymmetry).

The renderer cannot observe `lid_close`, `sleepy_amount`, drift state,
or any other behavior internal — only the composed pose. Replacing the
renderer (LCD with grayscale, host-side simulator) means re-implementing
this one function; behaviors don't change.

### Lid curvature math (spherical perspective)

Both lids share one helper (`drawCurvedLid`) and one perspective rule:

```
bow_curvature = (lid_centerline_y − eye_center_y) / sclera_ry
y_lid(x)      = lid_centerline_y + bow_curvature · max_dip · (1 − x_norm²)
where x_norm  = (x − eye_center_x) / sclera_rx   ∈ [-1, +1]
      max_dip = 2 · sclera_ry · LID_CURVE_FRACTION
```

The sign of `bow_curvature`:

| centerline position    | `bow_curvature` | shape |
|------------------------|----------------:|-------|
| above eye center       |             < 0 | **∩** "wrapping over the top" |
| at eye center          |               0 | flat line |
| below eye center       |             > 0 | **∪** "wrapping under the bottom" |

The two lids differ only in:

- **Direction of fill** — upper lid paints BLACK from the top of the eye
  down to the lid line; lower lid paints from the lid line down to the
  bottom of the eye.
- **Travel range** — upper lid fully closes the eye when `lid_close = 1`;
  the lower lid only travels `LOWER_LID_TRAVEL_FRACTION` of that under
  blink alone (real eyes blink mostly with the upper lid). Emotion
  contributions to upper / lower are independent, so a wince behavior
  could push the lower lid up without changing the upper.

Pixels in the bbox that fall outside the eye silhouette were never lit, so
painting BLACK over them is a no-op — the visible lid edge is naturally
clipped to the eye shape. No silhouette mask needed.

## Tuning knobs

Each behavior owns its own constants in its own `.cpp`, kept in an
anonymous namespace. Open the file you want to tune:

| File                                     | Knobs                                              |
|------------------------------------------|----------------------------------------------------|
| `behaviors/micro_motion.cpp`             | `DRIFT_RANGE_PX`, `DRIFT_TAU_S`, `DRIFT_HOLD_*`    |
| `behaviors/blink.cpp`                    | `BLINK_GAP_*`, `BLINK_CLOSE/HOLD/OPEN_MS`, `DOUBLE_BLINK_PCT`, `DOUBLE_GAP_*` |
| `behaviors/blink.h`                      | `LOWER_LID_TRAVEL_FRACTION`                        |
| `behaviors/sleepy.cpp`                   | `SLEEPY_*` (tau, hold, droop fractions, drift mults, blink mult, long-blink) |
| `behaviors/curiosity.cpp`                | `TAU_IN/OUT_S`, `EYE_OPEN_BOOST`, `PUPIL_SHRINK`, `LID_UPPER/LOWER_RETRACT`, `DRIFT_*` |
| `behaviors/idle_gaze.cpp`                | `SMALL/MEDIUM/LARGE_RANGE_PX`, `HOLD_*`, `MOVE_*`, `TAU_MIN/MAX_S`, `ASYMMETRY_*` |
| `behaviors/attentive.cpp`                | `TAU_*_S`, `FREEZE/GLANCE/VERIFY/RELAX_*_MS`, `DIR_RANGE_PX_*`, `EYE_OPEN_BOOST`, `PUPIL_SHRINK`, `DRIFT_*`, `BLINK_INHIBIT_AT_PEAK` |
| `eye_render.cpp`                         | `LID_CURVE_FRACTION` (rendering knob)              |
| `main.cpp`                               | `PHYS_SCLERA_R`, `PHYS_PUPIL_R`, `PHYS_SCALE` (physical eye geometry) |

Defaults:

| Constant                        | Default      | Effect                                                                                                               |
|---------------------------------|--------------|----------------------------------------------------------------------------------------------------------------------|
| `DRIFT_RANGE_PX`                | 2.0 px       | Peak pupil offset                                                                                                    |
| `DRIFT_TAU_S`                   | 0.35 s       | Smoothing tau (larger → lazier)                                                                                      |
| `DRIFT_HOLD_MIN/MAX_MS`         | 500 / 1800   | How long a drift target is held                                                                                      |
| `BLINK_GAP_MIN/MAX_MS`          | 2200 / 5800  | Time between blinks                                                                                                  |
| `BLINK_CLOSE/HOLD/OPEN`         | 90 / 40 / 160| Per-phase blink duration in ms                                                                                       |
| `DOUBLE_BLINK_PCT`              | 18           | % chance a blink is followed by a 2nd                                                                                |
| `LID_CURVE_FRACTION`            | 0.22         | Eyelid arc bow (∩/∪) as fraction of eye height. 0 = flat shutter; ~0.3 = very domed. Shared by upper and lower lids. |
| `LOWER_LID_TRAVEL_FRACTION`     | 0.30         | How far the lower lid rises relative to the upper lid's full travel during a blink.                                  |
| `SLEEPY_TAU_S`                  | 1.5          | Easing tau for sleepy `amount` itself.                                                                               |
| `SLEEPY_HOLD_MIN/MAX_MS`        | 8000 / 25000 | Autonomous re-roll cadence for sleepy `target`.                                                                      |
| `SLEEPY_UPPER_DROOP_FRACTION`   | 0.4          | Upper lid sag at full sleepy.                                                                                        |
| `SLEEPY_LOWER_RISE_FRACTION`    | 0.2          | Lower lid puff at full sleepy.                                                                                       |
| `SLEEPY_PUPIL_BIAS_PX`          | 1.5          | Downward pupil bias at full sleepy.                                                                                  |
| `SLEEPY_DRIFT_TAU_MULT`         | 1.5          | Drift τ slowdown at full sleepy.                                                                                     |
| `SLEEPY_DRIFT_RANGE_REDUCTION`  | 0.5          | Drift range shrinkage at full sleepy.                                                                                |
| `SLEEPY_DRIFT_HOLD_MULT`        | 1.5          | Drift hold extension at full sleepy.                                                                                 |
| `SLEEPY_BLINK_DURATION_MULT`    | 1.5          | Blink slowdown at full sleepy.                                                                                       |
| `SLEEPY_LONG_BLINK_PCT_AT_FULL` | 30           | Max % chance per blink of an extra-long sleepy hold.                                                                 |
| `PHYS_SCLERA_R`                 | 72           | Physical sclera radius — diameter 144 px overflows the 128×64 screen by 8 px per side (fur cutout absorbs the rest). |
| `PHYS_PUPIL_R`                  | 16           | Physical pupil radius. Ratio `PHYS_PUPIL_R / PHYS_SCLERA_R ≈ 1/4.5` matches the Ghibli-reference pupil size.          |
| Attentive `TAU_IN/OUT_S`        | 0.12 / 0.40  | Asymmetric tau on `amount` — snappy "huh?" attack, soft release.                                                     |
| Attentive `TAU_GAZE_SNAP_S`     | 0.08         | Gaze tau during Glance / Verify (animal-quick darts).                                                                |
| Attentive `TAU_GAZE_HOLD_S`     | 0.30         | Gaze tau during Hold (settled).                                                                                       |
| Attentive `DIR_RANGE_PX_X/Y`    | 5.0 / 3.0    | Pixel range of the directional pupil offset (horizontal > vertical, like real eyes).                                  |

## Extending it

Two patterns, each ~50 lines.

### Adding a new emotion (recommended pattern)

Sleepy is the canonical example. Copy `behaviors/sleepy.{h,cpp}` and edit
in place:

1. Drop a new state struct (`SurpriseState`, `HappyState`, ...) into the
   header. One scalar per emotion is usually enough.
2. Implement `xxxUpdate(state, now, dt)` — autonomous behavior, decay, or
   external setter.
3. Implement `xxxModulate(const xxxState&, Modulators&)` — pure function
   from internal state to bus contributions. Multiplicative for rates,
   additive for spatial offsets, no branching elsewhere.
4. Add a member to `EyeState` and call init / update / modulate from
   `eye_anim.cpp`'s pipeline. The existing four passes don't change.

```cpp
// behaviors/surprise.h (sketch)
struct SurpriseState {
  float    amount;       // 0..1 surprised
  uint32_t decay_after_ms;
};
void surpriseInit    (SurpriseState&, uint32_t now);
void surpriseTrigger (SurpriseState&, uint32_t now);   // event hook
void surpriseUpdate  (SurpriseState&, uint32_t now, float dt);
void surpriseModulate(const SurpriseState&, Modulators&);

// surprise.cpp -> contributes:
//   pupil_scale_mult *= 1 + a · 0.4   (dilated)
//   eye_open_add    +=     a · 0.3    (wider eyes)
//   blink_duration_mult *= 1 - a · 0.3 (snappy blinks)
//   drift_tau_mult     *= 1 - a · 0.5 (twitchier)
```

Stacks for free with sleepy. Happy + tired? Both contribute, modulators
combine.

### Adding a new motion behavior

Used when you need a new "live" output (e.g. micro-saccades, breathing
pupil pulse). Same shape as `micro_motion` / `blink`:

1. Add a state struct + `xxxUpdate(state, mods, ..., now, dt)`.
2. Decide where its output lands in the pose. If it's a brand-new field,
   add it to `EyePose` and read it in the renderer; if it's modifying an
   existing field, add a `Modulators` field for additive composition (the
   way `pupil_y_bias_px` works).
3. Wire init + update into `eye_anim.cpp`.

### Adding gaze tracking (already supported)

```cpp
// every frame from a face detector / IMU / scripted scene:
eyeSetGazeTarget(eyes, face_dx_px, face_dy_px, /*weight=*/1.0f);
eyeStateUpdate(eyes, millis());
renderEyes(display, kLeftEye, kRightEye, eyes);
```

`face_dx`, `face_dy` should be in the same coordinate space as drift —
i.e. pixels of pupil offset from neutral. A face centered in the camera
frame should produce `(0, 0)`; a face to the left produces a negative `dx`.

Use `weight = 0.5` for "follow but still wander", `weight = 1.0` for
"locked on with smoothing". Set `eyeClearGazeTarget()` to fall back to
pure micro motion.

### Other recipes

| Behavior       | Pattern                                                                                                                                           |
|----------------|---------------------------------------------------------------------------------------------------------------------------------------------------|
| Wink           | Per-eye pose. Add `EyePose poseL, poseR` to `EyeState`, have `composePose()` write both, add a `WinkState` that biases one side.                  |
| Surprise       | Emotion. `pupil_scale_mult` (dilation), `eye_open_add` (wider), snap-faster blinks via `blink_duration_mult < 1`. Quicker decay than curiosity.   |
| Sad / scared   | Emotion. Inverse of curiosity for lids (additive droop), pair with a downward `pupil_y_bias_px`. Triggered or autonomous.                         |
| Per-eye droop  | Move `SLEEPY_*_FRACTION` constants onto `Eye`, sleepy reads them. Or split sleepy into per-eye amounts.                                           |
| Scripted scene | Build a tiny scenario sequencer that calls `eyeStateSetSleepy` / `eyeTriggerCuriosity` / `eyeTriggerAttentive` / `eyeSetGazeTarget` on a timeline. |

## Dev / test tips

### Pin a value to evaluate the look

`main.cpp` ships with a test flag for evaluating maximum droop:

```cpp
constexpr bool TEST_PIN_MAX_SLEEPY = true;
// in loop():
if (TEST_PIN_MAX_SLEEPY) eyeStateSetSleepy(eyes, 1.0f);
```

Same trick works for any external setter — the autonomous re-roll inside
the emotion's update will clobber a one-shot call within 8–25 s, so pin
every frame if you want it to stick.

### Live-edit constants

Constants live next to the behavior they control (see Tuning knobs above).
Touch the file and rebuild:

```bash
platformio run -e esp32s3
```

If the Wokwi simulator is already open, click its green ▶ to reload the
new firmware in place — no need to restart the simulator.

### Frame rate

Default is ~50 fps (`FRAME_INTERVAL_MS = 20` in `main.cpp`). All easing is
dt-based, so the look is identical at lower rates — drop to 30 fps if you
need CPU for sensors / Wi-Fi.

### Footprint

Whole animation system: ~3 KB of flash (~0.1% of the 8 MB on the ESP32-S3
DevKit). RAM use is dominated by Adafruit_GFX's framebuffer (1 KB for a
128×64 1-bpp display); the animation state itself is < 200 bytes.
