#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the native-size display (numeric) fonts for big dashboard values.

Rasterises a small charset of an OFL font with FreeType (via ctypes, no Python
packages needed) at four cap heights and writes a compact C++ source:

    firmware/components/eego_ui/display_font_data.cpp

Glyphs are 2-bit coverage (0 = none .. 3 = full), run-length encoded as bytes
(level << 6) | (run - 1). The renderer draws level 3 black, level 2 dark gray
and level 1 light gray via drawPixelGray, so a fast (B/W) refresh shows the
same glyph thresholded at ~50 % coverage and a full refresh shows anti-aliased
edges.

Default font: Noto Sans CJK SC Bold (SIL OFL 1.1), Latin/digit glyphs only.

usage: gen_digit_font.py [--font PATH] [--out PATH] [--caps 20,28,48,80]
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import sys
from pathlib import Path

FW = Path(__file__).resolve().parents[1]
DEFAULT_FONT = "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc"
DEFAULT_OUT = FW / "components" / "eego_ui" / "display_font_data.cpp"

# Numbers, signs and separators for every size; letters (units such as GB,
# ms, req/s, km/h) only where the budget allows (see LETTER_SIZES).
SYMBOLS = "0123456789.,%+-:/()# −°·"
LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"

FT_LOAD_RENDER = 1 << 2
FT_LOAD_TARGET_LIGHT = 1 << 16


class FT_Generic(ctypes.Structure):
    _fields_ = [("data", ctypes.c_void_p), ("finalizer", ctypes.c_void_p)]


class FT_Vector(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class FT_BBox(ctypes.Structure):
    _fields_ = [("xMin", ctypes.c_long), ("yMin", ctypes.c_long),
                ("xMax", ctypes.c_long), ("yMax", ctypes.c_long)]


class FT_Glyph_Metrics(ctypes.Structure):
    _fields_ = [(name, ctypes.c_long) for name in
                ("width", "height", "horiBearingX", "horiBearingY", "horiAdvance",
                 "vertBearingX", "vertBearingY", "vertAdvance")]


class FT_Bitmap(ctypes.Structure):
    _fields_ = [("rows", ctypes.c_uint), ("width", ctypes.c_uint), ("pitch", ctypes.c_int),
                ("buffer", ctypes.POINTER(ctypes.c_ubyte)), ("num_grays", ctypes.c_ushort),
                ("pixel_mode", ctypes.c_ubyte), ("palette_mode", ctypes.c_ubyte),
                ("palette", ctypes.c_void_p)]


class FT_GlyphSlotRec(ctypes.Structure):
    _fields_ = [("library", ctypes.c_void_p), ("face", ctypes.c_void_p), ("next", ctypes.c_void_p),
                ("glyph_index", ctypes.c_uint), ("generic", FT_Generic),
                ("metrics", FT_Glyph_Metrics), ("linearHoriAdvance", ctypes.c_long),
                ("linearVertAdvance", ctypes.c_long), ("advance", FT_Vector),
                ("format", ctypes.c_uint), ("bitmap", FT_Bitmap),
                ("bitmap_left", ctypes.c_int), ("bitmap_top", ctypes.c_int)]


class FT_FaceRec(ctypes.Structure):
    _fields_ = [("num_faces", ctypes.c_long), ("face_index", ctypes.c_long),
                ("face_flags", ctypes.c_long), ("style_flags", ctypes.c_long),
                ("num_glyphs", ctypes.c_long), ("family_name", ctypes.c_char_p),
                ("style_name", ctypes.c_char_p), ("num_fixed_sizes", ctypes.c_int),
                ("available_sizes", ctypes.c_void_p), ("num_charmaps", ctypes.c_int),
                ("charmaps", ctypes.c_void_p), ("generic", FT_Generic), ("bbox", FT_BBox),
                ("units_per_EM", ctypes.c_ushort), ("ascender", ctypes.c_short),
                ("descender", ctypes.c_short), ("height", ctypes.c_short),
                ("max_advance_width", ctypes.c_short), ("max_advance_height", ctypes.c_short),
                ("underline_position", ctypes.c_short), ("underline_thickness", ctypes.c_short),
                ("glyph", ctypes.POINTER(FT_GlyphSlotRec)), ("size", ctypes.c_void_p)]


class FreeType:
    def __init__(self):
        path = ctypes.util.find_library("freetype")
        if not path:
            sys.exit("libfreetype not found")
        self.ft = ctypes.CDLL(path)
        self.lib = ctypes.c_void_p()
        if self.ft.FT_Init_FreeType(ctypes.byref(self.lib)):
            sys.exit("FT_Init_FreeType failed")
        self.ft.FT_New_Face.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_long,
                                        ctypes.POINTER(ctypes.POINTER(FT_FaceRec))]
        self.ft.FT_Set_Pixel_Sizes.argtypes = [ctypes.POINTER(FT_FaceRec), ctypes.c_uint, ctypes.c_uint]
        self.ft.FT_Load_Char.argtypes = [ctypes.POINTER(FT_FaceRec), ctypes.c_ulong, ctypes.c_int32]
        self.ft.FT_Get_Char_Index.argtypes = [ctypes.POINTER(FT_FaceRec), ctypes.c_ulong]
        self.ft.FT_Done_Face.argtypes = [ctypes.POINTER(FT_FaceRec)]

    def open(self, path, prefer=b" SC"):
        face = ctypes.POINTER(FT_FaceRec)()
        if self.ft.FT_New_Face(self.lib, path.encode(), 0, ctypes.byref(face)):
            sys.exit(f"cannot open {path}")
        count = face.contents.num_faces
        for index in range(count):
            candidate = ctypes.POINTER(FT_FaceRec)()
            if self.ft.FT_New_Face(self.lib, path.encode(), index, ctypes.byref(candidate)):
                continue
            if prefer in (candidate.contents.family_name or b""):
                self.ft.FT_Done_Face(face)
                return candidate
            self.ft.FT_Done_Face(candidate)
        return face

    def render(self, face, codepoint):
        """Returns (width, height, left, top, advance, coverage rows) or None."""
        if self.ft.FT_Get_Char_Index(face, codepoint) == 0:
            return None
        if self.ft.FT_Load_Char(face, codepoint, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT):
            return None
        slot = face.contents.glyph.contents
        bm = slot.bitmap
        rows = []
        for y in range(bm.rows):
            rows.append([bm.buffer[y * bm.pitch + x] for x in range(bm.width)])
        advance = (slot.advance.x + 32) >> 6
        return bm.width, bm.rows, slot.bitmap_left, slot.bitmap_top, advance, rows


