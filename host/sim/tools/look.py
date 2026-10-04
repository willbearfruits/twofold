#!/usr/bin/env python3
"""look.py: quick viewer helper: PBM -> upscaled PNG (for design review)."""
import sys
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
s = int(sys.argv[3]) if len(sys.argv) > 3 else 6
im = Image.open(src).convert("L")
im = im.resize((im.width * s, im.height * s), Image.NEAREST)
im.save(dst)
