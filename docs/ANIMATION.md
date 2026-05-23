# Procedural Eye Animation Guide

Deep reference for the animation system in `src/anim/eye_anim.{h,cpp}`. The
[README](../README.md) covers project setup, asset pipeline, and how to build
+ run in Wokwi — this doc is just the animation internals.

## Design philosophy

One rule, applied everywhere:

> **Behaviors are independent layers that all write into one `EyeState`.
> Rendering is a pure function of that state plus the static `Eye`
> descriptors.**

Concretely that means:

- **Parameter-driven, not frame-based.** Nothing is a sprite sheet. Every
  visible motion is a function `f(time, state) → numbers`.
- **Additive composition, no state machines.** Each behavior reads what
  earlier behaviors wrote and contributes its own offset / multiplier.
  There is no "isBlinking" or "isSleepy" branch anywhere.
- **Continuous transitions.** A behavior with `amount = 0` produces output
  identical to that behavior being absent — disable-able by easing a scalar.
- **Frame-rate independent easing.** All smoothing is done with
  `pos += (target − pos) · (1 − exp(−dt / τ))`, so 30 fps and 60 fps look
  identical.
- **Binocular pair, not two independent eyes.** A single `EyeState` drives
  both eyes (shared gaze + synchronized blink). Independent eyes look
  broken.

These constraints are why adding sleepy-mode took ~80 lines and zero
refactors of the existing blink / micro-motion code.

## Data model

```cpp
// Static, build-time geometry. Lives in `kLeftEye` / `kRightEye` in main.cpp.
struct Eye {
  EyeBitmap   sclera;     // bitmap + on-screen (x,y,w,h)
  PupilBitmap pupil;      // bitmap + neutral-gaze (x,y,w,h)
  // No eyelid bitmap: the eyelid is a per-column procedural arc derived
  // from the sclera's bbox + a global curvature constant.
};

// Live, per-frame state. Behaviors mutate this; render reads it.
struct EyeState {
  // ---- micro motion (shared drift -> both eyes look at same point) ----
  float    drift_x, drift_y;          // smoothed offset, pixels
  float    drift_tx, drift_ty;        // current target the drift eases to
  uint32_t drift_next_ms;             // when to pick a new target

  // ---- blink (binocular) ----
  float    lid_close;                 // 0 open .. 1 closed (smooth)
  uint32_t blink_next_ms;             // scheduled start of next blink
  uint32_t blink_start_ms;            // 0 if no blink in flight
  uint16_t blink_close_ms;            // close phase length (set per-blink)
  uint16_t blink_hold_ms;             // fully-closed hold
  uint16_t blink_open_ms;             // open phase length
  bool     double_pending;            // queue a 2nd blink right after this

  // ---- sleepy mode ----
  // One scalar that other layers read as a multiplier — they never branch
  // on it. At sleepy_amount = 0 the animation is identical to fully awake.
  float    sleepy_amount, sleepy_target;
  uint32_t sleepy_next_ms;

  uint32_t last_update_ms;
};
```

## Update loop

```
eyeStateUpdate(now):
    dt = clamp(now - last_update, 0, 100ms)
    updateSleepyState(state, now, dt)   # writes sleepy_amount
    updateMicroMotion(state, now, dt)   # reads sleepy, writes drift_x/y
    updateBlink(state, now)             # reads sleepy, writes lid_close
```

Order matters because later layers READ what earlier layers wrote (no layer
ever reads its own output, so the graph stays cycle-free). Swap, skip, or
add layers freely — each one only ever reads/writes named fields on
`EyeState`.

## Timing system

- All scheduling uses `millis()` and is **non-periodic** — both micro motion
  hold-time and blink gap are uniform-random within tunable ranges.
- A single `last_update_ms` produces a clamped `dt`, fed into the easing.
  Easing math is dt-based, so the look is identical at any frame rate.
- `loop()` is frame-paced via `FRAME_INTERVAL_MS` in `main.cpp` (defaults
  to ~50 fps).
- The RNG is seeded with the ESP32 hardware TRNG (`esp_random()`), so
  patterns differ across resets.

## Per-layer math

### Micro motion

Picks a new random drift target every 0.5–1.8 s and exponentially eases
toward it. Sleepy mode scales `range`, `hold`, and `τ` so movement gets
heavier and lazier without ever stopping.

```
target re-roll: every random(DRIFT_HOLD_MIN_MS, DRIFT_HOLD_MAX_MS)
                       · (1 + sleepy · SLEEPY_DRIFT_HOLD_MULT)
target value:   uniform(±DRIFT_RANGE_PX · (1 - sleepy · SLEEPY_DRIFT_RANGE_REDUCTION))
easing:         pos += (target − pos) · (1 − exp(−dt / τ))
                τ  = DRIFT_TAU_S · (1 + sleepy · SLEEPY_DRIFT_TAU_MULT)
```

Critically-damped exponential easing — never overshoots, frame-rate
independent.

### Blink

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

Phase lengths are sampled **once at the start of each blink**, not every
frame, so each blink stays consistent even if `sleepy_amount` shifts
mid-blink.

