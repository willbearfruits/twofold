// =====================================================================================================
// TWOFOLD enclosure: parametric OpenSCAD, millimetres.
//
//   part = "assembly" | "exploded" | "tray" | "lid" | "knob" | "check:<name>"   (set from the CLI: -D 'part="tray"')
//
// Two printed shells (tray + lid) that print WITHOUT supports, plus a matching knob:
//   tray  floor on the bed. Walls, module cradles/standoffs, screw pillars, foot recesses, back-panel I/O.
//   lid   tilted top face DOWN on the bed (lid_print() rotates it). 45-degree window chamfer = recessed bezel,
//         OLED glass pocket + clamp bosses, EC11 bushing hole with anti-rotation slot, M3 heat-set insert bosses.
//   knob  top face down on the bed. Fine flutes, indicator groove, D-bore, skirt that hides the M7 nut.
// Fastening: 4 x M3x20 cap screws from the underside (counterbored, hidden by the feet line) into
// M3x5.7 heat-set inserts in the lid bosses. Modules: M2 self-tapping screws / snap cradles.
//
// Frame: X right (seen from the front), Y toward the back (the I/O side), Z up; origin = front-left
// corner of the footprint on the underside. The top face is the plane z = h_front + y * tan(tilt).
//
// EVERY component dimension is a variable below, with its source. Module sizes vary between sellers:
// measure yours (docs/enclosure.md has the checklist) and edit the numbers in this block only.
// =====================================================================================================

part = "assembly";
output_jack = "module";     // "module": expose the PCM5102 module's own 3.5 mm jack | "panel": third panel jack
show_modules = true;        // draw module stand-ins in assembly / exploded views
explode = 28;               // exploded-view lift (mm)
$fa = 4; $fs = 0.4;
eps = 0.01;

// ---------------------------------------------------------------- case (design choices)
case_w   = 112;     // outside width (X)
case_d   = 86;      // outside depth (Y)
h_front  = 26;      // top face height at the front edge
h_back   = 40;      // top face height at the back edge  -> tilt = atan((h_back - h_front) / case_d) = 9.25 deg
seam_z   = 16;      // tray / lid split height (constant all round)
corner_r = 7;       // plan-view corner radius
wall     = 2.4;     // walls: six 0.4 mm perimeters
floor_t  = 2.4;     // tray floor
lid_t    = 3.0;     // lid plate thickness (normal to the top face)
edge_ch  = 1.2;     // 45-degree chamfer round the top face
bot_ch   = 0.6;     // 45-degree chamfer round the underside (hides elephant's foot)
reveal   = 0.8;     // the accent: 45-degree chamfers either side of the seam form a crisp V line
lip_t    = 1.2;     // lid locating lip (three perimeters)
lip_h    = 3.0;     // lip depth below the seam
lip_cl   = 0.3;     // lip-to-wall clearance (FDM fit)

// ---------------------------------------------------------------- fasteners (ISO 4762 M3, Ruthex M3x5.7 inserts)
screw_clear = 3.4;  // ISO 273 medium clearance, M3
cb_d   = 6.6;       // counterbore: DIN 974-1 6.5 for M3 + 0.1 print allowance
cb_h   = 3.5;       // counterbore depth: head k = 3.0 + 0.5
pil_in = 8.5;       // pillar axis from the outside edges (X and Y)
pil_d  = 7.0;       // tray pillar (sleeve) diameter
pil_gap = 0.5;      // pillar top below the seam: the lid bears on the rim only
ins_od = 4.6;       // insert OD (Ruthex RX-M3x5.7)
ins_l  = 5.7;       // insert length
ins_hole_d = 4.0;   // recommended hole for printed parts (Ruthex)
ins_hole_depth = 7.5; // >= L + 1, room for displaced plastic
ins_wall = 1.8;     // wall round the insert (>= 1.6)

// ---------------------------------------------------------------- feet (3M Bumpon SJ5302)
foot_recess_d = 8.9;   // 7.9 foot + 1.0
foot_recess_h = 1.0;   // SJ5302 is 2.2 tall: 1.2 stands proud
foot_inset = [18, 16]; // foot centres from the sides (X) and front/back (Y), clear of the counterbores

// ---------------------------------------------------------------- modules: shared
mod_so_h = 4.0;     // PCB underside above the floor (solder tails <= 2.5 + 1.5)
mod_so_d = 5.5;     // standoff diameter
m2_pilot = 1.7;     // M2 self-tapping pilot in PLA/PETG
dupont_h = 16.5;    // Dupont housing above a header: 2.5 + 14.0 (Pololu 1900 drawing)

