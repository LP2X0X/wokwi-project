# Display performance & optimization notes

This document captures how the eye loop spends time, what we measured in
Wokwi, which optimizations are implemented, and what **not** to bother with.

The loop each frame:

```
eyeStateUpdate()  →  render into TFT_eSprite  →  pushSprite() over SPI
     (~0.2 ms)            (~render ms)              (~push ms)
```

On ESP32-S3 the dominant costs are **CPU draw** and **SPI push**, not
animation logic.

---

## Measuring loop time

### Serial FPS counter (always on by default)

`ENABLE_FPS_COUNTER = true` in `main.cpp` prints one line per second:

```text
[fps] 22  upd=162us  render=27405us  push=19883us  (total ~47457us/loop)
```

| Field | Meaning |
| ----- | ------- |
| first number | loops completed in the last second (= effective FPS) |
| `upd` | `eyeStateUpdate()` average |
| `render` | sprite draw average (all enabled displays) |
| `push` | SPI push average (all enabled displays) |
| `total` | sum of the three (excludes serial printf itself) |

Timing uses `micros()` around each phase in `loop()`. Toggle
`ENABLE_FPS_COUNTER = false` for a tiny savings (negligible vs render).

### Heartbeat — keep off during perf work

`ENABLE_HEARTBEAT = true` dumps rich pose state every 250 ms over serial.
On Wokwi's simulated UART this can **dominate** loop time. Leave it
`false` unless debugging animation values.

---

## Wokwi baseline (before optimizations)

Preview display only (`ENABLE_PREVIEW_DISPLAY = true`, left/right off),
full 240×240 push, preview eyes at sclera **r = 50**:

| Phase | Time | % of loop |
| ----- | ---- | --------- |
| update | ~0.15–0.19 ms | < 0.3 % |
| render | ~23–40 ms | ~45–50 % |
| push | ~30.6 ms (flat) | ~50–55 % |
| **total** | **~54–59 ms** | **~17 fps** |

Key findings from A/B isolation (runtime toggles, since removed):

| Test | render | push | FPS | Conclusion |
| ---- | ------ | ---- | --- | ---------- |
| **baseline** | ~23–28 ms | ~30.6 ms | ~17 | push + render split ~50/50 |
| **skip push** | ~23–30 ms | ~0 ms | ~33 | SPI push is the main ceiling |
| **skip lids** | ~23 ms | ~30.6 ms | ~17 | lid draw not costly at preview size |
| **skip fillSprite** | ~23–25 ms | ~30.6 ms | ~17 | full-screen clear not costly |

**Takeaway:** optimize **push bandwidth** and **draw area** first. Do not
spend effort on animation update, lids, or `fillSprite()` for the preview
build.

`render` spikes (~32–40 ms) track heavier animation frames (sleepy lids,
sim jitter) — not a separate code bug.

---

## Implemented optimizations

### 1. Preview dirty-rect push

**Files:** `main.cpp` (`pushPreviewSprite`), `eye_render.cpp`
(`eyeBounds`, `boundsUnion`, `previewPushBounds`)

**Flag:** `ENABLE_PREVIEW_DIRTY_PUSH = true` (preview path only)

Instead of pushing the full 240×240 RGB565 buffer (57 600 px), the preview
path pushes the axis-aligned union of both eye bounding boxes via TFT_eSPI's
windowed API:

```cpp
spr.pushSprite(SPRITE_X + b.x, SPRITE_Y + b.y, b.x, b.y, b.w, b.h);
```

Bounds mirror `renderEye()` geometry (sclera ellipse + pupil + 1 px
margin), clipped to the sprite. Falls back to full push if bounds are
invalid or cover ≥ 95 % of the sprite (safety for huge eyes).

**Physical left/right displays** still call full `pushSprite()` — sclera
`PHYS_SCLERA_R = 135` overflows the 240×240 sprite and clips to the full
frame, so cropped push saves nothing there.

**Measured (r = 50 preview eyes):** push ~31 ms → ~20 ms, total ~54 ms →
~47 ms, **~17 fps → ~22 fps**.

### 2. Half-size preview eyes (Wokwi dev build)

**File:** `main.cpp` — `kPreviewLeft` / `kPreviewRight`

Preview geometry is **half** the previous sim size to cut render + push
cost. Physical eye constants (`PHYS_SCLERA_R`, etc.) are unchanged.

| Constant | Previous | Current (Wokwi preview) |
| -------- | -------- | ----------------------- |
| `sclera_r` | 50 | **25** |
| `pupil_r` | 11 | **6** |
| `pupil_dx_neutral` | ±12 | **±6** |
| `PREVIEW_SCALE` | 1.5 | **0.75** (drift stays proportional) |
| left `sclera_cx` | 64 | **88** |
| right `sclera_cx` | 176 | **152** |

Centers were moved inward so the half-size pair still reads as a binocular
face (~14 px gap between sclera edges). Tune `sclera_cx` if layout changes.

Sclera fill scales with r² → ~4× fewer pixels per eye. Dirty-rect push
region shrinks similarly (~107×53 px vs ~213×101 px at r = 50).

### 3. Fixed 240×240 sprite (not full panel height)

**File:** `main.cpp` — `SCREEN_WIDTH` / `SCREEN_HEIGHT = 240`

