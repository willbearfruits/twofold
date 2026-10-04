#!/usr/bin/env bash
# Build and run the TWOFOLD DSP host test-suite; writes docs/test-results/ (report + plots).
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
OUT="$ROOT/docs/test-results"
RAW="$OUT/raw"
mkdir -p "$RAW"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)" >/dev/null
T=./build/dsp_tests
LOG="$RAW/results.txt"
: > "$LOG"
run() { "$T" "$@" | tee -a "$LOG" | grep -E '^RESULT' || true; }

for c in 0 1 2; do run eq$c "$RAW"; done
run limiter "$RAW"
run limtransp_off "$RAW" >/dev/null; run limtransp_on "$RAW"
for d in 1.0 2.2 6.0; do run reverb$d "$RAW"; done
for d in 00 01 21 60; do run delay$d "$RAW"; done
for a in "5000 12 -12" "9000 12 -12" "9000 6 -6" "9000 24 -6" "13000 24 -6"; do run satalias "$RAW" $a; done
run dither "$RAW"; run bitperfect "$RAW"
NP=$("$T" nparams "$RAW")
: > "$RAW/clicks.txt"
for id in $(seq 0 $((NP - 1))); do "$T" click1 "$RAW" "$id" | grep CLICKWORST >> "$RAW/clicks.txt"; done
python3 - "$RAW/clicks.txt" >> "$LOG" <<'PY'
import sys
rows=[l.split() for l in open(sys.argv[1])]
w=max(rows,key=lambda r: float(r[2]))
ok=float(w[2])<=6.0
print(f"RESULT {'PASS' if ok else 'FAIL'} | click test: {len(rows)} parameters stepped def->max->min->def under a 1 kHz+100 Hz tone; worst 1 ms HF spike {float(w[2]):.2f} dB over its neighbourhood ({w[1]}, {w[3]}), limit 6 dB")
PY
tail -1 "$LOG"
run clickpresets "$RAW"
run bench "$RAW"
python3 plots.py "$RAW" "$OUT"
grep -q "RESULT FAIL" "$LOG" && { echo "SOME TESTS FAILED"; exit 1; } || echo "ALL TESTS PASSED"
