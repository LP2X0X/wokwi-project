# Susuwatori Eyes — ESP32‑S3 + TFT_eSPI (Wokwi)

A tiny PlatformIO project that drives SPI TFTs on an ESP32‑S3 to render
a pair of procedural cartoon eyes — fully parameter-driven, no sprite
sheets. Everything (sclera, pupil, eyelid arcs) is drawn as anti-aliased
circles + parabolic arcs each frame, composed off-screen in a
`TFT_eSprite` and pushed to each physical display via per-display CS.

The Wokwi diagram has three screens: two render each eye big enough to
fill the physical fur-cutout build, and a third shows both eyes together
so you can see how they animate as a pair.

> **Driver-agnostic.** The renderer talks to `TFT_eSPI`'s panel-neutral
> API. The default build targets **ILI9341 (240×320)** because that's
> the SPI TFT Wokwi ships in its parts catalog. To target real-hardware
> **ST7789 (240×240)** or **GC9A01 (240×240 round IPS)**, change two
> build flags in `platformio.ini` — no source code changes required:
>
> ```ini
> -DILI9341_DRIVER=1   ; -> -DST7789_DRIVER=1 or -DGC9A01_DRIVER=1
> -DTFT_HEIGHT=320     ; -> -DTFT_HEIGHT=240
> ```
>
> `SCREEN_WIDTH` / `SCREEN_HEIGHT` in `main.cpp` resolve to `TFT_WIDTH`
> / `TFT_HEIGHT` so the eye re-centers automatically on the new panel.