// ---------------------------------------------------------------- Waveshare ESP32-S3-Zero (Waveshare drawing + STEP v2)
s3_pcb = [18.0, 23.5, 1.6];      // W x L x T
s3_usb = [9.0, 7.35, 3.35];      // USB-C receptacle W x depth x H
s3_usb_axis = 1.67;              // USB-C axis above the PCB top
s3_usb_overhang = 1.22;          // receptacle face past the PCB edge
s3_top_h = 1.6;                  // tallest top component except USB-C
s3_ant_zone = 8.0;               // ceramic antenna at the far end (opposite USB-C): keep this length clear
s3_x = 90;                       // design: module centre / USB-C opening in X
s3_wall_gap = 0.3;               // PCB back edge to back wall
s3_cl = 0.4;                     // cradle clearance round the PCB
usbc_overmold = [12.35, 6.5];    // USB-IF Type-C compliance doc, plug overmold max W x H
usb_open = [13.2, 7.4];          // back-wall opening: overmold + clearance

// ---------------------------------------------------------------- PCM5102A "GY-PCM5102" (footprints: clumsyMIDI, picogus; photos)
dac_pcb = [32.2, 17.4, 1.6];     // L (X) x W (Y) x T: largest of the sources
dac_jack_u = 5.6;                // jack axis from the PCB's left end
dac_jack_axis = 2.75;            // jack axis above the PCB top (NOT published: measure!)
dac_jack_body = [6.1, 12.5, 5.5];// jack W x L x H
dac_jack_nose = 1.8;             // jack nose past the PCB back edge
dac_open_d = 11.6;               // plug-grip opening (largest common grip 10.8 + 0.8)
dac_top_h = 1.6;                 // tallest SMD part
dac_jack_x = 50;                 // design: module jack axis in X
dac_cl = 0.3;                    // cradle clearance
dac_panel_setback = 23.4;        // output_jack == "panel": module moves forward to clear the third panel jack

// ---------------------------------------------------------------- PCM1808 module (Fritzing / pecostm32 / Orange-Murker)
adc_pcb = [27.7, 34.6, 1.6];     // W (X) x L (Y) x T
adc_holes = [[2.6, 2.75], [25.1, 2.75], [2.6, 31.85], [25.1, 31.85]];  // from the PCB front-left corner
adc_top_h = 9.0;                 // electrolytic caps above the PCB top
adc_at = [9.6, 24.0];            // design: PCB front-left corner (x, y)

// ---------------------------------------------------------------- 3.5 mm stereo panel jack (PJ-392 class, threaded bushing + nut)
pj_hole_d = 6.3;                 // panel hole for the M6 bushing
pj_body_d = 8.0;                 // body behind the panel
pj_body_l = 22.8;                // overall depth behind the panel incl. solder lugs
pj_nut = [8.0, 2.0];             // nut across flats x thickness (outside)
pj_in_x = [18, 32];              // design: input jack axes (X)
pj_out_x = 60;                   // design: output jack axis when output_jack == "panel"

// ---------------------------------------------------------------- 0.96" 128x64 I2C OLED (Univision UG-2864 / Winstar WEA012864)
oled_pcb = [27.8, 27.8, 1.0];    // W x H x T (sellers 27.0 - 27.8)
oled_glass = [26.7, 19.26];      // glass W x H
oled_glass_h = 1.8;              // glass front above the PCB top (with tape)
oled_aa = [21.744, 10.864];      // active area
oled_aa_c = [0, 1.76];           // active-area centre from the PCB centre (+v toward the header)
oled_hdr_c = 1.65;               // header row from the header edge
oled_pin_l = 9.0;                // header pins behind the PCB
oled_at = [46, 30];              // design: active-area centre in the lid frame (u, v)
oled_skin = 1.6;                 // plate thickness at the window = bezel chamfer depth
oled_glass_gap = 0.3;            // glass front below the pocket ceiling
oled_glass_cl = 0.2;             // glass pocket clearance per side
oled_win_margin = 0.4;           // window beyond the active area per side
oled_boss_d = 5.0;               // clamp boss (M2 + washer laps the PCB edge)
oled_clamp_dy = 12.4;            // clamp screws this far from the PCB centre line (beyond the glass)
oled_clamp_lap = 1.3;            // clamp boss reaches this far over the PCB edge (notched round the PCB)

