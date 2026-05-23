"""Segment the susuwatari eyes PNG into 4 layered bitmaps and emit them as
Adafruit_GFX-compatible C++ headers.

Layers (each cropped to its own bounding box, with on-screen anchor x/y):
  - left eye white  (sclera, with pupil hole filled in white)
  - right eye white
  - left eye pupil  (the dark dot inside the left eye)
  - right eye pupil

Adafruit_GFX bitmaps are 1bpp, MSB-first, row-major, padded to whole bytes per row.

Run:
  .venv/bin/python tools/gen_eye_bitmaps.py
"""

from __future__ import annotations

import os
from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC_IMG = ROOT / "assets" / "susuwatari-eyes-idle-21100092-7f08-4071-a9b3-ca2a31590d62.png"
OUT_HEADER = ROOT / "src" / "assets" / "eyes_bitmaps.h"

SCREEN_W, SCREEN_H = 128, 64

# Pixel is "on" (white) if luminance is above this. The source PNG is white-on-
# transparent so we also treat fully transparent as off.
LUM_ON_THRESHOLD = 128


def load_mask(path: Path) -> np.ndarray:
    """Return a HxW boolean mask of "lit" pixels (white + opaque)."""
    img = Image.open(path).convert("RGBA")
    if img.size != (SCREEN_W, SCREEN_H):
        raise SystemExit(f"expected {SCREEN_W}x{SCREEN_H}, got {img.size}")
    arr = np.array(img)
    rgb = arr[..., :3]
    a = arr[..., 3]
    lum = (0.299 * rgb[..., 0] + 0.587 * rgb[..., 1] + 0.114 * rgb[..., 2]).astype(
        np.int32
    )
    return (lum >= LUM_ON_THRESHOLD) & (a >= 128)


def connected_components(mask: np.ndarray) -> list[np.ndarray]:
    """4-connected components of True pixels; returns list of component masks."""
    h, w = mask.shape
    visited = np.zeros_like(mask, dtype=bool)
    components: list[np.ndarray] = []
    for sy in range(h):
        for sx in range(w):
            if not mask[sy, sx] or visited[sy, sx]:
                continue
            comp = np.zeros_like(mask, dtype=bool)
            q: deque[tuple[int, int]] = deque([(sy, sx)])
            visited[sy, sx] = True
            while q:
                y, x = q.popleft()
                comp[y, x] = True
                for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                    ny, nx = y + dy, x + dx
                    if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not visited[ny, nx]:
                        visited[ny, nx] = True
                        q.append((ny, nx))
            components.append(comp)
    return components


def bbox(mask: np.ndarray) -> tuple[int, int, int, int]:
    """Return (x, y, w, h) bounding box of True pixels."""
    ys, xs = np.where(mask)
    if len(xs) == 0:
        return (0, 0, 0, 0)
    x0, x1 = int(xs.min()), int(xs.max())
    y0, y1 = int(ys.min()), int(ys.max())
    return (x0, y0, x1 - x0 + 1, y1 - y0 + 1)


def fill_holes(mask: np.ndarray) -> np.ndarray:
    """Fill interior holes (4-connected background regions not touching the border)."""
    h, w = mask.shape
    bg = ~mask
    visited = np.zeros_like(mask, dtype=bool)
    q: deque[tuple[int, int]] = deque()
    for x in range(w):
        for y in (0, h - 1):
            if bg[y, x] and not visited[y, x]:
                visited[y, x] = True
                q.append((y, x))
    for y in range(h):
        for x in (0, w - 1):
            if bg[y, x] and not visited[y, x]:
                visited[y, x] = True
                q.append((y, x))
    while q:
        y, x = q.popleft()
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            ny, nx = y + dy, x + dx
            if 0 <= ny < h and 0 <= nx < w and bg[ny, nx] and not visited[ny, nx]:
                visited[ny, nx] = True
                q.append((ny, nx))
    holes = bg & ~visited
    return mask | holes


