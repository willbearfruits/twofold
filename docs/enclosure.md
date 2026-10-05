# Enclosure

`enclosure/twofold.scad` is the single parametric source (mm). Every component dimension is a named variable in
the block at the top of the file, with its source. Module sizes vary between sellers, so measure yours (checklist
below), edit those numbers only, then run `enclosure/check.sh` to re-check every clearance and re-export the STLs.

| file | |
|---|---|
| `enclosure/twofold.scad` | the model. `-D 'part="assembly"'`, `"exploded"`, `"tray"`, `"lid"`, `"knob"` (print orientation), `"check:*"` |
| `enclosure/check.sh [module\|panel]` | 8 collision checks (each intersection must be empty or a contact sliver < 1 mm³) + STL export |
| `enclosure/stl/twofold_{tray,lid,knob}.stl` | ready to slice, already in print orientation |
| `enclosure/renders/*.png` | assembly (front/back), exploded, the three parts as printed |

## Design
A 112 × 86 mm wedge: 26 mm high at the front, 40 mm at the back (9.25° top face), 7 mm corner radius. Two-tone:
a dark tray, a light lid, and a crisp V "reveal" at the 16 mm seam (two 45° chamfers). The OLED sits on the left
behind a 45° chamfered window (the recessed bezel frames exactly the active area + 0.4 mm). The knob is on the
right. All I/O is on the back: USB-C, the two input jacks, and the DAC's own output jack (`output_jack="module"`) or a third
panel jack (`"panel"`). The S3-Zero sits back-right with its USB-C in the back wall. Its antenna end points forward into an
8 mm keep-out zone, away from the DAC (centre-back) and ADC (left). `check:antenna-keepout` verifies that.

## Checks (both `output_jack` variants pass)
tray↔modules, lid↔modules, tray↔lid, knob↔lid, modules↔modules, antenna keep-out, USB-C plug overmold
(USB-IF max 12.35 × 6.5) through the back wall, and Dupont housings on the floor modules' headers vs the lid.

## Printing (no supports)
| part | orientation | notes |
|---|---|---|
| tray | floor on the bed | 0.2 mm layers, 3 walls, 15 % gyroid. Back-wall holes are round/rounded and print as short bridges. |
| lid | top face on the bed | gives the visible face a smooth bed finish. The window chamfer is 45°; bosses and lip point up. |
| knob | top on the bed | flutes are vertical, D-bore and skirt open upward |
PLA or PETG. The STL export from OpenSCAD 2021.01 may report "not a valid 2-manifold" for the lid (coincident faces at
the boss/plate joins). PrusaSlicer, Cura and Bambu Studio repair this automatically on import.

## Hardware BOM (enclosure)
| qty | part |
|---|---|
| 4 | M3 × 20 socket cap screw (ISO 4762), from below into the lid |
| 4 | M3 × 5.7 heat-set insert (Ruthex RX-M3x5.7 / CNC Kitchen), pressed into the lid bosses |
| 4 | M2 × 6 self-tapping screw + DIN 9021 M2 washer: OLED edge clamps |
| 4 | M2 × 5 self-tapping screw: PCM1808 module to its standoffs |
| 4 | 3M Bumpon SJ5302 (Ø7.9 × 2.2) in the foot recesses |
| 2 (3) | 3.5 mm stereo panel jack, M6 bushing (PJ-392 class); 3 with `output_jack="panel"` |
| 1 | EC11 encoder with push switch, **15 mm** D-shaft, M7 nut + washer |
ESP32-S3-Zero and GY-PCM5102 have no mounting holes. They snap into printed cradles (front snap lip / side lips).

## Board variant
The model was drawn for the Waveshare ESP32-S3-Zero (18 × 23.5 mm). The ESP32-S3 SuperMini used at bring-up is
about 18 × 22.5 mm, also with USB-C on a short edge and the antenna at the far end. Set `s3_pcb`, `s3_usb_overhang`
and `s3_top_h` from your board (and the RGB LED/buttons height), then run `check.sh`. The cradle and the USB-C
opening follow the variables.

## Measurement checklist (calipers, before printing)
| measure | variable(s) | why |
|---|---|---|
| PCM5102 module: PCB L × W, jack axis height above PCB, jack nose overhang | `dac_pcb`, `dac_jack_axis`, `dac_jack_nose` | jack must line up with the back-wall opening (axis height is not published) |
| PCM5102 jack axis distance from the PCB's left end | `dac_jack_u` | X position of the opening |
| PCM1808 module: PCB W × L, hole positions, tallest capacitor | `adc_pcb`, `adc_holes`, `adc_top_h` | standoffs, lid clearance |
| OLED: PCB W × H, glass W × H, glass height above PCB, active-area offset | `oled_pcb`, `oled_glass`, `oled_glass_h`, `oled_aa_c` | window must frame the active area; glass pocket fit |
| OLED: is it 4-pin with the header on the top edge? | `oled_hdr_c` | the header/pins keep-out |
| EC11: bushing thread length, shaft length from mounting face, anti-rotation tab position | `enc_shaft_l`, `enc_tab_r`, `enc_tab` | knob height / tab slot. If your shaft is 20 mm, increase `knob_gap` or cut the shaft |
| Panel jacks: bushing diameter, body depth incl. lugs | `pj_hole_d`, `pj_body_l` | hole size; clearance to the modules |
| S3-Zero: USB-C receptacle overhang past the PCB | `s3_usb_overhang` | USB-C opening alignment |
| Your USB-C cable's overmold | `usbc_overmold`, `usb_open` | plugs with fat overmolds need a bigger opening |
Then run `enclosure/check.sh` (and `enclosure/check.sh panel` if you use the third jack). It must print 8 × PASS.

## Wiring room
The checks reserve the 16.5 mm Dupont housing height above the floor modules' headers. For the OLED and the encoder
(on the lid), solder the wires directly or use right-angle headers: straight Dupont housings would land 1–3 mm
above the floor.