// ---------------------------------------------------------------- EC11 encoder (Alps EC11E series drawing)
enc_body = [12.0, 12.0, 6.5];    // body W x D x H (below the mounting face)
enc_pins_h = 3.5;                // terminal length below the body
enc_bush_d = 7.0;                // M7 x 0.75 bushing
enc_hole_cl = 0.4;               // hole = bushing + clearance
enc_shaft_d = 6.0;               // D-shaft diameter
enc_shaft_flat = 4.5;            // across the flat
enc_shaft_l = 15.0;              // shaft length above the mounting face (choose the 15 mm version)
enc_tab = [1.2, 2.2, 0.8];       // anti-rotation tab W x L x H on the mounting face
enc_tab_r = 7.6;                 // tab centre from the shaft axis (measure: 6.5 - 8 between makers)
enc_panel_t = 1.6;               // plate thickness at the encoder (thread 5 mm: washer 0.5 + nut 2 fit)
enc_at = [92, 30];               // design: shaft axis in the lid frame (u, v)
enc_nut = [10.0, 2.0];           // M7 nut across corners x thickness, + washer below it
enc_washer_t = 0.5;

// ---------------------------------------------------------------- knob
knob_d = 25;  knob_h = 13;  knob_ch = 1.0;  knob_gap = 1.0;      // lower edge 1 mm above the panel
knob_skirt_id = 14.6;  knob_skirt_h = 4.4;                       // hides washer + nut
knob_bore_cl = 0.15;  knob_flutes = 48;  knob_flute_w = 0.8;
knob_ind_w = 1.0;  knob_ind_depth = 0.6;

// =====================================================================================================
// derived values
// =====================================================================================================
tilt = atan((h_back - h_front) / case_d);
function top_z(y) = h_front + y * tan(tilt);
y_bi = case_d - wall;                                   // back wall inner face
pcb_z = floor_t + mod_so_h;                             // floor modules' PCB underside
s3_y0 = y_bi - s3_wall_gap - s3_pcb[1];
s3_usb_z = pcb_z + s3_pcb[2] + s3_usb_axis;
dac_back = output_jack == "module" ? y_bi - dac_cl - dac_jack_nose : y_bi - dac_panel_setback;
dac_x0 = dac_jack_x - dac_jack_u;  dac_y0 = dac_back - dac_pcb[1];
dac_jack_z = pcb_z + dac_pcb[2] + dac_jack_axis;
pj_z = dac_jack_z;                                      // all back-panel jacks on one line
pj_xs = output_jack == "panel" ? concat(pj_in_x, [pj_out_x]) : pj_in_x;
pillars = [[pil_in, pil_in], [case_w - pil_in, pil_in], [pil_in, case_d - pil_in], [case_w - pil_in, case_d - pil_in]];
lid_boss_d = ins_od + 2 * ins_wall;
oled_pcb_c = [oled_at[0] - oled_aa_c[0], oled_at[1] - oled_aa_c[1]];   // lid frame
oled_pcb_w = -oled_skin - oled_glass_gap - oled_glass_h;                // PCB front face (w)

// =====================================================================================================
// helpers
// =====================================================================================================
module rr(w, d, r) translate([r, r]) offset(r = r) square([w - 2 * r, d - 2 * r]);
module fp(i = 0) translate([i, i]) rr(case_w - 2 * i, case_d - 2 * i, max(corner_r - i, 0.5));
module slab(z, i) translate([0, 0, z]) linear_extrude(eps) fp(i);
module sheared() multmatrix([[1, 0, 0, 0], [0, 1, 0, 0], [0, tan(tilt), 1, 0], [0, 0, 0, 1]]) children();
module top_slab(dz, i) sheared() slab(h_front + dz, i);           // a slab parallel to the top face
module on_top() translate([0, 0, h_front]) rotate([tilt, 0, 0]) children();   // lid frame (u, v, w)
module box(p0, p1) translate(p0) cube([for (k = [0:2]) p1[k] - p0[k]]);
module ring2d(i0, i1) difference() { fp(i0); fp(i1); }

// =====================================================================================================
// tray
// =====================================================================================================
module tray_outer() hull() {
    slab(0, bot_ch); slab(bot_ch, 0);
    slab(seam_z - reveal, 0); slab(seam_z - eps, reveal);
}

