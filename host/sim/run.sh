#!/usr/bin/env bash
# Build the TWOFOLD UI simulator, render every screen/transition/tour, write docs/img/ui/.
# Needs: cmake, a C11 compiler, python3 + Pillow.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/build/out"
cmake -S "$HERE" -B "$HERE/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$HERE/build" -j"$(nproc 2>/dev/null || echo 4)"
rm -rf "$OUT"
mkdir -p "$OUT"
"$HERE/build/twofold_sim" "$OUT" tour
"$HERE/build/twofold_sim" "$OUT" catalog
rm -rf "$ROOT/docs/img/ui/1x" "$ROOT/docs/img/ui/oled" "$ROOT/docs/img/ui/seq"
python3 "$HERE/tools/post.py" "$OUT" "$ROOT/docs/img/ui"
cp "$OUT"/timing_*.txt "$ROOT/docs/img/ui/" 2>/dev/null || true
echo "done: $ROOT/docs/img/ui"
