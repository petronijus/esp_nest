#!/usr/bin/env python3
"""Convert an lv_font_conv C font (--bpp 4 --no-compress) to static A8.

LVGL 9.6 blends a glyph straight from the font data only when the font has
`static_bitmap = 1` and 8 bpp glyphs (lv_draw_sw_letter.c). A 4-bpp font is
instead unpacked through `opa4_table` into a scratch buffer for every glyph
on every frame — for the 140 px fonts that is a large share of a page
transition.

This tool expands each 4-bpp pixel through the same table LVGL uses at run
time (value * 17, lv_font_fmt_txt.c `opa4_table`), so the rendered output is
pixel-identical; only the work moves from run time to build time. Glyph
bitmaps double in flash.

Usage:  python3 tools/font_a8.py <font.c> [<font.c> ...]   (rewrites in place)

Run it on the output of lv_font_conv every time a font is regenerated; a
converted file is detected and left alone.
"""

import re
import sys
from pathlib import Path

OPA4_TABLE = [i * 17 for i in range(16)]   # 0, 17, ... 255 — LVGL's opa4_table

MARKER = "/* Converted to static A8 by tools/font_a8.py */"

# `stride` and `static_bitmap` exist from LVGL 9.3 on.
LVGL_GE_9_3 = "#if LVGL_VERSION_MAJOR > 9 || (LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR >= 3)\n"


def parse_bitmap(text):
    m = re.search(r"glyph_bitmap\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise ValueError("glyph_bitmap[] not found")
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    data = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]+)", body))
    return m, data


def parse_glyphs(text):
    m = re.search(r"glyph_dsc\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise ValueError("glyph_dsc[] not found")
    glyphs = []
    for g in re.finditer(r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), "
                         r"\.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", m.group(1)):
        glyphs.append([int(x) for x in g.groups()])
    return m, glyphs


def unpack4(data, start, count):
    """`count` 4-bit pixels from `start`, rows packed continuously, high nibble first."""
    out = bytearray(count)
    for i in range(count):
        byte = data[start + (i >> 1)]
        nib = (byte >> 4) if (i & 1) == 0 else (byte & 0x0F)
        out[i] = OPA4_TABLE[nib]
    return out


def convert(path):
    text = path.read_text()
    if MARKER in text:
        print(f"{path.name}: already static A8, skipped")
        return
    if not re.search(r"\.bpp = 4,", text) or "--no-compress" not in text:
        raise ValueError(f"{path.name}: expected an uncompressed 4-bpp lv_font_conv font")

    bm_match, data = parse_bitmap(text)
    gl_match, glyphs = parse_glyphs(text)

    out = bytearray()
    new_index = []
    for (index, _adv, w, h, _ox, _oy) in glyphs:
        new_index.append(len(out))
        if w and h:
            out += unpack4(data, index, w * h)

    lines = [", ".join(f"0x{b:02x}" for b in out[i:i + 16]) for i in range(0, len(out), 16)]
    bitmap_body = "\n" + ",\n".join("    " + line for line in lines)
    text = text[:bm_match.start(1)] + bitmap_body + text[bm_match.end(1):]

    gl_match = re.search(r"glyph_dsc\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    entries = iter(new_index)
    glyph_body = re.sub(r"\.bitmap_index = \d+", lambda _m: f".bitmap_index = {next(entries)}", gl_match.group(1))
    text = text[:gl_match.start(1)] + glyph_body + text[gl_match.end(1):]

    # 8 bpp, rows of exactly box_w bytes (stride 1 = byte aligned), static.
    # LVGL 9.2 (the native firmware) has neither field: it packs glyph rows
    # continuously, which at 8 bpp is the same layout as stride 1, and has
    # no static-bitmap path, so the font simply renders there as before.
    text = text.replace(".bpp = 4,", ".bpp = 8,", 1)
    text = re.sub(r"\.bitmap_format = 0,",
                  lambda m: m.group(0) + "\n" + LVGL_GE_9_3 + "    .stride = 1,\n#endif", text, count=1)
    text = re.sub(r"\n\s*\.dsc = &font_dsc,",
                  lambda m: "\n" + LVGL_GE_9_3 + "    .static_bitmap = 1,\n#endif" + m.group(0), text, count=1)
    text = text.replace(" * Bpp: 4", " * Bpp: 8 (static A8 from 4)", 1)
    text = text.replace("******************************************************************************/",
                        "******************************************************************************/\n" + MARKER, 1)
    path.write_text(text)
    print(f"{path.name}: {len(glyphs) - 1} glyphs, bitmap {len(data)} -> {len(out)} bytes")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for arg in sys.argv[1:]:
        convert(Path(arg))


if __name__ == "__main__":
    main()