**Double blinks** — after ~18% of blinks, the next blink is scheduled
90–180 ms later, producing a natural "blink-blink" occasionally.

**Sleepy long blink** — at blink start, with probability
`sleepy_amount · 30%`, an extra 300–900 ms is added to `blink_hold_ms`.
Awake eyes literally cannot produce this; very sleepy eyes do it ~30% of
the time.

### Sleepy mode

A single scalar `sleepy_amount ∈ [0, 1]` is produced by `updateSleepyState`
and consumed as a continuous multiplier by every other layer.

**Autonomous mood drift** (re-rolled every 8–25 s):

| bucket    | probability | resulting `sleepy_target` |
|-----------|------------:|---------------------------|
| `< 0.55`  |         55% | uniform `[0.00, 0.20]` — bright |
| `< 0.90`  |         35% | uniform `[0.20, 0.55]` — drowsy |
| `≥ 0.90`  |         10% | uniform `[0.65, 1.00]` — very sleepy |

**Easing** toward the target with `SLEEPY_TAU_S ≈ 1.5 s` so transitions
feel like a slow mood change, not a switch.

**External hook** — call `eyeStateSetSleepy(state, target)` from any sensor
(ambient light, time-of-day, touch) to nudge `sleepy_target`. The
autonomous re-roll inside `updateSleepyState()` will eventually overwrite
your value, so call it on every tick if you want it to stick.

#### What sleepy_amount modulates

All effects scale linearly. At `sleepy_amount = 0` every multiplier
contributes nothing.

| Surface           | Effect                                                                                |
|-------------------|---------------------------------------------------------------------------------------|
| Upper lid offset  | `+ sleepy_amount · SLEEPY_UPPER_DROOP_FRACTION · sclera.h`                            |
| Lower lid offset  | `+ sleepy_amount · SLEEPY_LOWER_RISE_FRACTION · sclera.h`                             |
| Pupil y           | `+ sleepy_amount · SLEEPY_PUPIL_BIAS_PX` downward, on top of micro-motion drift       |
| Drift τ           | `× (1 + sleepy_amount · SLEEPY_DRIFT_TAU_MULT)` — heavier feel, never locks up        |
| Drift range       | `× (1 − sleepy_amount · SLEEPY_DRIFT_RANGE_REDUCTION)` — wanders less far             |
| Drift hold        | `× (1 + sleepy_amount · SLEEPY_DRIFT_HOLD_MULT)` — stays put longer between targets   |
| Blink durations   | `× (1 + sleepy_amount · SLEEPY_BLINK_DURATION_MULT)` close + hold + open all stretched|
| Long sleepy blink | Per-blink coin flip with probability `sleepy_amount · SLEEPY_LONG_BLINK_PCT_AT_FULL%` |

Two key properties that come for free from the additive design:

1. **The lid arc curvature still works.** The unified perspective rule
   `curvature = (centerline_y − eye_cy) / b` means a sleepy droop alone
   (no blink) automatically gets a soft ∩ shape, and combined with a
   partial blink the curvature smoothly slides toward ∪ as the lid passes
   the eye center. No special cases needed.
2. **Micro motion never stops.** Drift and pupil bias are independent
   contributions to the same final position. Even at `sleepy_amount = 1`
   the pupil keeps wandering — just slowly, in a smaller range, around a
   slightly lower center.

Mood reference:

```
sleepy_amount = 0.0     awake — identical to disabled
              = 0.3     light drowsy — small lid sag, pupil sinks, drift slightly slower
              = 0.7     visibly sleepy — heavy lids, slow blinks, occasional long holds
              = 1.0     "Ghibli soot creature about to nap" — barely-open eyes, big slow blinks
```

## Render pipeline

```
renderEyes():
    clearDisplay()
    for each eye:
        drawBitmap(sclera, WHITE)                                            # 1
        drawBitmap(pupil at (x+drift_x, y+drift_y+sleepy_pupil_dy), BLACK)   # 2 (hole)
        drawCurvedLid(eye, upper_centerline, curvature, fill_from_top=true)  # 3
        drawCurvedLid(eye, lower_centerline, curvature, fill_from_top=false) # 4
    display()
```

### Lid offset blending (the "additive offsets" rule)

```cpp
upper_offset = blink_upper_offset + sleepy_droop_offset    // clamped to [0, h]
lower_offset = blink_lower_offset + sleepy_rise_offset     // clamped to [0, h]
```

Where:

```
blink_upper_offset  = lid_close · h
blink_lower_offset  = lid_close · LOWER_LID_TRAVEL_FRACTION · h
sleepy_droop_offset = sleepy_amount · SLEEPY_UPPER_DROOP_FRACTION · h
sleepy_rise_offset  = sleepy_amount · SLEEPY_LOWER_RISE_FRACTION  · h
```

Pupils are drawn in `SSD1306_BLACK` after the sclera, so they cut holes
through the white. This lets a pupil drift freely inside its eye without
leaving smear pixels behind.

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
- **Travel range** — upper lid centerline travels the full eye height (top
  edge → bottom edge) as `lid_close` goes 0 → 1; the lower lid only
  travels `LOWER_LID_TRAVEL_FRACTION` of that. Real eyes blink mostly with
  the upper lid (~80% of the closing motion).

