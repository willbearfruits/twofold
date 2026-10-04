#!/usr/bin/env bash
# Collision checks + STL export for twofold.scad. Every "check:" view must be EMPTY.
# Usage: enclosure/check.sh [output_jack=module|panel]
set -uo pipefail
cd "$(dirname "$0")"
OJ=${1:-module}
fail=0
for c in tray-modules lid-modules tray-lid knob-lid modules-modules antenna-keepout usb-plug dupont-lid; do
  out=$(openscad -o /tmp/twofold_check_$$.stl -D "part=\"check:$c\"" -D "output_jack=\"$OJ\"" twofold.scad 2>&1)
  if echo "$out" | grep -q "Current top level object is empty"; then echo "PASS  $c (no intersection)"
  else
    # touching faces (a PCB resting on its standoff) give zero-volume slivers: grade by real volume
    read -r vol box <<<"$(python3 stl_vol.py /tmp/twofold_check_$$.stl)"
    if python3 -c "import sys; sys.exit(0 if $vol < 1.0 else 1)"; then echo "PASS  $c (contact only, ${vol} mm3)"
    else echo "FAIL  $c: ${vol} mm3 at x,y,z $box"; fail=1; fi
  fi
done
rm -f /tmp/twofold_check_$$.stl
if [ "$OJ" = module ]; then
  for p in tray lid knob; do
    openscad -o stl/twofold_$p.stl -D "part=\"$p\"" twofold.scad 2>&1 | grep -iE "error|warning" | grep -v "Viewall" || true
    echo "STL   stl/twofold_$p.stl $(du -h stl/twofold_$p.stl | cut -f1)"
  done
fi
exit $fail
