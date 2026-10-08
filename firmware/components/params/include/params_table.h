/*
 * params_table.h: the X-macro table of every user-facing parameter.
 * This is the single source of truth for UI, DSP, web, MIDI, BLE and storage.
 *
 * X(ID, key, name, unit, type, curve, min, max, def, step, flags, enum_names, midi_cc)
 *   key   : stable identifier used in NVS blobs (hashed) and web JSON. Never rename a shipped key.
 *   name  : short UI label (<= 10 chars, caps).
 *   unit  : "dB" "Hz" "ms" "s" "%" ":1" "BPM" "" .
 *   type  : PT_FLOAT / PT_BOOL / PT_ENUM / PT_INT.
 *   curve : PC_LIN or PC_LOG (normalised mapping for MIDI, web sliders and encoder stepping).
 *   step  : PC_LIN -> encoder step in units. PC_LOG -> encoder steps per octave.
 *   flags : PF_PRESET (stored in presets), PF_SETTING (device setting), PF_VOLATILE (never stored).
 *   midi_cc : default CC number, or NO_CC.
 */
#pragma once

#define NO_CC 0xFF

#define PARAMS_TABLE(X) \
  /* ---------- output / input / monitor (device settings) ---------- */ \
  X(P_VOLUME,      "out.volume",  "VOLUME",   "dB",  PT_FLOAT, PC_LIN, -80.f,  0.f,   -20.f,  0.5f, PF_SETTING, NULL, 7)   \
  X(P_MUTE,        "out.mute",    "MUTE",     "",    PT_BOOL,  PC_LIN,  0.f,   1.f,    0.f,   1.f,  PF_SETTING, NULL, 9)   \
  X(P_IN_GAIN,     "in.gain",     "IN GAIN",  "dB",  PT_FLOAT, PC_LIN, -12.f, 24.f,    0.f,   0.5f, PF_SETTING, NULL, 14)  \
  X(P_IN_MODE,     "in.mode",     "INPUT",    "",    PT_ENUM,  PC_LIN,  0.f,   3.f,    0.f,   1.f,  PF_SETTING, E_IN_MODE, NO_CC) \
  X(P_MON_MIX,     "mon.mix",     "MON MIX",  "%",   PT_FLOAT, PC_LIN,  0.f, 100.f,    0.f,   1.f,  PF_SETTING, NULL, 15)  \
  X(P_MON_PATH,    "mon.path",    "MON PATH", "",    PT_ENUM,  PC_LIN,  0.f,   1.f,    0.f,   1.f,  PF_SETTING, E_MON_PATH, NO_CC) \
  X(P_TEMPO,       "sys.tempo",   "TEMPO",    "BPM", PT_FLOAT, PC_LIN, 40.f, 240.f,  120.f,   1.f,  PF_PRESET,  NULL, NO_CC) \
  /* ---------- EQ (preset) ---------- */ \
  X(P_EQ_ON,       "eq.on",       "EQ",       "",    PT_BOOL,  PC_LIN,  0.f,   1.f,    1.f,   1.f,  PF_PRESET, NULL, 90)   \
  X(P_EQ_IN,       "eq.in",       "IN TRIM",  "dB",  PT_FLOAT, PC_LIN, -24.f, 12.f,    0.f,   0.5f, PF_PRESET, NULL, NO_CC) \
  X(P_EQ_OUT,      "eq.out",      "OUT TRIM", "dB",  PT_FLOAT, PC_LIN, -24.f, 12.f,    0.f,   0.5f, PF_PRESET, NULL, NO_CC) \
  X(P_EQ_B1_ON,    "eq.b1.on",    "LOW SHELF","",    PT_BOOL,  PC_LIN,  0.f,   1.f,    1.f,   1.f,  PF_PRESET, NULL, NO_CC) \
  X(P_EQ_B1_FREQ,  "eq.b1.freq",  "FREQ",     "Hz",  PT_FLOAT, PC_LOG, 20.f, 20000.f, 80.f,  24.f,  PF_PRESET, NULL, 25)  \
  X(P_EQ_B1_GAIN,  "eq.b1.gain",  "GAIN",     "dB",  PT_FLOAT, PC_LIN, -18.f, 18.f,    0.f,   0.25f,PF_PRESET, NULL, 20)  \
  X(P_EQ_B1_Q,     "eq.b1.q",     "Q",        "",    PT_FLOAT, PC_LOG, 0.3f, 10.f,   0.71f, 12.f,  PF_PRESET, NULL, 85)  \
  X(P_EQ_B2_ON,    "eq.b2.on",    "PEAK 1",   "",    PT_BOOL,  PC_LIN,  0.f,   1.f,    1.f,   1.f,  PF_PRESET, NULL, NO_CC) \
  X(P_EQ_B2_FREQ,  "eq.b2.freq",  "FREQ",     "Hz",  PT_FLOAT, PC_LOG, 20.f, 20000.f, 250.f, 24.f,  PF_PRESET, NULL, 26)  \
  X(P_EQ_B2_GAIN,  "eq.b2.gain",  "GAIN",     "dB",  PT_FLOAT, PC_LIN, -18.f, 18.f,    0.f,   0.25f,PF_PRESET, NULL, 21)  \
  X(P_EQ_B2_Q,     "eq.b2.q",     "Q",        "",    PT_FLOAT, PC_LOG, 0.3f, 10.f,    1.0f, 12.f,  PF_PRESET, NULL, 86)  \
  X(P_EQ_B3_ON,    "eq.b3.on",    "PEAK 2",   "",    PT_BOOL,  PC_LIN,  0.f,   1.f,    1.f,   1.f,  PF_PRESET, NULL, NO_CC) \
  X(P_EQ_B3_FREQ,  "eq.b3.freq",  "FREQ",     "Hz",  PT_FLOAT, PC_LOG, 20.f, 20000.f, 1000.f,24.f,  PF_PRESET, NULL, 27)  \
  X(P_EQ_B3_GAIN,  "eq.b3.gain",  "GAIN",     "dB",  PT_FLOAT, PC_LIN, -18.f, 18.f,    0.f,   0.25f,PF_PRESET, NULL, 22)  \
  X(P_EQ_B3_Q,     "eq.b3.q",     "Q",        "",    PT_FLOAT, PC_LOG, 0.3f, 10.f,    1.0f, 12.f,  PF_PRESET, NULL, 87)  \
  X(P_EQ_B4_ON,    "eq.b4.on",    "PEAK 3",   "",    PT_BOOL,  PC_LIN,  0.f,   1.f,    1.f,   1.f,  PF_PRESET, NULL, NO_CC) \
  X(P_EQ_B4_FREQ,  "eq.b4.freq",  "FREQ",     "Hz",  PT_FLOAT, PC_LOG, 20.f, 20000.f, 4000.f,24.f,  PF_PRESET, NULL, 28)  \
  X(P_EQ_B4_GAIN,  "eq.b4.gain",  "GAIN",     "dB",  PT_FLOAT, PC_LIN, -18.f, 18.f,    0.f,   0.25f,PF_PRESET, NULL, 23)  \
  X(P_EQ_B4_Q,     "eq.b4.q",     "Q",        "",    PT_FLOAT, PC_LOG, 0.3f, 10.f,    1.0f, 12.f,  PF_PRESET, NULL, 88)  \
  X(P_EQ_B5_ON,    "eq.b5.on",    "HI SHELF", "",    PT_BOOL,  PC_LIN,  0.f,   1.f,    1.f,   1.f,  PF_PRESET, NULL, NO_CC) \
  X(P_EQ_B5_FREQ,  "eq.b5.freq",  "FREQ",     "Hz",  PT_FLOAT, PC_LOG, 20.f, 20000.f, 10000.f,24.f, PF_PRESET, NULL, 29)  \
  X(P_EQ_B5_GAIN,  "eq.b5.gain",  "GAIN",     "dB",  PT_FLOAT, PC_LIN, -18.f, 18.f,    0.f,   0.25f,PF_PRESET, NULL, 24)  \
  X(P_EQ_B5_Q,     "eq.b5.q",     "Q",        "",    PT_FLOAT, PC_LOG, 0.3f, 10.f,   0.71f, 12.f,  PF_PRESET, NULL, 89)  \
  /* ---------- Saturation ---------- */ \
  X(P_SAT_ON,      "sat.on",      "SATURATE", "",    PT_BOOL,  PC_LIN,  0.f,   1.f,    0.f,   1.f,  PF_PRESET, NULL, 102) \
  X(P_SAT_DRIVE,   "sat.drive",   "DRIVE",    "dB",  PT_FLOAT, PC_LIN,  0.f,  24.f,    6.f,   0.5f, PF_PRESET, NULL, 103) \
  X(P_SAT_WARMTH,  "sat.warmth",  "WARMTH",   "%",   PT_FLOAT, PC_LIN,  0.f, 100.f,   30.f,   1.f,  PF_PRESET, NULL, NO_CC) \
  X(P_SAT_TONE,    "sat.tone",    "TONE",     "%",   PT_FLOAT, PC_LIN,-100.f,100.f,    0.f,   2.f,  PF_PRESET, NULL, NO_CC) \
  X(P_SAT_MIX,     "sat.mix",     "MIX",      "%",   PT_FLOAT, PC_LIN,  0.f, 100.f,  100.f,   1.f,  PF_PRESET, NULL, 104) \
  /* ---------- Chorus / ensemble ---------- */ \
  X(P_CHO_ON,      "cho.on",      "CHORUS",   "",    PT_BOOL,  PC_LIN,  0.f,   1.f,    0.f,   1.f,  PF_PRESET, NULL, 105) \
  X(P_CHO_MODE,    "cho.mode",    "MODE",     "",    PT_ENUM,  PC_LIN,  0.f,   2.f,    1.f,   1.f,  PF_PRESET, E_CHO_MODE, NO_CC) \
  X(P_CHO_RATE,    "cho.rate",    "RATE",     "Hz",  PT_FLOAT, PC_LOG, 0.05f, 5.f,    0.6f, 12.f,  PF_PRESET, NULL, 106) \
  X(P_CHO_DEPTH,   "cho.depth",   "DEPTH",    "%",   PT_FLOAT, PC_LIN,  0.f, 100.f,   40.f,   1.f,  PF_PRESET, NULL, 107) \
  X(P_CHO_MIX,     "cho.mix",     "MIX",      "%",   PT_FLOAT, PC_LIN,  0.f, 100.f,   35.f,   1.f,  PF_PRESET, NULL, 108) \
  /* ---------- Delay ---------- */ \
  X(P_DLY_ON,      "dly.on",      "DELAY",    "",    PT_BOOL,  PC_LIN,  0.f,   1.f,    0.f,   1.f,  PF_PRESET, NULL, 109) \
  X(P_DLY_SYNC,    "dly.sync",    "SYNC",     "",    PT_ENUM,  PC_LIN,  0.f,   7.f,    0.f,   1.f,  PF_PRESET, E_DLY_SYNC, NO_CC) \
  X(P_DLY_TIME,    "dly.time",    "TIME",     "ms",  PT_FLOAT, PC_LOG, 10.f, 1500.f, 375.f, 24.f,  PF_PRESET, NULL, 110) \
  X(P_DLY_FB,      "dly.fb",      "FEEDBACK", "%",   PT_FLOAT, PC_LIN,  0.f,  95.f,   35.f,   1.f,  PF_PRESET, NULL, 111) \
  X(P_DLY_MODE,    "dly.mode",    "MODE",     "",    PT_ENUM,  PC_LIN,  0.f,   1.f,    1.f,   1.f,  PF_PRESET, E_DLY_MODE, NO_CC) \
  X(P_DLY_HICUT,   "dly.hicut",   "HI CUT",   "Hz",  PT_FLOAT, PC_LOG, 1000.f,20000.f,6000.f,12.f, PF_PRESET, NULL, NO_CC) \
  X(P_DLY_LOCUT,   "dly.locut",   "LO CUT",   "Hz",  PT_FLOAT, PC_LOG, 20.f, 1000.f, 120.f, 12.f,  PF_PRESET, NULL, NO_CC) \
  X(P_DLY_MIX,     "dly.mix",     "MIX",      "%",   PT_FLOAT, PC_LIN,  0.f, 100.f,   25.f,   1.f,  PF_PRESET, NULL, 112) \
  /* ---------- Plate reverb ---------- */ \
  X(P_REV_ON,      "rev.on",      "REVERB",   "",    PT_BOOL,  PC_LIN,  0.f,   1.f,    0.f,   1.f,  PF_PRESET, NULL, 113) \
  X(P_REV_DECAY,   "rev.decay",   "DECAY",    "s",   PT_FLOAT, PC_LOG, 0.3f, 12.f,    2.2f, 12.f,  PF_PRESET, NULL, 114) \
  X(P_REV_PREDLY,  "rev.predelay","PRE-DLY",  "ms",  PT_FLOAT, PC_LIN,  0.f, 150.f,   20.f,   1.f,  PF_PRESET, NULL, 116) \
  X(P_REV_DAMP,    "rev.damp",    "DAMPING",  "%",   PT_FLOAT, PC_LIN,  0.f, 100.f,   45.f,   1.f,  PF_PRESET, NULL, NO_CC) \
  X(P_REV_MOD,     "rev.mod",     "MOD",      "%",   PT_FLOAT, PC_LIN,  0.f, 100.f,   30.f,   1.f,  PF_PRESET, NULL, NO_CC) \
  X(P_REV_LOCUT,   "rev.locut",   "LO CUT",   "Hz",  PT_FLOAT, PC_LOG, 20.f,  600.f,  120.f, 12.f,  PF_PRESET, NULL, NO_CC) \
  X(P_REV_WIDTH,   "rev.width",   "WIDTH",    "%",   PT_FLOAT, PC_LIN,  0.f, 100.f,  100.f,   1.f,  PF_PRESET, NULL, NO_CC) \
  X(P_REV_MIX,     "rev.mix",     "MIX",      "%",   PT_FLOAT, PC_LIN,  0.f, 100.f,   20.f,   1.f,  PF_PRESET, NULL, 115) \
  /* ---------- Bus compressor ---------- */ \
  X(P_CMP_ON,      "cmp.on",      "COMPRESS", "",    PT_BOOL,  PC_LIN,  0.f,   1.f,    0.f,   1.f,  PF_PRESET, NULL, 117) \
  X(P_CMP_THRESH,  "cmp.thresh",  "THRESH",   "dB",  PT_FLOAT, PC_LIN, -40.f,  0.f,  -12.f,   0.5f, PF_PRESET, NULL, 118) \
  X(P_CMP_RATIO,   "cmp.ratio",   "RATIO",    ":1",  PT_FLOAT, PC_LOG,  1.f,  20.f,    2.f,  12.f,  PF_PRESET, NULL, 119) \
  X(P_CMP_ATTACK,  "cmp.attack",  "ATTACK",   "ms",  PT_FLOAT, PC_LOG, 0.1f, 100.f,   10.f,  12.f,  PF_PRESET, NULL, NO_CC) \
  X(P_CMP_RELEASE, "cmp.release", "RELEASE",  "ms",  PT_FLOAT, PC_LOG, 30.f, 2000.f, 200.f,  12.f,  PF_PRESET, NULL, NO_CC) \
  X(P_CMP_KNEE,    "cmp.knee",    "KNEE",     "dB",  PT_FLOAT, PC_LIN,  0.f,  12.f,    6.f,   0.5f, PF_PRESET, NULL, NO_CC) \
  X(P_CMP_MAKEUP,  "cmp.makeup",  "MAKEUP",   "dB",  PT_FLOAT, PC_LIN,  0.f,  24.f,    0.f,   0.5f, PF_PRESET, NULL, NO_CC) \
  X(P_CMP_MIX,     "cmp.mix",     "MIX",      "%",   PT_FLOAT, PC_LIN,  0.f, 100.f,  100.f,   1.f,  PF_PRESET, NULL, NO_CC) \
  /* ---------- Safety limiter (setting) ---------- */ \
  X(P_LIM_ON,      "lim.on",      "LIMITER",  "",    PT_BOOL,  PC_LIN,  0.f,   1.f,    1.f,   1.f,  PF_SETTING, NULL, NO_CC) \
  X(P_LIM_CEIL,    "lim.ceiling", "CEILING",  "dB",  PT_FLOAT, PC_LIN, -3.f,   0.f,   -0.3f,  0.1f, PF_SETTING, NULL, NO_CC) \
  /* ---------- Listening: loudness compensation (ISO 226) and headphone crossfeed (settings) ---------- */ \
  X(P_LOUD_ON,     "loud.on",     "LOUDNESS", "",    PT_BOOL,  PC_LIN,  0.f,   1.f,    0.f,   1.f,  PF_SETTING, NULL, NO_CC) \
  X(P_LOUD_REF,    "loud.ref",    "REF",      "dB",  PT_FLOAT, PC_LIN, 60.f, 100.f,   83.f,   1.f,  PF_SETTING, NULL, NO_CC) \
  X(P_LOUD_AMT,    "loud.amount", "AMOUNT",   "%",   PT_FLOAT, PC_LIN,  0.f, 200.f,  100.f,   5.f,  PF_SETTING, NULL, NO_CC) \
  X(P_XF_ON,       "xf.on",       "CROSSFEED","",    PT_BOOL,  PC_LIN,  0.f,   1.f,    0.f,   1.f,  PF_SETTING, NULL, NO_CC) \
  X(P_XF_FEED,     "xf.feed",     "FEED",     "dB",  PT_FLOAT, PC_LIN,  1.f,  15.f,    4.5f,  0.5f, PF_SETTING, NULL, NO_CC) \
  X(P_XF_FC,       "xf.fc",       "CUTOFF",   "Hz",  PT_FLOAT, PC_LOG, 300.f, 2000.f, 700.f, 12.f,  PF_SETTING, NULL, NO_CC) \
  /* ---------- UI / system settings ---------- */ \
  X(P_UI_BRIGHT,   "ui.bright",   "BRIGHT",   "%",   PT_FLOAT, PC_LIN,  0.f, 100.f,   70.f,   5.f,  PF_SETTING, NULL, NO_CC) \
  X(P_UI_DIM,      "ui.dim",      "DIM AFTER","",    PT_ENUM,  PC_LIN,  0.f,   4.f,    2.f,   1.f,  PF_SETTING, E_UI_DIM, NO_CC) \
  X(P_UI_SAVER,    "ui.saver",    "SAVER",    "",    PT_ENUM,  PC_LIN,  0.f,   4.f,    3.f,   1.f,  PF_SETTING, E_UI_SAVER, NO_CC) \
  X(P_UI_OLED,     "ui.oled",     "OLED",     "",    PT_ENUM,  PC_LIN,  0.f,   1.f,    0.f,   1.f,  PF_SETTING, E_UI_OLED, NO_CC) \
  X(P_UI_LED,      "ui.led",      "LED",      "",    PT_ENUM,  PC_LIN,  0.f,   2.f,    1.f,   1.f,  PF_SETTING, E_UI_LED, NO_CC) \
  X(P_UI_SPECTRUM, "ui.spectrum", "SPECTRUM", "",    PT_BOOL,  PC_LIN,  0.f,   1.f,    1.f,   1.f,  PF_SETTING, NULL, NO_CC) \
  X(P_MIDI_CH,     "midi.ch",     "MIDI CH",  "",    PT_INT,   PC_LIN,  0.f,  16.f,    0.f,   1.f,  PF_SETTING, NULL, NO_CC) \
  X(P_MIDI_ENC,    "midi.enc",    "KNOB MODE","",    PT_ENUM,  PC_LIN,  0.f,   1.f,    0.f,   1.f,  PF_SETTING, E_MIDI_ENC, NO_CC) \
  X(P_MIDI_ENC_CC, "midi.enc_cc", "KNOB CC",  "",    PT_INT,   PC_LIN,  0.f, 127.f,   16.f,   1.f,  PF_SETTING, NULL, NO_CC) \
  X(P_NET_BOOT,    "net.boot",    "RADIOS",   "",    PT_ENUM,  PC_LIN,  0.f,   1.f,    0.f,   1.f,  PF_SETTING, E_NET_BOOT, NO_CC) \
  X(P_NET_WIFI,    "net.wifi",    "WIFI",     "",    PT_BOOL,  PC_LIN,  0.f,   1.f,    0.f,   1.f,  PF_SETTING, NULL, NO_CC) \
  X(P_NET_BLE,     "net.ble",     "BLE MIDI", "",    PT_BOOL,  PC_LIN,  0.f,   1.f,    0.f,   1.f,  PF_SETTING, NULL, NO_CC)

/* Enum value names (defined in params.c). Index = enum value. */
#define PARAMS_ENUMS(E) \
  E(E_IN_MODE,   "STEREO", "MONO", "LEFT", "RIGHT") \
  E(E_MON_PATH,  "DRY", "THRU FX") \
  E(E_CHO_MODE,  "CHORUS", "ENSEMBLE", "VIBRATO") \
  E(E_DLY_SYNC,  "FREE", "1/2", "1/4", "1/4 D", "1/4 T", "1/8", "1/8 D", "1/16") \
  E(E_DLY_MODE,  "STEREO", "PING-PONG") \
  E(E_UI_DIM,    "NEVER", "15 S", "30 S", "1 MIN", "5 MIN") \
  E(E_UI_SAVER,  "NEVER", "1 MIN", "5 MIN", "10 MIN", "30 MIN") \
  E(E_UI_OLED,   "SSD1306", "SH1106") \
  E(E_UI_LED,    "OFF", "SUBTLE", "METER") \
  E(E_MIDI_ENC,  "VOLUME", "MIDI CC") \
  E(E_NET_BOOT,  "STUDIO", "RESTORE")
