#!/usr/bin/env bash
# Shared helper: send a SysEx to the TWOFOLD over USB-MIDI and print the ASCII payload of its F0 7D <kind> reply.
# Usage: tools/twofold_sysex.sh "<request hex>" <reply-kind-hex> [port]
set -euo pipefail
REQ=$1; KIND=$2
PORT=${3:-$(amidi -l | awk '/TWOFOLD/{print $2; exit}')}
[ -n "$PORT" ] || { echo "no TWOFOLD MIDI port found (amidi -l)" >&2; exit 1; }
HEX=$( (sleep 0.3; amidi -p "$PORT" -S "$REQ") & amidi -p "$PORT" -d -t 2 2>/dev/null | tr -d '\n' )
echo "$HEX" | grep -oE "F0 7D $KIND( [0-9A-F]{2})* F7" | head -1 | sed "s/^F0 7D $KIND //; s/ F7$//" |
  python3 -c 'import sys; print(bytes.fromhex(sys.stdin.read().replace(" ","")).decode("ascii","replace"))'
