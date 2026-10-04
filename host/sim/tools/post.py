#!/usr/bin/env python3
"""
post.py: turn the simulator's raw frames (PBM 1:1 + manifest) into the docs images.

  docs/img/ui/1x/<shot>.png        exact 128x64 frames (lit = white)
  docs/img/ui/oled/<shot>.png      "OLED look": 6x, pixel grid, glow, bezel, panel brightness applied
  docs/img/ui/seq/<name>/fNNN.png  transition / animation frame sequences, 1:1 at 60 fps
  docs/img/ui/seq/<name>.png       filmstrip of the sequence (every frame, 2x, timestamps)
  docs/img/ui/tour.gif             scripted navigation tour at real timing (25 fps), OLED look 3x
  docs/img/ui/contact_sheet.png    every screen and state, OLED look 3x, labelled

usage: post.py <sim_out_dir> <docs_img_ui_dir>
"""
import os
import sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

SRC, DST = sys.argv[1], sys.argv[2]

LIT = (226, 240, 255)        # cool white OLED
OFF = (10, 12, 15)
GAP = (3, 4, 5)
BEZEL = (17, 18, 20)
BEZEL_EDGE = (44, 46, 50)


def load(name):
    im = Image.open(os.path.join(SRC, name + ".pbm")).convert("L")
    return im  # lit = 255


def intensity(level):
    # panel level (0..255, already perceptual) -> displayed intensity for the render
    level = max(0, min(255, int(level)))
    return 0.30 + 0.70 * (level / 255.0) ** 0.6


_grid_cache = {}


def grid_mask(w, h, scale):
    key = (w, h, scale)
    if key not in _grid_cache:
        g = max(1, scale // 6)
        m = Image.new("L", (w * scale, h * scale), 255)
        d = ImageDraw.Draw(m)
        if scale >= 3:
            for x in range(w):
                d.rectangle([x * scale + scale - g, 0, x * scale + scale - 1, h * scale - 1], fill=0)
            for y in range(h):
                d.rectangle([0, y * scale + scale - g, w * scale - 1, y * scale + scale - 1], fill=0)
        _grid_cache[key] = m
    return _grid_cache[key]


def oled(im, scale=6, level=200, bezel=True, glow=True):
    """1:1 frame -> upscaled OLED look (pixel grid, soft glow, bezel). Brightness follows the panel level."""
    w, h = im.size
    k = intensity(level)
    lit = tuple(int(c * k) for c in LIT)
    m = im.point(lambda v: 255 if v > 127 else 0).resize((w * scale, h * scale), Image.NEAREST)
    big = Image.composite(Image.new("RGB", m.size, lit), Image.new("RGB", m.size, OFF), m)
    big = Image.composite(big, Image.new("RGB", m.size, GAP), grid_mask(w, h, scale))
    if glow:
        blur = m.filter(ImageFilter.GaussianBlur(scale * 0.9)).point(lambda v: int(v * 0.30))
        big = ImageChops.add(big, Image.composite(Image.new("RGB", m.size, lit), Image.new("RGB", m.size, (0, 0, 0)), blur))
    if not bezel:
        return big
    pad = max(8, scale * 4)
    out = Image.new("RGB", (big.width + 2 * pad, big.height + 2 * pad), (0, 0, 0))
    d = ImageDraw.Draw(out)
    d.rounded_rectangle([0, 0, out.width - 1, out.height - 1], radius=pad, fill=BEZEL, outline=BEZEL_EDGE, width=max(1, scale // 3))
    d.rectangle([pad - 2, pad - 2, pad + big.width + 1, pad + big.height + 1], fill=(0, 0, 0))
    out.paste(big, (pad, pad))
    return out


def one_x(im):
    return im.convert("RGB").point(lambda v: 255 if v > 127 else 0)


def main():
    man = []
    for fn in sorted(os.listdir(SRC)):
        if fn.startswith("manifest") and fn.endswith(".txt"):
            man += [l.split() for l in open(os.path.join(SRC, fn)) if l.strip()]
    os.makedirs(os.path.join(DST, "1x"), exist_ok=True)
    os.makedirs(os.path.join(DST, "oled"), exist_ok=True)
    shots, seqs, gif = [], {}, []
    for row in man:
        kind, name, level, t = row[:4]
        if kind == "shot":
            shots.append((name, int(level)))
        elif kind == "seq":
            seqs.setdefault(name.split("/")[1], []).append((name, int(level), int(t)))
        elif kind == "gif":
            gif.append((name, int(level)))

    # shots
    for name, level in shots:
        im = load(name)
        one_x(im).save(os.path.join(DST, "1x", name + ".png"))
        oled(im, 6, level).save(os.path.join(DST, "oled", name + ".png"))

    # sequences
    try:
        font = ImageFont.load_default()
    except Exception:
        font = None
    for sname, frames in seqs.items():
        d = os.path.join(DST, "seq", sname)
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):
            os.remove(os.path.join(d, f))
        ims = []
        for name, level, t in frames:
            im = load(name)
            one_x(im).save(os.path.join(d, os.path.basename(name) + ".png"))
            ims.append((im, t))
        cols = 6
        sc = 2
        cw, ch = 128 * sc + 8, 64 * sc + 20
        rows = (len(ims) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * cw + 8, rows * ch + 8), (24, 24, 26))
        dr = ImageDraw.Draw(sheet)
        t0 = ims[0][1] if ims else 0
        for i, (im, t) in enumerate(ims):
            x, y = 8 + (i % cols) * cw, 8 + (i // cols) * ch
            sheet.paste(oled(im, sc, 220, bezel=False, glow=False), (x, y))
            dr.text((x, y + 64 * sc + 3), "+%d ms" % (t - t0), fill=(150, 150, 155), font=font)
        sheet.save(os.path.join(DST, "seq", sname + ".png"))

    # tour gif (3x OLED look, real timing: 40 ms per frame)
    if gif:
        frames = []
        for name, level in gif:
            frames.append(oled(load(name), 3, level, bezel=True, glow=True).convert("P", palette=Image.ADAPTIVE, colors=32))
        frames[0].save(os.path.join(DST, "tour.gif"), save_all=True, append_images=frames[1:], duration=40, loop=0, optimize=True, disposal=1)

    # contact sheet
    sc = 3
    tiles = [(n, oled(load(n), sc, l)) for n, l in shots]
    tw, th = tiles[0][1].size
    cols = 5
    lab = 18
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (tw + 12) + 12, rows * (th + lab + 12) + 12 + 30), (12, 12, 13))
    dr = ImageDraw.Draw(sheet)
    dr.text((12, 10), "TWOFOLD UI - every screen and state (host simulator, 128x64 1-bit, OLED look 3x)", fill=(200, 200, 205), font=font)
    for i, (n, t) in enumerate(tiles):
        x = 12 + (i % cols) * (tw + 12)
        y = 40 + (i // cols) * (th + lab + 12)
        sheet.paste(t, (x, y))
        dr.text((x + 4, y + th + 3), n, fill=(160, 160, 166), font=font)
    sheet.save(os.path.join(DST, "contact_sheet.png"))
    print("post: %d shots, %d sequences, %d gif frames -> %s" % (len(shots), len(seqs), len(gif), DST))


if __name__ == "__main__":
    main()