module tray() difference() {
    union() {
        difference() {
            tray_outer();
            translate([0, 0, floor_t]) linear_extrude(seam_z) fp(wall);
        }
        // screw pillars (sleeves) and their floor bosses
        for (p = pillars) translate([p[0], p[1], 0]) cylinder(d = pil_d, h = seam_z - pil_gap);
        s3_cradle(); dac_cradle(); adc_standoffs();
    }
    for (p = pillars) translate([p[0], p[1], -eps]) {
        cylinder(d = screw_clear, h = seam_z);
        cylinder(d = cb_d, h = cb_h);
    }
    for (sx = [0, 1], sy = [0, 1])
        translate([sx ? case_w - foot_inset[0] : foot_inset[0], sy ? case_d - foot_inset[1] : foot_inset[1], -eps])
            cylinder(d = foot_recess_d, h = foot_recess_h + eps);
    // back panel: USB-C, jacks
    translate([s3_x, case_d + eps, s3_usb_z]) rotate([90, 0, 0])
        linear_extrude(wall + 1) offset(r = 1.2) square([usb_open[0] - 2.4, usb_open[1] - 2.4], center = true);
    for (x = pj_xs) translate([x, case_d + eps, pj_z]) rotate([90, 0, 0]) cylinder(d = pj_hole_d, h = wall + 1);
    if (output_jack == "module") translate([dac_jack_x, case_d + eps, dac_jack_z]) rotate([90, 0, 0])
        cylinder(d = dac_open_d, h = wall + 1);
}

// ESP32-S3-Zero has no mounting holes: a platform with side rails, a back stop (the wall) and a front snap.
module s3_cradle() {
    x0 = s3_x - s3_pcb[0] / 2 - s3_cl;  x1 = s3_x + s3_pcb[0] / 2 + s3_cl;
    difference() {
        box([x0 - 1.2, s3_y0 - s3_cl - 1.6, 0], [x1 + 1.2, y_bi, pcb_z + 1.4]);
        box([x0, s3_y0 - s3_cl, pcb_z], [x1, y_bi + 1, pcb_z + 5]);             // PCB seat
        box([x0 + 2.6, s3_y0 + 2.6, pcb_z - 0.6], [x1 - 2.6, y_bi - 2.6, pcb_z + 1]);   // relief under pads
        box([s3_x - 3, s3_y0 - s3_cl - 2, pcb_z - 1.5], [s3_x + 3, s3_y0 - s3_cl - 1.0, pcb_z + 5]);  // snap slot
    }
    // front snap (flexes), lip 0.5 over the PCB top
    translate([s3_x - 3, s3_y0 - s3_cl - 1.0, pcb_z]) cube([6, 1.0, s3_pcb[2] + 0.6]);
    translate([s3_x - 3, s3_y0 - s3_cl - 1.0, pcb_z + s3_pcb[2] + 0.1]) cube([6, 1.5, 0.5]);
}

// GY-PCM5102 has no mounting holes: four posts + a U-frame with snap lips.
module dac_cradle() {
    x0 = dac_x0 - dac_cl;  x1 = dac_x0 + dac_pcb[0] + dac_cl;  y0 = dac_y0 - dac_cl;  y1 = dac_back + dac_cl;
    for (p = [[4.5, 15.0], [17.5, 4.5], [29.5, 15.5], [30.6, 3.4]])
        translate([dac_x0 + p[0] - 1.1, dac_y0 + p[1] - 1.1, 0]) cube([2.2, 2.2, pcb_z]);
    difference() {
        box([x0 - 1.2, y0 - 1.2, 0], [x1 + 1.2, y1, pcb_z + dac_pcb[2] + 1.2]);
        box([x0, y0, floor_t], [x1, y1 + 1, pcb_z + 6]);
        box([dac_x0 + 9, y0 - 2, floor_t + 1], [dac_x0 + 23, y0 + 1, pcb_z + 6]);  // wire exit, front
    }
    for (x = [x0 + 3, x1 - 7]) translate([x, y0, pcb_z + dac_pcb[2] + 0.15]) cube([4, 0.5, 0.5]);  // lips
}

module adc_standoffs() for (h = adc_holes) translate([adc_at[0] + h[0], adc_at[1] + h[1], 0]) difference() {
    cylinder(d = mod_so_d, h = pcb_z);
    translate([0, 0, 1]) cylinder(d = m2_pilot, h = pcb_z);
}