The project runs in [Wokwi](https://wokwi.com/) — no physical hardware required.

## Layout

```
assets/
  susuwatari-eyes-idle-*.png   # 128x64 source artwork
tools/
  gen_eye_bitmaps.py           # segments the PNG into 4 layers, emits header
src/
  main.cpp                     # setup + frame-paced loop driving the anim
  anim/
    eye_anim.h / .cpp          # public API + composePose orchestrator
    eye_pose.h                 # data contracts: EyePose, Modulators, GazeIntent
    eye_render.h / .cpp        # pure (Eye, EyePose) -> pixels renderer
    anim_util.h                # shared math helpers (smoothstep, lerp, RNG)
    behaviors/
      micro_motion.h / .cpp    # pupil drift + gaze blending
      blink.h / .cpp           # blink schedule + lid_close
      sleepy.h / .cpp          # sleepy emotion -> Modulators
  assets/
    eyes_bitmaps.h             # auto-generated 1bpp Adafruit_GFX bitmaps
docs/
  ANIMATION.md                 # full animation system guide
  PERFORMANCE.md               # display loop timing & optimization notes
diagram.json                   # Wokwi circuit (ESP32-S3 + SSD1306)
wokwi.toml                     # tells Wokwi where the firmware binaries live
platformio.ini                 # PlatformIO env (esp32-s3-devkitc-1, Arduino)
```

## Prerequisites

- [PlatformIO Core](https://platformio.org/install/cli) (CLI) — `brew install platformio`
  or use the PlatformIO VS Code extension.
- [Wokwi for VS Code](https://marketplace.visualstudio.com/items?itemName=wokwi.wokwi-vscode)
  extension. After install, run `Wokwi: Request a new License` from the
  Command Palette and follow the browser flow (free for personal use).
- Python 3 (only required if you want to regenerate the eye bitmaps from the
  source PNG).

## Build the firmware

Wokwi reads the binaries declared in `wokwi.toml`:

```toml
[wokwi]
version = 1
firmware = ".pio/build/esp32s3/firmware.bin"
elf = ".pio/build/esp32s3/firmware.elf"
```

So before starting the simulator, build them:

```bash
platformio run -e esp32s3
```

The first build downloads the ESP32 toolchain + Adafruit libs and takes a
minute or two; subsequent builds are seconds.

## Run in Wokwi

1. Open this folder in VS Code (the folder containing `diagram.json`).
2. Open `diagram.json` so the Wokwi controls appear.
3. Command Palette → **`Wokwi: Start Simulator`**.

The OLED + serial monitor pane opens and starts running `src/main.cpp`. After
any code change, rerun `platformio run -e esp32s3` and restart the simulator
(or click the green ▶ in the simulator pane) to load the new firmware.

### Headless / CLI alternative

If you'd rather skip VS Code:

```bash
brew install wokwi-cli
export WOKWI_CLI_TOKEN=<token from https://wokwi.com/dashboard/ci>
platformio run -e esp32s3
wokwi-cli .
```

The CLI reads `wokwi.toml` and `diagram.json` from the current directory.

## Wiring (in `diagram.json`)

The project drives **three SPI TFTs** — two physical eyes that go on
the real model, plus a "preview" screen that shows both eyes together
so you can sanity-check the animation while you tune it.

All three displays share **one SPI bus** (MOSI / SCLK / DC / RST / BL).
Each has its own **chip-select (CS)** pin so we pick which panel
receives a given `pushSprite()` by asserting only that CS LOW.

| Display | role                          | CS pin   | shared SPI pins                |
| ------- | ----------------------------- | -------- | ------------------------------ |
| `tft_l` | left eye (physical)           | GPIO 10  | MOSI=11, SCLK=12, DC=13, RST=14, BL=15 |
| `tft_r` | right eye (physical)          | GPIO 9   | (same)                         |
| `tft_p` | both-eye preview (extra panel)| GPIO 8   | (same)                         |

`VCC` → `3V3`, `GND` → `GND` for all.

**Wokwi pin names** (ILI9341 in sim): `SCK` = SCLK, `SDI` = MOSI,
`D/C` = DC, `RESET` = RST, `LED` = backlight. On real **ST7789/GC9A01**
modules the same lines are labelled `SCL` / `SDA` / `DC` / `RES` /
`BLK` — wire across by function, not by name.

The bus pin assignments live in `platformio.ini` (TFT_eSPI build flags —
`TFT_MOSI`, `TFT_SCLK`, `TFT_DC`, `TFT_RST`, `TFT_BL`). The library's
own CS is disabled (`-DTFT_CS=-1`) because we manage CS manually for
the multi-display setup; CS pin numbers live at the top of
`src/main.cpp`.

If your real-hardware build doesn't ship with the preview panel, set
`has_preview = false` in `main.cpp` (the loop then skips the third
render + push). Preview push optimization is controlled separately by
`ENABLE_PREVIEW_DIRTY_PUSH`.

### Two eye geometries, one EyePose

Both physical eyes and the preview screen render from the **same**
`EyeState` / `EyePose` — so a blink on the preview is the same blink on
the physical screens. They differ only in static `Eye` geometry:

- **Physical** eyes use `kBigLeftEye` / `kBigRightEye` with
  `PHYS_SCLERA_R = 135` (diameter 270 — overflows the 240-wide screen by
  ~15 px per side, soaked up by the fur cutout) and `PHYS_PUPIL_R = 30`
  (the artwork's ~1/4.5 pupil:sclera diameter ratio). `PHYS_SCALE = 4.0`
  scales the animated drift amplitudes so motion reads proportionally
  on the bigger TFT.
- **Preview** screen uses `kPreviewLeft` / `kPreviewRight` at half the
  previous sim size — sclera / pupil radii **25 / 6** (was 50 / 11),
  centers at `(88, 120)` and `(152, 120)` — to keep Wokwi render + push
  cheap while still showing both eyes animating as a pair.

All four constants live at the top of `src/main.cpp` — tune them in one
place to change the look on real hardware.

> **One screen instead of three?** Drop the panels you don't want from
> `diagram.json`, drop the matching `renderEyeOn` / `selectDisplay` /
> `pushSprite` block from `main.cpp`. For a single-screen build that
> shows both eyes, call `renderEyes(spr, kPreviewLeft, kPreviewRight,
> eyes)` and push to the one display.

## Eye bitmaps — how the four layers are generated

The PNG in `assets/` is segmented by `tools/gen_eye_bitmaps.py` into four
independent layers, each cropped to its own bounding box and packed as a
1‑bit‑per‑pixel Adafruit_GFX bitmap (MSB first, row‑major):

| layer             | screen pos | size  | role                                         |
| ----------------- | ---------- | ----- | -------------------------------------------- |
| `eye_left_white`  | (25, 9)    | 36×44 | left sclera (pupil hole filled in white)     |
| `eye_right_white` | (69, 10)   | 36×43 | right sclera                                 |
| `eye_left_pupil`  | (45, 24)   | 12×13 | left pupil (drawn in BLACK on top of sclera) |
| `eye_right_pupil` | (73, 25)   | 12×13 | right pupil                                  |

Pipeline inside the script:

1. Threshold the PNG to a boolean mask of lit pixels.
2. Find 4‑connected components → the two largest blobs are the eyes
   (left = leftmost bbox).
3. Flood‑fill the background from outside to detect enclosed holes per eye;
   holes become the pupil layer, "eye + holes filled" becomes the sclera
   layer.
4. Crop each layer to its bounding box, pack MSB‑first, and emit as a C
   array plus its original `(x, y)` anchor.

### Regenerate after changing the artwork

```bash
python3 -m venv .venv
.venv/bin/pip install Pillow numpy
.venv/bin/python tools/gen_eye_bitmaps.py
platformio run -e esp32s3
```

This rewrites `src/assets/eyes_bitmaps.h`. The script expects the PNG to be
exactly 128×64.

## Procedural animation system

The animation pipeline lives in `src/anim/`. It's parameter-driven (no
frame sheets), runs a full redraw per frame, and is built around one rule:

> **Emotions write into a shared `Modulators` bus; motion behaviors read it
> and update their own state. `composePose()` assembles a final `EyePose`.
> Rendering is a pure function of `(Eye geometry, EyePose)`.**

Currently implemented:

- **Micro motion** — pupils drift continuously toward random targets with
  exponential easing. Optionally blends with an external `GazeIntent` so
  face-tracking signals arrive as smooth eye follow.
- **Blinking** — non-periodic, three-phase asymmetric `smoothstep` with
  occasional double blinks. Hold can be extended via emotion modulators
  ("long sleepy blink").
- **Sleepy mode** — a single 0..1 scalar that writes into `Modulators`.
  Drives lid droop, pupil sink, slower drift, longer blinks, and
  occasional long sleepy blinks. Autonomous mood drift over 8–25 s cycles.
- **Curved eyelids** — two procedural parabolic arcs (upper + lower) with
  spherical perspective: ∩ above the eye center, flat at center, ∪ below.

The architecture is built for extension. Adding a new emotion (surprise,
happy, sad, …) means dropping a new file under `behaviors/` with the same
shape as `sleepy` — no existing behavior changes. Face-tracking, wink, and
scripted scenes are explicit recipes in the guide.

For data model, math, blending rules, tuning knobs, and the full
extensibility playbook, see:

→ **[docs/ANIMATION.md](docs/ANIMATION.md)** — the full animation guide.

→ **[docs/PERFORMANCE.md](docs/PERFORMANCE.md)** — loop timing, Wokwi
profiling results, and every display optimization (dirty push, preview
geometry, SPI, debug flags).

## Performance (summary)

Each frame: `eyeStateUpdate()` → render into a shared 240×240
`TFT_eSprite` → `pushSprite()` over SPI. Animation update is
negligible (~0.2 ms); **render + push** dominate.

Current Wokwi preview build (one panel, half-size eyes, dirty-rect push):

| Phase | Typical | Notes |
| ----- | ------- | ----- |
| update | ~0.2 ms | not a bottleneck |
| render | ~15–30 ms | scales with eye size + animation |
| push | ~10–20 ms | cropped push; was ~31 ms full-frame |
| **FPS** | **~22+** | was ~17 before optimizations |

Implemented levers (details in [docs/PERFORMANCE.md](docs/PERFORMANCE.md)):

- **`ENABLE_PREVIEW_DIRTY_PUSH`** — push eye bbox union only (preview)
- **Half-size preview eyes** — `sclera_r = 25`, centers `(88, 120)` /
  `(152, 120)`; physical eyes unchanged
- **240×240 sprite** on ILI9341 (not full 320 px height)
- **`SPI_FREQUENCY = 80000000`** in `platformio.ini`
- **`ENABLE_*_DISPLAY`** — disable render+push for unwired panels
- **`ENABLE_FPS_COUNTER`** — `[fps] upd=… render=… push=…` once per second

Not worth optimizing on preview (A/B tested): skipping lids, skipping
`fillSprite()`, dirty push on physical eyes (full sprite clip).

## Troubleshooting

- **`Wokwi: Start Simulator` says "firmware not found"** — you skipped
  `platformio run`, or your `[env:...]` name in `platformio.ini` doesn't
  match the path in `wokwi.toml` (this project uses `esp32s3` for both).
- **All three displays show the same content** — one or more CS pins is
  stuck LOW. Confirm `CS_LEFT` / `CS_RIGHT` / `CS_PREVIEW` in `main.cpp`
  match the wiring in `diagram.json` and that `pinMode(... OUTPUT)` ran
  in `setup()`.
- **All displays blank / white screen** — backlight off (check `TFT_BL`
  pin wiring), or the displays never received their init sequence
  (verify the per-display `tft.init()` loop in `setup()` ran with each
  CS asserted in turn).
- **Garbled / shifted pixels** — SPI clock too fast for the wiring. Drop
  `-DSPI_FREQUENCY` in `platformio.ini` from `40000000` to `27000000` or
  `20000000` for breadboard setups; bump back up to `60000000` /
  `80000000` once it's stable.
- **"Sprite alloc failed" in serial** — `TFT_eSprite::createSprite(240,
  240)` needs ~115 KB of RAM. On a stock ESP32-S3 that's fine; if you've
  added a big Wi-Fi/BLE stack later and run out, allocate the sprite in
  PSRAM via `spr.setPsram(true)` before `createSprite()`.
- **Wokwi "Board not found"** — Wokwi doesn't ship an ST7789 part, so
  the sim uses `wokwi-ili9341` (240×320 SPI TFT). Same TFT_eSPI
  library, same wiring pattern; switching to the real ST7789 / GC9A01
  panel is a two-flag change in `platformio.ini`.
- **PlatformIO complains about `~/.platformio` permissions** — fix with
  `sudo chown -R $(whoami) ~/.platformio` (one‑time).

### Switching the panel target

The default build targets **ILI9341 (240×320)** because that's what
Wokwi has. For real hardware you almost certainly want a different
panel; the change is two build flags in `platformio.ini`:

| Target          | `*_DRIVER`           | `TFT_HEIGHT` |
|-----------------|----------------------|--------------|
| ILI9341 (Wokwi) | `ILI9341_DRIVER=1`   | `320`        |
| ST7789 1.3" sq  | `ST7789_DRIVER=1`    | `240`        |
| GC9A01 round    | `GC9A01_DRIVER=1`    | `240`        |

Then rebuild & reflash. No source code changes. Same `TFT_eSPI` API,
same `TFT_eSprite` framebuffer, same wiring (pins are identical between
all three drivers). `SCREEN_WIDTH` / `SCREEN_HEIGHT` in `main.cpp`
resolve to `TFT_WIDTH` / `TFT_HEIGHT` from the build flags, so the eye
re-centers itself on the new panel.

The eye geometry constants (`PHYS_SCLERA_R = 135`, `PHYS_PUPIL_R = 30`)
are sized for 240-wide panels — same on ILI9341 and ST7789/GC9A01. On
ILI9341's taller 240×320 the eye sits centered in the middle of the
display with empty bands top + bottom.
