#!/usr/bin/env python3
"""Generate per-page background glow assets for the 4-page menu.

Takes the original bg-glow-yellow.png (a yellow-on-black gradient strip),
extracts its intensity map, recolors it to each page's accent color and
orients the bright edge toward the screen edge the glow sits on
(Figma frames: Lights=top, Fiiuuu=right, Blinds=bottom, Sound=left).

Emits LVGL 9 C arrays (RGB565, same format as the original assets).
Byte order is auto-detected by comparing a bright pixel against the
existing bg-glow-yellow.c.

Paths (relative to the repo root, independent of the working directory):
  input   assets/bg-glow-yellow.png                     (design source)
  output  esphome/esp-nest-components/esp_nest/bg-glow-*.c
          (flat in the ESPHome component dir, which ESPHome compiles and
          components/esp_nest_shared builds into the native firmware; the
          existing bg-glow-yellow.c there is also the byte-order reference)

Requires Pillow, e.g.:
  uv run --with pillow python3 tools/gen_glow_assets.py
Regenerating from unchanged sources reproduces the committed files
byte-for-byte.
"""

import re
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ROOT / "assets"
OUT_DIR = ROOT / "esphome" / "esp-nest-components" / "esp_nest"

SRC_PNG = SOURCES / "bg-glow-yellow.png"
REF_C = OUT_DIR / "bg-glow-yellow.c"

# (c_symbol, file_stem, hex_color, edge, visible_thickness)
#   edge: which screen edge the glow hugs; thickness from the Figma
#   frame's visible slice of the 450x450 glow square.
TARGETS = [
    ("bg_glow_yellow", "bg-glow-yellow", 0xFFE600, "top", 201),
    ("bg_glow_teal",   "bg-glow-teal",   0x00E5CC, "right", 180),
    ("bg_glow_orange", "bg-glow-orange", 0xFB4A04, "bottom", 173),
    ("bg_glow_purple", "bg-glow-purple", 0xDE04FF, "left", 171),
]


def load_intensity(path):
    """Composite RGBA onto black, return 0..1 intensity map (max channel)."""
    im = Image.open(path).convert("RGBA")
    w, h = im.size
    px = im.load()
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            r, g, b, a = px[x, y]
            row.append(max(r, g, b) * a / (255 * 255))
        rows.append(row)
    return rows  # rows[y][x]


def bright_edge(intens):
    """Return 'top' or 'bottom' depending on which row is brighter."""
    first = sum(intens[0]) / len(intens[0])
    last = sum(intens[-1]) / len(intens[-1])
    return "top" if first >= last else "bottom"


def detect_byte_order(ref_c_text, intens):
    """Compare the brightest pixel of the reference asset against both
    RGB565 packings to learn whether bytes are little or big endian."""
    data = [int(t, 16) for t in re.findall(r"0x([0-9A-Fa-f]{2})", ref_c_text)]
    w = len(intens[0])
    # brightest pixel position in the intensity map
    by, bx, bv = 0, 0, -1.0
    for y, row in enumerate(intens):
        for x, v in enumerate(row):
            if v > bv:
                by, bx, bv = y, x, v
    idx = (by * w + bx) * 2
    lo, hi = data[idx], data[idx + 1]
    r, g, b = int(255 * bv), int(230 * bv), 0  # #FFE600 scaled
    expect = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    le = hi << 8 | lo
    be = lo << 8 | hi
    return "little" if abs(le - expect) <= abs(be - expect) else "big"


def orient(intens, edge):
    """Rotate/flip the (bright-at-`bright_edge`) strip so its bright side
    faces `edge`, returning rows[y][x] of the result."""
    src_edge = bright_edge(intens)
    if src_edge == "bottom":
        intens = intens[::-1]  # normalize: bright at top
    if edge == "top":
        return intens
    if edge == "bottom":
        return intens[::-1]
    h, w = len(intens), len(intens[0])
    if edge == "right":   # rotate 90 deg CW: top edge -> right edge
        return [[intens[h - 1 - x][y] for x in range(h)] for y in range(w)]
    if edge == "left":    # rotate 90 deg CCW: top edge -> left edge
        return [[intens[x][w - 1 - y] for x in range(h)] for y in range(w)]
    raise ValueError(edge)


def crop_to(intens, edge, thickness):
    """Crop the dark side so the strip is `thickness` deep from `edge`."""
    if edge == "top":
        return intens[:thickness]
    if edge == "bottom":
        return intens[-thickness:]
    if edge == "right":
        return [row[-thickness:] for row in intens]
    if edge == "left":
        return [row[:thickness] for row in intens]
    raise ValueError(edge)


def emit_c(symbol, stem, color, intens, byte_order):
    h, w = len(intens), len(intens[0])
    tr, tg, tb = (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF
    out = []
    for row in intens:
        for v in row:
            r, g, b = int(tr * v + 0.5), int(tg * v + 0.5), int(tb * v + 0.5)
            px = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            lo, hi = px & 0xFF, px >> 8
            out += [lo, hi] if byte_order == "little" else [hi, lo]

    lines = []
    for i in range(0, len(out), 16):
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in out[i:i + 16]) + ",")
    body = "\n".join(lines)

    (OUT_DIR / f"{stem}.c").write_text(f"""\
/* Generated by tools/gen_glow_assets.py — do not edit by hand.
 * Source: bg-glow-yellow.png intensity, recolored to #{color:06X},
 * bright edge: see TARGETS table in the generator. */

#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMAGE_{symbol}
#define LV_ATTRIBUTE_IMAGE_{symbol}
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMAGE_{symbol} uint8_t {symbol}_map[] = {{
{body}
}};

const lv_image_dsc_t {symbol} = {{
  .header.cf = LV_COLOR_FORMAT_RGB565,
  .header.magic = LV_IMAGE_HEADER_MAGIC,
  .header.w = {w},
  .header.h = {h},
  .data_size = {w * h} * 2,
  .data = {symbol}_map,
}};
""")
    print(f"{stem}.c: {w}x{h} #{color:06X}")


def main():
    intens = load_intensity(SRC_PNG)
    byte_order = detect_byte_order(REF_C.read_text(), intens)
    print(f"source {len(intens[0])}x{len(intens)}, bright edge: "
          f"{bright_edge(intens)}, byte order: {byte_order}")
    for symbol, stem, color, edge, thickness in TARGETS:
        emit_c(symbol, stem, color, crop_to(orient(intens, edge), edge, thickness), byte_order)


if __name__ == "__main__":
    main()