def pack_bitmap(mask: np.ndarray) -> tuple[bytes, int, int]:
    """Pack a tightly-cropped mask to Adafruit_GFX 1bpp MSB-first bytes.

    Returns (bytes, width, height) where width/height are the *cropped* dims.
    """
    x, y, w, h = bbox(mask)
    if w == 0 or h == 0:
        return (b"", 0, 0)
    cropped = mask[y : y + h, x : x + w]
    row_bytes = (w + 7) // 8
    out = bytearray(row_bytes * h)
    for ry in range(h):
        for rx in range(w):
            if cropped[ry, rx]:
                out[ry * row_bytes + (rx >> 3)] |= 0x80 >> (rx & 7)
    return (bytes(out), w, h)


def fmt_bytes_c(name: str, data: bytes, bytes_per_line: int = 12) -> str:
    if not data:
        return f"static const uint8_t {name}[] PROGMEM = {{}};\n"
    lines = []
    for i in range(0, len(data), bytes_per_line):
        chunk = data[i : i + bytes_per_line]
        lines.append("  " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    body = "\n".join(lines)
    return f"static const uint8_t {name}[] PROGMEM = {{\n{body}\n}};\n"


def emit_layer(name: str, mask: np.ndarray) -> tuple[str, dict]:
    x, y, w, h = bbox(mask)
    data, _, _ = pack_bitmap(mask)
    cdef = fmt_bytes_c(f"{name}_bmp", data)
    meta = {
        "name": name,
        "x": x,
        "y": y,
        "w": w,
        "h": h,
        "bytes": len(data),
    }
    return cdef, meta


def main() -> None:
    mask = load_mask(SRC_IMG)
    comps = connected_components(mask)
    comps = [c for c in comps if c.sum() >= 8]
    if len(comps) < 2:
        raise SystemExit(f"expected at least 2 eye blobs, got {len(comps)}")
    comps.sort(key=lambda c: c.sum(), reverse=True)
    eye_a, eye_b = comps[0], comps[1]
    xa, *_ = bbox(eye_a)
    xb, *_ = bbox(eye_b)
    if xa <= xb:
        left_eye, right_eye = eye_a, eye_b
    else:
        left_eye, right_eye = eye_b, eye_a

    def split_eye(eye: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        """white-with-pupil-filled, pupil-only."""
        x, y, w, h = bbox(eye)
        if w == 0:
            empty = np.zeros_like(eye)
            return empty, empty
        eye_filled = fill_holes(eye)
        pupil = eye_filled & ~eye
        return eye_filled, pupil

    l_white, l_pupil = split_eye(left_eye)
    r_white, r_pupil = split_eye(right_eye)

    layers = [
        ("eye_left_white", l_white),
        ("eye_right_white", r_white),
        ("eye_left_pupil", l_pupil),
        ("eye_right_pupil", r_pupil),
    ]

    out = []
    out.append("// Auto-generated by tools/gen_eye_bitmaps.py - DO NOT EDIT BY HAND.")
    out.append("// Layers are independent so each can be translated/animated separately.")
    out.append("//")
    out.append("// Each layer exposes:")
    out.append("//   <name>_bmp[]  : Adafruit_GFX 1bpp packed bitmap (MSB first)")
    out.append("//   <name>_w      : bitmap width in pixels")
    out.append("//   <name>_h      : bitmap height in pixels")
    out.append("//   <name>_x      : default top-left X on the 128x64 screen")
    out.append("//   <name>_y      : default top-left Y on the 128x64 screen")
    out.append("")
    out.append("#pragma once")
    out.append("")
    out.append("#include <Arduino.h>")
    out.append("")

    metas = []
    for name, m in layers:
        cdef, meta = emit_layer(name, m)
        out.append(cdef)
        out.append(
            f"static constexpr int16_t {name}_w = {meta['w']};\n"
            f"static constexpr int16_t {name}_h = {meta['h']};\n"
            f"static constexpr int16_t {name}_x = {meta['x']};\n"
            f"static constexpr int16_t {name}_y = {meta['y']};\n"
        )
        metas.append(meta)

    OUT_HEADER.parent.mkdir(parents=True, exist_ok=True)
    OUT_HEADER.write_text("\n".join(out))

    print(f"wrote {OUT_HEADER.relative_to(ROOT)}")
    for m in metas:
        print(
            f"  {m['name']:<18s} pos=({m['x']:>3d},{m['y']:>2d}) "
            f"size={m['w']:>3d}x{m['h']:<2d}  {m['bytes']:>4d} bytes"
        )


if __name__ == "__main__":
    main()