// =====================================================================================================
// lid
// =====================================================================================================
module lid_outer() hull() {
    slab(seam_z, reveal); slab(seam_z + reveal, 0);
    top_slab(-edge_ch - eps, 0); top_slab(-eps, edge_ch);
}

module lid_cavity() hull() { slab(seam_z - 1, wall); top_slab(-lid_t / cos(tilt) - eps, wall); }

module lid() difference() {
    union() {
        difference() { lid_outer(); lid_cavity(); }
        difference() {                         // locating lip on three sides; none along the back (jacks, USB-C)
            translate([0, 0, seam_z - lip_h]) linear_extrude(lip_h + eps) ring2d(wall + lip_cl, wall + lip_cl + lip_t);
            translate([corner_r + 5, case_d - wall - lip_cl - lip_t - 1, seam_z - lip_h - 1]) cube([case_w - 2 * corner_r - 10, lip_t + 2, lip_h + 1]);
        }
        intersection() {                       // insert bosses: plate down to the seam
            for (p = pillars) translate([p[0], p[1], seam_z]) cylinder(d = lid_boss_d, h = h_back);
            hull() { slab(seam_z, wall - eps); top_slab(-lid_t / cos(tilt) + eps, wall - eps); }
        }
        on_top() lid_under();
    }
    for (p = pillars) translate([p[0], p[1], seam_z - eps]) cylinder(d = ins_hole_d, h = ins_hole_depth);
    on_top() lid_cuts();
}

module lid_under() {
    // OLED: four seats under the PCB front corners (outside the glass), and two-sided edge clamps: bosses beside
    // the PCB, notched round it, so an M2 x 6 screw + DIN 9021 washer from below laps oled_clamp_lap over the PCB edge
    difference() {
        union() {
            for (sx = [-1, 1], sy = [-1, 1])
                translate([oled_pcb_c[0] + sx * (oled_pcb[0] / 2 + oled_boss_d / 2 - oled_clamp_lap), oled_pcb_c[1] + sy * oled_clamp_dy,
                           oled_pcb_w - oled_pcb[2]])
                    cylinder(d = oled_boss_d, h = -oled_pcb_w + oled_pcb[2] - lid_t + eps);
            for (sx = [-1, 1], sy = [-1, 1])
                translate([oled_pcb_c[0] + sx * (oled_pcb[0] / 2 - 2.0) - 2.0, oled_pcb_c[1] + sy * (oled_pcb[1] / 2 - 1.3) - 1.3, oled_pcb_w])
                    cube([4.0, 2.6, -oled_pcb_w - lid_t + eps]);
        }
        translate([oled_pcb_c[0], oled_pcb_c[1], oled_pcb_w - oled_pcb[2] / 2 - 0.05])          // the PCB itself (+clearance)
            cube([oled_pcb[0] + 0.4, oled_pcb[1] + 0.4, oled_pcb[2] + 0.1], center = true);
        for (sx = [-1, 1], sy = [-1, 1])                                                          // M2 pilots
            translate([oled_pcb_c[0] + sx * (oled_pcb[0] / 2 + oled_boss_d / 2 - oled_clamp_lap), oled_pcb_c[1] + sy * oled_clamp_dy,
                       oled_pcb_w - oled_pcb[2] - eps]) cylinder(d = m2_pilot, h = 4);
    }
    // EC11: a collar round the bushing that also carries the anti-rotation slot
    translate([enc_at[0], enc_at[1], -lid_t - 1.0]) cylinder(d = enc_body[0] + 6, h = 1.0 + eps);
}

module lid_cuts() {
    // OLED window: 45-degree chamfer from the face down to the active area (the recessed bezel), glass pocket below
    win = [oled_aa[0] + 2 * oled_win_margin, oled_aa[1] + 2 * oled_win_margin];
    translate([oled_at[0], oled_at[1], 0]) hull() {
        translate([0, 0, -oled_skin - eps]) linear_extrude(eps) square(win, center = true);
        translate([0, 0, eps]) linear_extrude(eps) square([win[0] + 2 * oled_skin, win[1] + 2 * oled_skin], center = true);
    }
    translate([oled_pcb_c[0], oled_pcb_c[1], -lid_t - 5])
        linear_extrude(5 + lid_t - oled_skin) square([oled_glass[0] + 2 * oled_glass_cl, oled_glass[1] + 2 * oled_glass_cl], center = true);
    // EC11: bushing hole, pocket thinning the plate to enc_panel_t, anti-rotation slot in the pocket ceiling
    translate([enc_at[0], enc_at[1], 0]) {
        translate([0, 0, -lid_t - 2]) cylinder(d = enc_bush_d + enc_hole_cl, h = lid_t + 3);
        translate([0, 0, -lid_t - 1.0 - eps]) linear_extrude(lid_t + 1.0 - enc_panel_t)
            square([enc_body[0] + 0.6, enc_body[1] + 0.6], center = true);
        translate([0, enc_tab_r, -enc_panel_t - eps]) linear_extrude(enc_tab[2] + 0.4)
            square([enc_tab[0] + 0.4, enc_tab[1] + 0.4], center = true);
    }
}

