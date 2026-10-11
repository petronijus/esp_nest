# Design sources

Source files for the UI assets. Nothing in this directory is compiled: the
generated C arrays live flat in the ESPHome component directory
`esphome/esp-nest-components/esp_nest/` (ESPHome compiles only top-level
sources there), and the native firmware builds the same single copy through
`components/esp_nest_shared`.

| Source | Generated asset(s) | Generator |
|---|---|---|
| `bg-glow-yellow.png` | `bg-glow-{yellow,teal,orange,purple}.c` (page edge glows) | `tools/gen_glow_assets.py` |
| `helvetica.otf` (Helvetica Neue LT Std 23 Ultra Light Extended) | `font_helvetica_100.c` (blinds group labels) | `lv_font_conv` |
| `helvetica_detail.otf` (subset of the same face) | `font_helvetica_140.c` (detail-screen values) | `lv_font_conv` |
| `cambria.ttc` (face 0, Cambria Regular) | `font_cambria_140.c` (menu page titles) | `lv_font_conv` |

Kept for reference only, with no generated counterpart in the build:
`bg-glow-teal.png`, `circle-glow-teal.png`, `circle-glow-yellow.png`,
`fiiuuu.png`, `lights.png` (earlier design exports) and `cambria_title.ttf`
(a 9-glyph subset of Cambria Regular).

All commands below run from the repository root. With unchanged sources they
reproduce the committed glyph and pixel data byte-for-byte; for the fonts only
the `Opts:` comment in the file header changes, because it records the paths
the converter was invoked with (the committed headers of the two 140 px fonts
still name the old `main/assets/` location, which is harmless generated
metadata).

## Page glows

```bash
uv run --with pillow python3 tools/gen_glow_assets.py
```

The script recolours and orients the intensity map of `bg-glow-yellow.png`
for each page (colours and edges in its `TARGETS` table) and auto-detects the
RGB565 byte order from the existing `bg-glow-yellow.c`.

## Fonts

`lv_font_conv` is pinned to **1.5.3**: it is the version that reproduces the
committed fonts exactly (1.5.2 rasterizes differently).

```bash
OUT=esphome/esp-nest-components/esp_nest

# Printable ASCII plus the Czech letters the face has (it lacks ČĎĚŇŘŤŮ).
npx --yes lv_font_conv@1.5.3 --bpp 4 --size 100 --font assets/helvetica.otf \
  --range 0x20-0x7E,0xC1,0xC9,0xCD,0xD3,0xDA,0xDD,0xE1,0xE9,0xED,0xF3,0xFA,0xFD,0x160-0x161,0x17D-0x17E \
  --format lvgl --no-compress -o $OUT/font_helvetica_100.c --lv-include lvgl.h

npx --yes lv_font_conv@1.5.3 --bpp 4 --size 140 --font assets/helvetica_detail.otf \
  --range 0x30-0x39,0x64-0x69,0x6C-0x6D,0x6F,0x75,0x77 \
  --format lvgl --no-compress -o $OUT/font_helvetica_140.c --lv-include lvgl.h

# lv_font_conv cannot read a font collection: extract face 0 first.
CAMBRIA="${TMPDIR:-/tmp}/cambria_regular.ttf"
uv run --with fonttools python3 -c \
  "import sys; from fontTools.ttLib import TTCollection; TTCollection('assets/cambria.ttc').fonts[0].save(sys.argv[1])" \
  "$CAMBRIA"
npx --yes lv_font_conv@1.5.3 --bpp 4 --size 140 --font "$CAMBRIA" \
  --range 0x42,0x46,0x4C,0x53,0x64,0x67-0x69,0x6C,0x6E,0x6F,0x73-0x75 \
  --format lvgl --no-compress -o $OUT/font_cambria_140.c --lv-include lvgl.h

# Expand the 4-bpp glyphs to static A8 (pixel-identical, see below).
python3 tools/font_a8.py $OUT/font_helvetica_100.c $OUT/font_helvetica_140.c $OUT/font_cambria_140.c
```

**Static A8.** LVGL 9.6 blends a glyph straight from the font data only when
the font is 8 bpp with `static_bitmap = 1`; a 4-bpp font is unpacked into a
scratch buffer for every glyph on every frame, which made the 140 px titles
and values a measurable part of each page transition (about 5 ms of a
46 ms frame, `esphome/README.md`). `tools/font_a8.py` expands every 4-bit
pixel through the table LVGL itself uses at run time (`opa4_table`, value ×
17), so the rendered output is unchanged; glyph data doubles in flash. The
`Opts:` header is left as generated, so it still names `--bpp 4` (the
converter's input) while the `Bpp:` line and the descriptor say 8 — and the
blinds-label glyph check keeps reading the `--range`. A converted file is
recognised and skipped, so the step is safe to repeat.

The `--range` lists of the two 140 px fonts are the glyphs the UI actually
renders. `font_helvetica_100.c` renders the configurable blinds group labels,
so it carries every glyph a label may reasonably need: printable ASCII and the
Czech letters present in `helvetica.otf` (`ÁÉÍÓÚÝŠŽáéíóúýšž`; the face has no
`ČĎĚŇŘŤŮ`, so those cannot be added without another face). The `esp_nest`
component reads this `--range` from the generated file's `Opts:` header and
rejects a label with any other character at `esphome config`. Extend a range
whenever new text needs a glyph the font lacks.
