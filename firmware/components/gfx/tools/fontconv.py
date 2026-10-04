#!/usr/bin/env python3
"""
fontconv.py: convert the ASCII-art pixel fonts in ../fonts/*.txt into ../gfx_fonts.c.

The generated C file is committed, so the firmware/host builds never need Python.
Run after editing a font:   python3 firmware/components/gfx/tools/fontconv.py

Font file format (one font per file):
    # comment
    @font   small          C symbol becomes gfx_font_small
    @height 5              rows per glyph (<= 32)
    @cap    5              cap height (informational, used by layout code)
    @spacing 1             default px between glyphs (added to each glyph's advance)
    @space  3              advance of ' ' and of missing glyphs
    @flags  upper          lowercase falls back to uppercase
    @glyph A               glyph name: one character, or 0xNN; then exactly `height` art rows
    .##.                   '#' = on, '.' = off; all rows of a glyph have the same width
    ...
    @glyph 0x80 inf        optional comment after the code
    @kern A V -1           kerning pair (chars or 0xNN), added to the advance between them

Bitmap file format (e.g. the wordmark):
    @bitmap wordmark       C symbol becomes gfx_wordmark
    rows of '#'/'.'
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_DIR = os.path.join(HERE, "..", "fonts")
OUT = os.path.join(HERE, "..", "gfx_fonts.c")


def code_of(tok):
    if len(tok) == 1:
        return ord(tok)
    if tok.lower().startswith("0x"):
        return int(tok, 16)
    raise ValueError("bad glyph token %r" % tok)


def parse(path):
    lines = open(path, encoding="utf-8").read().splitlines()
    font = {"glyphs": {}, "kern": [], "spacing": 1, "space": 3, "flags": [], "cap": None}
    cur = None
    rows = []
    kind = None

    def flush():
        nonlocal cur, rows
        if cur is None:
            return
        if kind == "font":
            h = font["height"]
            if len(rows) != h:
                raise SystemExit("%s: glyph %r has %d rows, expected %d" % (path, chr(cur) if cur < 128 else hex(cur), len(rows), h))
            w = len(rows[0])
            for r in rows:
                if len(r) != w:
                    raise SystemExit("%s: glyph %r ragged rows" % (path, chr(cur) if cur < 128 else hex(cur)))
            font["glyphs"][cur] = rows
        cur = None
        rows = []

    for ln, raw in enumerate(lines, 1):
        line = raw.rstrip()
        if not line:
            continue
        is_art = set(line) <= set("#.")
        if line.startswith("#") and not is_art:
            continue  # comment ("# ..."); a row made only of '#'/'.' is always art
        if line.startswith("@"):
            tok = line[1:].split()
            key = tok[0]
            if key == "font":
                kind = "font"
                font["name"] = tok[1]
            elif key == "bitmap":
                kind = "bitmap"
                font["name"] = tok[1]
                cur = 0
                rows = []
            elif key in ("height", "cap", "spacing", "space"):
                font[key] = int(tok[1])
            elif key == "flags":
                font["flags"] = tok[1:]
            elif key == "glyph":
                flush()
                cur = code_of(tok[1])
                if cur in font["glyphs"]:
                    raise SystemExit("%s:%d duplicate glyph %s" % (path, ln, tok[1]))
            elif key == "kern":
                font["kern"].append((code_of(tok[1]), code_of(tok[2]), int(tok[3])))
            else:
                raise SystemExit("%s:%d unknown directive %s" % (path, ln, key))
            continue
        if cur is None:
            raise SystemExit("%s:%d art outside a glyph" % (path, ln))
        if not set(line) <= set("#."):
            raise SystemExit("%s:%d bad art row %r" % (path, ln, line))
        rows.append(line)
    if kind == "bitmap":
        font["bitmap"] = rows
        return "bitmap", font
    flush()
    return "font", font


def emit_font(f, out):
    name = f["name"]
    h = f["height"]
    bpc = (h + 7) // 8
    codes = sorted(f["glyphs"])
    first, last = min(min(codes), 0x20), max(codes)
    data = []
    table = []
    for c in range(first, last + 1):
        rows = f["glyphs"].get(c)
        if rows is None:
            table.append((0, 0, 0))
            continue
        w = len(rows[0])
        off = len(data)
        for x in range(w):
            col = 0
            for y in range(h):
                if rows[y][x] == "#":
                    col |= 1 << y
            for b in range(bpc):
                data.append((col >> (8 * b)) & 0xFF)
        table.append((off, w, w + f["spacing"]))
    if len(data) > 65535:
        raise SystemExit("font too large")
    kern = sorted(f["kern"])
    flags = " | ".join("GFX_FONT_UPPER_FALLBACK" for fl in f["flags"] if fl == "upper") or "0"
    out.append("/* ---- font %s: %d rows, %d glyphs, %d bytes ---- */" % (name, h, len(codes), len(data)))
    out.append("static const uint8_t %s_bits[%d] = {" % (name, max(1, len(data))))
    for i in range(0, len(data), 16):
        out.append("    " + ", ".join("0x%02x" % v for v in data[i:i + 16]) + ",")
    if not data:
        out.append("    0")
    out.append("};")
    out.append("static const gfx_glyph_t %s_glyphs[%d] = {" % (name, len(table)))
    for i, (off, w, adv) in enumerate(table):
        c = first + i
        label = chr(c) if 0x20 < c < 0x7F and chr(c) not in "\\*/" else "0x%02x" % c
        out.append("    { %d, %d, %d }, /* %s */" % (off, w, adv, label))
    out.append("};")
    if kern:
        out.append("static const gfx_kern_t %s_kern[%d] = {" % (name, len(kern)))
        for a, b, k in kern:
            out.append("    { 0x%02x, 0x%02x, %d }," % (a, b, k))
        out.append("};")
    out.append("const gfx_font_t gfx_font_%s = {" % name)
    out.append("    .h = %d, .bpc = %d, .first = 0x%02x, .last = 0x%02x, .cap = %d, .space = %d," %
               (h, bpc, first, last, f["cap"] if f["cap"] is not None else h, f["space"]))
    out.append("    .flags = %s, .nkern = %d, .glyphs = %s_glyphs, .bits = %s_bits, .kern = %s," %
               (flags, len(kern), name, name, ("%s_kern" % name) if kern else "NULL"))
    out.append("};")
    out.append("")
    return len(data) + 4 * len(table) + 3 * len(kern)


def emit_bitmap(f, out):
    name = f["name"]
    rows = f["bitmap"]
    w, h = len(rows[0]), len(rows)
    for r in rows:
        if len(r) != w:
            raise SystemExit("bitmap %s ragged" % name)
    stride = (w + 7) // 8
    data = []
    for r in rows:
        for bx in range(stride):
            v = 0
            for k in range(8):
                x = bx * 8 + k
                if x < w and r[x] == "#":
                    v |= 0x80 >> k
            data.append(v)
    out.append("/* ---- bitmap %s: %dx%d ---- */" % (name, w, h))
    out.append("static const uint8_t %s_bits[%d] = {" % (name, len(data)))
    for i in range(0, len(data), 16):
        out.append("    " + ", ".join("0x%02x" % v for v in data[i:i + 16]) + ",")
    out.append("};")
    out.append("const gfx_bitmap_t gfx_%s = { %d, %d, %s_bits };" % (name, w, h, name))
    out.append("")
    return len(data)


def main():
    files = sorted(fn for fn in os.listdir(FONT_DIR) if fn.endswith(".txt"))
    out = ["/* gfx_fonts.c: GENERATED by tools/fontconv.py from fonts/<name>.txt. Do not edit by hand. */",
           "#include \"gfx.h\"", "#include <stddef.h>", ""]
    total = 0
    for fn in files:
        kind, f = parse(os.path.join(FONT_DIR, fn))
        total += emit_font(f, out) if kind == "font" else emit_bitmap(f, out)
    out.append("/* total const data: ~%d bytes (flash) */" % total)
    with open(OUT, "w") as fh:
        fh.write("\n".join(out) + "\n")
    print("wrote %s (%d bytes of font data)" % (os.path.relpath(OUT), total))


if __name__ == "__main__":
    sys.exit(main())