// =====================================================================================================
// knob (own frame: z = 0 at its lower edge, axis at the origin)
// =====================================================================================================
module knob() difference() {
    hull() {
        cylinder(d = knob_d, h = knob_h - knob_ch);
        cylinder(d = knob_d - 2 * knob_ch, h = knob_h);
    }
    for (i = [0:knob_flutes - 1]) rotate([0, 0, i * 360 / knob_flutes])        // fine flutes (45-degree V)
        translate([knob_d / 2, 0, 1.5]) rotate([0, 0, 45]) cube([knob_flute_w, knob_flute_w, 2 * knob_h], center = true);
    translate([0, 0, -eps]) cylinder(d = knob_skirt_id, h = knob_skirt_h);
    translate([0, 0, knob_skirt_h - eps]) linear_extrude(enc_shaft_l - (knob_gap + enc_panel_t) - knob_skirt_h + 1)
        intersection() {                                                       // D-bore
            circle(d = enc_shaft_d + knob_bore_cl);
            translate([-(enc_shaft_d + knob_bore_cl) / 2, -enc_shaft_d]) square([enc_shaft_d + knob_bore_cl, enc_shaft_flat + enc_shaft_d / 2 + knob_bore_cl / 2]);
        }
    translate([0, 3, knob_h - knob_ind_depth]) translate([-knob_ind_w / 2, 0, 0]) cube([knob_ind_w, knob_d / 2, 1]);  // indicator
}
module knob_placed() on_top() translate([enc_at[0], enc_at[1], knob_gap]) rotate([0, 0, 180]) knob();

// print orientations (each on the bed at z = 0)
module tray_print() tray();
module lid_print() rotate([180 - tilt, 0, 0]) translate([0, 0, -h_front]) lid();   // top face down
module knob_print() translate([0, 0, knob_h]) rotate([180, 0, 0]) knob();