On ILI9341 (240×320, Wokwi default) the sprite is 240² centered on the
panel (`SPRITE_X`, `SPRITE_Y`). Top/bottom 40 px stay black. Saves ~25 %
SPI vs pushing 240×320.

### 4. SPI clock

**File:** `platformio.ini` — `SPI_FREQUENCY = 80000000`

80 MHz targets ~15 ms full-frame push on real hardware with good wiring.
Drop to 40 / 27 / 20 MHz if breadboard wiring shows garbled pixels.

Optional A/B env: `esp32s3_fspi` drops `USE_HSPI_PORT` and uses
`USE_FSPI_PORT` (ESP32-S3 default). Boot log may show
`spiAttachMISO(): HSPI Does not have default pins on ESP32S3!` with HSPI;
compare push timing on real hardware with `-e esp32s3_fspi`.

### 5. Per-display compile-time gates

**File:** `main.cpp`

```cpp
constexpr bool ENABLE_LEFT_DISPLAY    = false;  // real hardware
constexpr bool ENABLE_RIGHT_DISPLAY   = false;
constexpr bool ENABLE_PREVIEW_DISPLAY = true;
```

Each enabled display runs a full **render + push** every frame even if
that CS is not wired in `diagram.json`. Disable both the panel **and**
the matching `ENABLE_*` flag when testing with one screen.

Also set `has_preview = false` if the preview panel is removed from the
build entirely.

### 6. Renderer: plain circles (not smooth)

**File:** `eye_render.cpp`

`fillSmoothCircle` was tried; on Wokwi it **doubled** render time with no
visible quality win at this resolution. Production path uses
`fillCircle` / `fillEllipse`.

---

## Wokwi vs real hardware

| Topic | Wokwi sim | Real hardware |
| ----- | --------- | --------------- |
| Push time | Often flat ~20–31 ms regardless of cropped size | Scales with pixel count and `SPI_FREQUENCY` |
| FPS | ~22+ with current preview opts | Often higher push throughput at 80 MHz |
| HSPI warning | Harmless in sim; try `esp32s3_fspi` on bench | Use FSPI if HSPI misbehaves |

Wokwi CI (`wokwi-cli`) uses a monthly minute quota; the VS Code extension
uses the desktop license instead.

---

## Multi-display budget

With all three displays enabled, each frame does up to **three** render +
push passes on one shared sprite (~3× single-display cost). At Wokwi-like
timings (~47 ms per pass) expect **~140 ms / frame ≈ 7 fps** unless SPI
and/or geometry optimizations apply per display.

---

## Not worth doing (preview)

These were A/B tested and rejected for the Wokwi preview path:

- **Skip `fillSprite()`** — saves < 2 ms
- **Skip eyelid column loops** — no measurable change at preview radii
- **Dirty-rect push on physical eyes** — bounds clip to full 240×240
- **Faster animation update** — already sub-millisecond

Future work if preview FPS is still insufficient:

- Partial **render** (dirty redraw) — higher complexity, modest gain after
  half-size eyes + cropped push
- **DMA** `pushImage` — library support varies; test on hardware
- Smaller preview sprite (e.g. 128²) — layout tradeoff

---

## Debug / test flags (`main.cpp`)

| Flag | Default | Purpose |
| ---- | ------- | ------- |
| `ENABLE_FPS_COUNTER` | `true` | Phase timing log |
| `ENABLE_HEARTBEAT` | `false` | Rich pose dump (expensive on Wokwi) |
| `ENABLE_PREVIEW_DIRTY_PUSH` | `true` | Cropped preview push |
| `ENABLE_*_DISPLAY` | preview only | Skip unused render+push |
| `TEST_PIN_MAX_SLEEPY` | `false` | Pin sleepy to 1.0 |
| `TEST_DISABLE_MICRO_MOTION` | `false` | Isolate idle gaze |

External emotion triggers (not boot defaults):

```cpp
eyeTriggerAttentive(eyes, intensity, duration_ms, dir_x, dir_y);
eyeTriggerCuriosity(eyes, intensity, duration_ms);
```

`duration_ms` on attentive is the **listening hold** before Relax; add
~500–750 ms for the relax tail. Production values: ~1500–2000 ms, not
multi-second test values.

---

## Related: blink timing (not SPI)

Awake blink total ≈ **360 ms** (`BLINK_CLOSE/HOLD/OPEN = 120/60/180 ms
in `blink.cpp`). Blinks can feel **> 1 s** when autonomous **sleepy**
ramps up:

- `blink_duration_mult` up to **2.5×** at full sleepy
- **Long sleepy blinks** add 300–900 ms hold (~30 % chance at full sleepy)

Gap between blinks: **2.2–5.8 s** (`BLINK_GAP_MIN/MAX_MS`) — separate
from blink animation length.

---

## Quick verification checklist

1. `platformio run -e esp32s3`
2. Wokwi: Start Simulator (preview panel only in `diagram.json`)
3. Serial: confirm `[fps]` lines — `push` should be well below ~31 ms
4. Compare: set `ENABLE_PREVIEW_DIRTY_PUSH = false` → push rises
5. On hardware: try `-e esp32s3_fspi` if HSPI warnings appear
