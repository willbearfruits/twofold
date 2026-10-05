#!/usr/bin/env bash
# Read the last-crash report from a TWOFOLD over USB-MIDI SysEx and decode its backtrace.
# Usage: tools/crash_report.sh [midi-port]   (default: the first port named TWOFOLD)
set -euo pipefail
cd "$(dirname "$0")/.."
PORT=${1:-$(amidi -l | awk '/TWOFOLD/{print $2; exit}')}
[ -n "$PORT" ] || { echo "no TWOFOLD MIDI port found (amidi -l)"; exit 1; }
HEX=$( (sleep 0.3; amidi -p "$PORT" -S 'F0 7D 01 F7') & amidi -p "$PORT" -d -t 2 2>/dev/null | tr -d '\n' )
TXT=$(echo "$HEX" | grep -oE "F0 7D 02( [0-9A-F]{2})* F7" | head -1 | sed 's/^F0 7D 02 //; s/ F7$//' | python3 -c 'import sys; print(bytes.fromhex(sys.stdin.read().replace(" ","")).decode("ascii","replace"))')
[ -n "$TXT" ] || { echo "no reply (is the firmware new enough?)"; exit 1; }
echo "$TXT"
PCS=$(echo "$TXT" | grep -oE "0x4[0-9a-f]{7}" || true)
if [ -n "$PCS" ]; then
  . ~/esp/esp-idf/export.sh >/dev/null 2>&1
  echo "--- backtrace (firmware/build/twofold.elf) ---"
  xtensa-esp32s3-elf-addr2line -pfiaC -e firmware/build/twofold.elf $PCS
fi
