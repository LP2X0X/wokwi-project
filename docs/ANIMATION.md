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
    blink.h / .cpp        # blink schedule + lid_close (a "motion")
    micro_motion.h / .cpp # pupil drift + gaze blending (a "motion")
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
  float pupil_dx, pupil_dy;        // pixels, additive offset from neutral
  float pupil_scale;               // 1.0 neutral; future surprise/dilation
  float upper_lid_amount;          // 0..1 fraction of eye height (0 = open)
  float lower_lid_amount;          // 0..1
  float eye_open_amount;           // 1.0 neutral; future wide-eyes / squint
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
```

### Container (eye_anim.h)

```cpp
struct EyeState {
  MicroMotionState micro;
  BlinkState       blink;
  SleepyState      sleepy;
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
    sleepyUpdate(s.sleepy, now, dt)
    # surpriseUpdate(s.surprise, now, dt)   # future
    # happyUpdate(s.happy, now, dt)         # future

    # Pass 2: emotions write into a fresh Modulators bus.
    mods = Modulators::neutral()
    sleepyModulate(s.sleepy, mods)
    # surpriseModulate(s.surprise, mods)    # future

    # Pass 3: motion behaviors consume modulators + gaze.
    microMotionUpdate(s.micro, mods, s.gaze, now, dt)
    blinkUpdate(s.blink, mods, now)

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
  // Pupil position
  out.pupil_dx    = s.micro.drift_x;
  out.pupil_dy    = s.micro.drift_y + mods.pupil_y_bias_px;
  out.pupil_scale = mods.pupil_scale_mult;

  // Lid amounts (normalized; renderer scales by per-eye height)
  float upper = s.blink.lid_close + mods.lid_upper_droop;
  float lower = s.blink.lid_close * blink::LOWER_LID_TRAVEL_FRACTION
                + mods.lid_lower_rise;
  out.upper_lid_amount = clamp01(upper);
  out.lower_lid_amount = clamp01(lower);

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

## Render pipeline

```
renderEyes(d, left, right, pose):
    clearDisplay()
    renderEye(d, left,  pose)
    renderEye(d, right, pose)
    display()

renderEye(d, eye, pose):
    drawBitmap(sclera, WHITE)                                              # 1
    drawBitmap(pupil at (x+pose.pupil_dx, y+pose.pupil_dy), BLACK)         # 2 (hole)
    upper_y = sclera.y + pose.upper_lid_amount * sclera.h
    lower_y = sclera.y + sclera.h - pose.lower_lid_amount * sclera.h
    drawCurvedLid(eye, upper_y, curvature, fill_from_top=true)             # 3
    drawCurvedLid(eye, lower_y, curvature, fill_from_top=false)            # 4
```

The renderer cannot observe `lid_close`, `sleepy_amount`, drift state, or
any other behavior internal — only the composed pose. Replacing the
renderer (LCD with grayscale, host-side simulator) means re-implementing
this one function; behaviors don't change.

### Lid curvature math (spherical perspective)

Both lids share one helper (`drawCurvedLid`) and one perspective rule:

```
bow_curvature = (lid_centerline_y − eye_center_y) / (sclera.h / 2)
y_lid(x)      = lid_centerline_y + bow_curvature · max_dip · (1 − x_norm²)
where x_norm  = (x − eye_center_x) / (sclera.w / 2)   ∈ [-1, +1]
      max_dip = sclera.h · LID_CURVE_FRACTION
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
| `eye_render.cpp`                         | `LID_CURVE_FRACTION` (rendering knob)              |

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
| Curiosity      | Emotion. Lower drift hold, raise drift range, slight upward `pupil_y_bias_px`. Same template as sleepy.                                           |
| Surprise       | Emotion. `pupil_scale_mult`, `eye_open_add`, briefly snap-faster blinks. Triggered by event with quick decay.                                     |
| Per-eye droop  | Move `SLEEPY_*_FRACTION` constants onto `Eye`, sleepy reads them. Or split sleepy into per-eye amounts.                                           |
| Scripted scene | Build a tiny scenario sequencer that calls `eyeStateSetSleepy` / `eyeSetGazeTarget` / future emotion triggers on a timeline. Behaviors don't care. |

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
