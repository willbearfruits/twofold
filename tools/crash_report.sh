#!/usr/bin/env bash
# Last-crash report from a TWOFOLD over USB-MIDI SysEx, with the backtrace decoded against firmware/build/twofold.elf.
set -euo pipefail
cd "$(dirname "$0")/.."
TXT=$(tools/twofold_sysex.sh "F0 7D 01 F7" 02 "$@")
[ -n "$TXT" ] || { echo "no reply (is the firmware new enough?)"; exit 1; }
echo "$TXT"
PCS=$(echo "$TXT" | grep -oE "0x4[0-9a-f]{7}" || true)
if [ -n "$PCS" ]; then
  . ~/esp/esp-idf/export.sh >/dev/null 2>&1
  echo "--- backtrace (firmware/build/twofold.elf) ---"
  xtensa-esp32s3-elf-addr2line -pfiaC -e firmware/build/twofold.elf $PCS
fi