Pixels in the bbox that fall outside the eye silhouette were never lit, so
painting BLACK over them is a no-op — the visible lid edge is naturally
clipped to the eye shape. No silhouette mask needed.

## Tuning knobs (top of `eye_anim.cpp`)

| Constant                        | Default      | Effect                                                                                                               |
|---------------------------------|--------------|----------------------------------------------------------------------------------------------------------------------|
| `DRIFT_RANGE_PX`                | 2.0 px       | Peak pupil offset                                                                                                    |
| `DRIFT_TAU_S`                   | 0.35 s       | Smoothing tau (larger → lazier)                                                                                      |
| `DRIFT_HOLD_MIN/MAX_MS`         | 500 / 1800   | How long a drift target is held                                                                                      |
| `BLINK_GAP_MIN/MAX_MS`          | 2200 / 5800  | Time between blinks                                                                                                  |
| `BLINK_CLOSE/HOLD/OPEN`         | 90 / 40 / 160| Per-phase blink duration in ms                                                                                       |
| `DOUBLE_BLINK_PCT`              | 18           | % chance a blink is followed by a 2nd                                                                                |
| `LID_CURVE_FRACTION`            | 0.22         | Eyelid arc bow (∩/∪) as fraction of eye height. 0 = flat shutter; ~0.3 = very domed. Shared by upper and lower lids. |
| `LOWER_LID_TRAVEL_FRACTION`     | 0.30         | How far the lower lid rises relative to the upper lid's full travel. Lower = lower lid does less of the closing.     |
| `SLEEPY_TAU_S`                  | 1.5          | Easing tau for `sleepy_amount` itself. Bigger = slower mood transitions.                                             |
| `SLEEPY_HOLD_MIN/MAX_MS`        | 8000 / 25000 | Autonomous re-roll cadence for `sleepy_target`.                                                                      |
| `SLEEPY_UPPER_DROOP_FRACTION`   | 0.2          | Upper lid sag at full sleepy.                                                                                        |
| `SLEEPY_LOWER_RISE_FRACTION`    | 0.15         | Lower lid puff at full sleepy.                                                                                       |
| `SLEEPY_PUPIL_BIAS_PX`          | 1.5          | Downward pupil bias at full sleepy.                                                                                  |
| `SLEEPY_DRIFT_TAU_MULT`         | 1.5          | Drift τ slowdown at full sleepy (× 1 + this).                                                                        |
| `SLEEPY_DRIFT_RANGE_REDUCTION`  | 0.5          | Drift range shrinkage at full sleepy.                                                                                |
| `SLEEPY_DRIFT_HOLD_MULT`        | 1.5          | Drift hold extension at full sleepy.                                                                                 |
| `SLEEPY_BLINK_DURATION_MULT`    | 1.5          | Blink slowdown at full sleepy.                                                                                       |
| `SLEEPY_LONG_BLINK_PCT_AT_FULL` | 30           | Max % chance per blink of an extra-long sleepy hold.                                                                 |

## Extending it (recipes)

Every new behavior follows the same pattern: write a new `updateXxx()` that
reads time + earlier layers' outputs, write into `EyeState`, register it in
`eyeStateUpdate()`. No state machines, no flags.

| Behavior       | How to add                                                                                                                                                          |
|----------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Look-at target | Add `updateGaze()` *before* `updateMicroMotion`; have it set `drift_tx/ty` so micro motion eases to it.                                                            |
| Curiosity      | Temporarily widen `DRIFT_RANGE_PX`, lower `BLINK_GAP_MIN/MAX_MS`, and bias `drift_ty` upward — same additive pattern as sleepy.                                    |
| Wink           | Make `lid_close` per-eye (split into `lid_l`, `lid_r`) and animate one side independently.                                                                          |
| Face tracking  | Drive `drift_tx/ty` from the camera/sensor input. The smoothing already turns jittery readings into soft motion.                                                   |
| Emotions       | Per-emotion preset (drift range, blink gap, lid baseline, pupil scale) — crossfade between presets via a 0..1 mix variable, identical pattern to `sleepy_amount`.  |
| Per-eye droop  | Move `SLEEPY_*_FRACTION` constants onto `Eye` as `float lid_curve, lower_lid_travel`, pass them through to `drawCurvedLid` — already takes everything per-call.    |

## Dev / test tips

### Pin a value to evaluate the look

`main.cpp` ships with a test flag for evaluating maximum droop:

```cpp
constexpr bool TEST_PIN_MAX_SLEEPY = true;
// in loop():
if (TEST_PIN_MAX_SLEEPY) eyeStateSetSleepy(eyes, 1.0f);
```

Same trick works for any scalar parameter — the `setXxx` helpers (or direct
field assignment in your own code) get clobbered by the autonomous re-roll
in 8–25 s unless you pin them every tick.

### Live-edit constants

Constants live at the top of `src/anim/eye_anim.cpp`. Touch the file and
rebuild:

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
128×64 1-bpp display); the animation state itself is < 100 bytes.