def level(coverage):
    # Fast refresh shows levels 2-3 (gray < 8) black: ~50 % coverage threshold.
    return 0 if coverage < 48 else 1 if coverage < 128 else 2 if coverage < 208 else 3


def encode(width, height, rows):
    levels = [level(c) for row in rows for c in row]
    out = bytearray()
    i = 0
    while i < len(levels):
        value = levels[i]
        run = 1
        while i + run < len(levels) and levels[i + run] == value and run < 64:
            run += 1
        out.append((value << 6) | (run - 1))
        i += run
    return bytes(out)


def pixel_size_for_cap(ft, face, cap):
    size = cap
    for _ in range(40):
        ft.ft.FT_Set_Pixel_Sizes(face, 0, size)
        glyph = ft.render(face, ord("0"))
        height = glyph[3] if glyph else size
        if height >= cap:
            return size
        size += 1
    return size


def build(ft, face, cap, charset):
    size = pixel_size_for_cap(ft, face, cap)
    ft.ft.FT_Set_Pixel_Sizes(face, 0, size)
    glyphs = []
    # Metrics of the number line: digits and signs (letters may poke out a few px).
    ascent = 0
    descent = 0
    numeric = set("0123456789.,%+-")
    for ch in sorted(set(charset), key=ord):
        glyph = ft.render(face, ord(ch))
        if glyph is None:
            continue
        width, height, left, top, advance, rows = glyph
        glyphs.append((ord(ch), width, height, left, top, advance, encode(width, height, rows)))
        if height and ch in numeric:
            ascent = max(ascent, top)
            descent = max(descent, height - top)
    return size, ascent, descent, glyphs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", default=DEFAULT_FONT)
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    parser.add_argument("--caps", default="20,28,48,80")
    args = parser.parse_args()
    caps = [int(value) for value in args.caps.split(",")]
    ft = FreeType()
    face = ft.open(args.font)
    family = (face.contents.family_name or b"").decode()
    style = (face.contents.style_name or b"").decode()

    lines = []
    fonts = []
    total = 0
    for index, cap in enumerate(caps):
        charset = SYMBOLS + LETTERS
        size, ascent, descent, glyphs = build(ft, face, cap, charset)
        blob = bytearray()
        table = []
        for codepoint, width, height, left, top, advance, data in glyphs:
            if not (0 <= width < 256 and 0 <= height < 256 and -128 <= left < 128 and
                    -128 <= top < 128 and 0 <= advance < 256 and len(blob) < (1 << 24)):
                sys.exit(f"glyph U+{codepoint:04X} out of range at cap {cap}")
            table.append(f"    {{0x{codepoint:04X}, {len(blob)}, {width}, {height}, {left}, {top}, {advance}}},")
            blob += data
        total += len(blob) + len(table) * 12
        lines.append(f"// cap {cap} px: {size} px em, {len(glyphs)} glyphs, {len(blob)} bytes")
        lines.append(f"const DisplayGlyph kGlyphs{index}[] = {{")
        lines += table
        lines.append("};")
        lines.append(f"const uint8_t kData{index}[] = {{")
        for start in range(0, len(blob), 20):
            lines.append("    " + ",".join(f"0x{b:02x}" for b in blob[start:start + 20]) + ",")
        lines.append("};")
        lines.append("")
        fonts.append(f"    {{{cap}, {ascent}, {descent}, {len(glyphs)}, kGlyphs{index}, kData{index}}},")

    source = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// GENERATED by firmware/tools/gen_digit_font.py -- do not edit.",
        f"// Glyphs rasterised from {family} {style} (SIL Open Font License 1.1),",
        "// see THIRD_PARTY_NOTICES.md. 2-bit coverage, RLE: (level << 6) | (run - 1).",
        f"// Total {total} bytes.",
        "#include \"display_font_data.h\"",
        "",
        "namespace eego::display_font_data {",
        "namespace {",
        "",
        *lines,
        "}  // namespace",
        "",
        "const DisplayFontData kFonts[] = {",
        *fonts,
        "};",
        "const size_t kFontCount = sizeof(kFonts) / sizeof(kFonts[0]);",
        "",
        "}  // namespace eego::display_font_data",
        "",
    ]
    Path(args.out).write_text("\n".join(source), encoding="utf-8")
    print(f"{family} {style}: {len(fonts)} sizes, {total} bytes -> {args.out}")


if __name__ == "__main__":
    main()
