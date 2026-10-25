#!/usr/bin/env python3
"""Convert an lv_font_conv C font (--format lvgl) into a ZXF1 font pack.

ZXF1 fonts live on the SD card (/fonts/*.zxf) and are read glyph-by-glyph by
src/ui/sd_font.cpp, so large CJK fonts do not have to be linked into flash or
loaded into RAM.

Regenerate the Korean UI font:

    npx lv_font_conv --size 14 --bpp 2 --format lvgl \\
      --font NotoSansCJKkr-Regular.otf --range 0x20-0x7F --range 0x1100-0x11FF \\
      --range 0x3130-0x318F --range 0xAC00-0xD7A3 --no-kerning \\
      --lv-font-name lv_font_korean_ui_14 -o /tmp/korean_ui_14.c
    python3 scripts/lvgl_font_to_zxf.py /tmp/korean_ui_14.c assets/fonts/korean_ui_14.zxf

Layout (little endian):
    header   "<4sBBBBhhbbHIIIIHH" (36 bytes, see HEADER below)
    cmaps    cmap_count  x "<IHH"    range_start, range_length, glyph_id_start
    glyphs   glyph_count x "<IHBBbb" bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y
    bitmaps  bitmap_size bytes (lv_font_conv glyph_bitmap, compressed or plain)
"""

import re
import struct
import sys

HEADER = struct.Struct("<4sBBBBhhbbHIIIIHH")
CMAP = struct.Struct("<IHH")
GLYPH = struct.Struct("<IHBBbb")
VERSION = 1


def section(source, start_marker, end_marker="};"):
    start = source.index(start_marker)
    end = source.index(end_marker, start)
    return source[start + len(start_marker):end]


def int_field(text, name):
    match = re.search(r"\." + name + r"\s*=\s*(-?\w+)", text)
    if not match:
        raise ValueError(f"missing .{name}")
    return int(match.group(1), 0)


def main(src_path, out_path):
    source = open(src_path, encoding="utf-8").read()

    bitmap_text = section(source, "glyph_bitmap[] = {")
    bitmap_text = re.sub(r"/\*.*?\*/", "", bitmap_text, flags=re.S)
    bitmap = bytes(int(tok, 16) for tok in re.findall(r"0x[0-9a-fA-F]+", bitmap_text))

    glyph_text = section(source, "glyph_dsc[] = {")
    glyphs = []
    for entry in re.findall(r"\{([^{}]*)\}", glyph_text):
        glyphs.append(tuple(int_field(entry, f) for f in
                            ("bitmap_index", "adv_w", "box_w", "box_h", "ofs_x", "ofs_y")))

    cmap_text = section(source, "cmaps[] =")
    cmaps = []
    for entry in re.findall(r"\{([^{}]*)\}", cmap_text):
        kind = re.search(r"\.type\s*=\s*(\w+)", entry).group(1)
        if kind != "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY":
            raise ValueError(f"unsupported cmap type {kind}")
        cmaps.append((int_field(entry, "range_start"),
                      int_field(entry, "range_length"),
                      int_field(entry, "glyph_id_start")))

    font_dsc = section(source, "font_dsc = {")
    if "kern_dsc = NULL" not in font_dsc:
        raise ValueError("kerning is not supported (use --no-kerning)")
    bpp = int_field(font_dsc, "bpp")
    bitmap_format = int_field(font_dsc, "bitmap_format")

    public = section(source, "lv_font_korean_ui_14 = {" if "lv_font_korean_ui_14 = {" in source
                     else re.search(r"lv_font_t \w+ = \{", source).group(0))
    line_height = int_field(public, "line_height")
    base_line = int_field(public, "base_line")
    underline_position = int_field(public, "underline_position")
    underline_thickness = int_field(public, "underline_thickness")

    max_len = 0
    for gid, glyph in enumerate(glyphs):
        end = glyphs[gid + 1][0] if gid + 1 < len(glyphs) else len(bitmap)
        if end < glyph[0]:
            raise ValueError(f"glyph {gid}: bitmap_index not monotonic")
        max_len = max(max_len, end - glyph[0])

    glyph_offset = HEADER.size + CMAP.size * len(cmaps)
    bitmap_offset = glyph_offset + GLYPH.size * len(glyphs)
    with open(out_path, "wb") as out:
        out.write(HEADER.pack(b"ZXF1", VERSION, bpp, bitmap_format, 0,
                              line_height, base_line, underline_position, underline_thickness,
                              len(cmaps), len(glyphs), glyph_offset, bitmap_offset,
                              len(bitmap), max_len, 0))
        for cmap in cmaps:
            out.write(CMAP.pack(*cmap))
        for glyph in glyphs:
            out.write(GLYPH.pack(*glyph))
        out.write(bitmap)

    print(f"{out_path}: {len(glyphs)} glyphs, {len(cmaps)} cmaps, "
          f"{len(bitmap)} bitmap bytes, max glyph {max_len} bytes")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: lvgl_font_to_zxf.py <font.c> <out.zxf>")
    main(sys.argv[1], sys.argv[2])
