# Susuwatori Eyes — ESP32‑S3 + SSD1306 (Wokwi)

A tiny PlatformIO project that drives a 128×64 SSD1306 OLED on an ESP32‑S3 to
render a pair of cartoon eyes. The artwork is split into four independent
layers (left/right sclera + left/right pupil) so each can be transformed and
animated separately.

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

| OLED pin | ESP32-S3 pin |
| -------- | ------------ |
| `SDA`    | GPIO 2       |
| `SCL`    | GPIO 1       |
| `VCC`    | 3V3          |
| `GND`    | GND          |

I²C address: `0x3C`. These match `SDA_PIN` / `SCL_PIN` in `src/main.cpp`.

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

## Troubleshooting

- **`Wokwi: Start Simulator` says "firmware not found"** — you skipped
  `platformio run`, or your `[env:...]` name in `platformio.ini` doesn't
  match the path in `wokwi.toml` (this project uses `esp32s3` for both).
- **OLED stays blank** — confirm `Wire.begin(SDA_PIN, SCL_PIN)` matches the
  pins in `diagram.json` (`SDA=GPIO2`, `SCL=GPIO1`) and the I²C address is
  `0x3C`.
- **PlatformIO complains about `~/.platformio` permissions** — fix with
  `sudo chown -R $(whoami) ~/.platformio` (one‑time).
