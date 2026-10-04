#!/usr/bin/env python3
"""Deterministic gzip of the web app (mtime 0, no file name), used by the net component build.
Usage: gzip_web.py <in.html> <out.gz>
Also usable by hand to check the size: python3 gzip_web.py index.html /tmp/x.gz
"""
import gzip
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as f:
        raw = f.read()
    gz = gzip.compress(raw, compresslevel=9, mtime=0)
    with open(dst, "wb") as f:
        f.write(gz)
    print(f"web app: {len(raw)} bytes -> {len(gz)} bytes gzip")
    return 0


if __name__ == "__main__":
    sys.exit(main())
