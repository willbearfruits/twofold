#!/usr/bin/env bash
# Live status of a TWOFOLD over USB-MIDI (no WiFi): CPU per DSP block, xruns, USB FIFO/feedback, heap, uptime.
cd "$(dirname "$0")"
./twofold_sysex.sh "F0 7D 03 F7" 04 "$@" | tr '|' '\n' | sed 's/^ //'