// =====================================================================================================
// module stand-ins (also the bodies used by the collision checks)
// =====================================================================================================
module env_s3() {
    translate([s3_x - s3_pcb[0] / 2, s3_y0, pcb_z]) cube(s3_pcb);
    translate([s3_x - s3_pcb[0] / 2 + 1, s3_y0 + 1, pcb_z + s3_pcb[2]]) cube([s3_pcb[0] - 2, s3_pcb[1] - 8, s3_top_h]);
    translate([s3_x - s3_usb[0] / 2, y_bi - s3_wall_gap - s3_usb[1] + s3_usb_overhang, pcb_z + s3_pcb[2]]) cube(s3_usb);
}
module keep_s3_antenna() translate([s3_x - s3_pcb[0] / 2 - 4, s3_y0 - s3_ant_zone, pcb_z]) cube([s3_pcb[0] + 8, s3_ant_zone, 14]);
module keep_usb_plug() translate([s3_x - usbc_overmold[0] / 2, case_d - 0.1, s3_usb_z - usbc_overmold[1] / 2]) cube([usbc_overmold[0], 20, usbc_overmold[1]]);
module env_dac() {
    translate([dac_x0, dac_y0, pcb_z]) cube(dac_pcb);
    translate([dac_x0 + dac_jack_u - dac_jack_body[0] / 2, dac_back - dac_jack_body[1] + dac_jack_nose, pcb_z + dac_pcb[2]]) cube(dac_jack_body);
    translate([dac_x0 + 9, dac_y0 + 2, pcb_z + dac_pcb[2]]) cube([dac_pcb[0] - 11, dac_pcb[1] - 4, dac_top_h]);
}
module env_adc() {
    translate([adc_at[0], adc_at[1], pcb_z]) cube(adc_pcb);
    translate([adc_at[0] + 4, adc_at[1] + 5, pcb_z + adc_pcb[2]]) cube([adc_pcb[0] - 8, adc_pcb[1] - 10, adc_top_h]);
}
module env_jacks() for (x = pj_xs) translate([x, y_bi + eps, pj_z]) rotate([90, 0, 0]) cylinder(d = pj_body_d, h = pj_body_l);
module env_oled() on_top() {
    translate([oled_pcb_c[0], oled_pcb_c[1], oled_pcb_w - oled_pcb[2] / 2]) cube(oled_pcb, center = true);
    translate([oled_pcb_c[0], oled_pcb_c[1], oled_pcb_w + oled_glass_h / 2]) cube([oled_glass[0], oled_glass[1], oled_glass_h], center = true);
    translate([oled_pcb_c[0] - 5, oled_pcb_c[1] + oled_pcb[1] / 2 - oled_hdr_c - 1.3, oled_pcb_w - oled_pcb[2] - oled_pin_l])
        cube([10.2, 2.6, oled_pin_l]);
}
module env_enc() on_top() translate([enc_at[0], enc_at[1], -enc_panel_t - 0.05]) {   // 0.05: the mounting face bears on the pocket ceiling
    translate([-enc_body[0] / 2, -enc_body[1] / 2, -enc_body[2]]) cube(enc_body);
    translate([-enc_body[0] / 2 + 1, -enc_body[1] / 2 - 2, -enc_body[2] - enc_pins_h]) cube([enc_body[0] - 2, enc_body[1] + 4, enc_pins_h]);
    cylinder(d = enc_bush_d, h = 5);
    cylinder(d = enc_shaft_d, h = enc_shaft_l);
    translate([0, 0, enc_panel_t + 0.1 + enc_washer_t]) cylinder(d = enc_nut[0], h = enc_nut[1], $fn = 6);
}
// Dupont housings standing on the floor modules' headers (wiring room under the lid)
module keep_dupont() {
    translate([dac_x0 + 0.2, dac_y0, pcb_z + dac_pcb[2]]) cube([3, dac_pcb[1] - 2, dupont_h]);   // 6-pin I2S header
    for (x = [1.4, 26.3]) translate([adc_at[0] + x - 1.3, adc_at[1] + 3, pcb_z + adc_pcb[2]]) cube([2.6, 15.3, dupont_h]);
}
module modules() { env_s3(); env_dac(); env_adc(); env_jacks(); env_oled(); env_enc(); }

// =====================================================================================================
// what this file shows
// =====================================================================================================
if (part == "assembly" || part == "exploded") {
    e = part == "exploded" ? explode : 0;
    color("#2b2d31") tray();
    translate([0, 0, e]) color("#e4e0d8") lid();
    translate([0, 0, 1.7 * e]) color("#2b2d31") knob_placed();
    if (show_modules) {
        color("#1d1f22") env_s3(); color("#5b2a86") env_dac(); color("#2a6b3f") env_adc(); color("#9a9a9a") env_jacks();
        translate([0, 0, 0.6 * e]) { color("#1f4f8a") env_oled(); color("#777") env_enc(); }
    }
}
if (part == "tray") tray_print();
if (part == "lid") lid_print();
if (part == "knob") knob_print();
// collision checks (tools: enclosure/check.sh): each must render EMPTY
if (part == "check:tray-modules") intersection() { tray(); modules(); }
if (part == "check:lid-modules")  intersection() { lid(); modules(); }
if (part == "check:tray-lid")     intersection() { tray(); lid(); }
if (part == "check:knob-lid")     intersection() { knob_placed(); lid(); }
if (part == "check:modules-modules") {
    intersection() { env_s3(); union() { env_dac(); env_adc(); env_jacks(); env_oled(); env_enc(); } }
    intersection() { env_dac(); union() { env_adc(); env_jacks(); env_oled(); env_enc(); } }
    intersection() { env_adc(); union() { env_jacks(); env_oled(); env_enc(); } }
    intersection() { env_jacks(); union() { env_oled(); env_enc(); } }
    intersection() { env_oled(); env_enc(); }
}
if (part == "check:antenna-keepout") intersection() { keep_s3_antenna(); union() { env_dac(); env_adc(); env_jacks(); env_oled(); env_enc(); } }
if (part == "check:usb-plug")        intersection() { keep_usb_plug(); union() { tray(); lid(); } }
if (part == "check:dupont-lid")      intersection() { keep_dupont(); lid(); }
