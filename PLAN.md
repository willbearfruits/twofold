# TWOFOLD — 2-in / 2-out USB audio interface on ESP32-S3 — PLAN

Status: living document. Deviations from the original plan are logged in §14.

## 1. Product summary

TWOFOLD is a class-compliant USB-C audio interface: 48 kHz / 24-bit, stereo in (PCM1808) and stereo
out (PCM5102A), plus a USB-MIDI port. It has an onboard 32-bit float DSP chain (5-band parametric EQ,
saturation, chorus/ensemble, stereo/ping-pong delay, plate reverb, bus compressor, safety limiter), a 128×64 OLED
with an encoder, and opt-in WiFi (web editor, OTA) and BLE-MIDI. Radios are off by default ("Studio Mode").

## 2. Architecture

```
            ┌──────────────────────────── core 1 (audio only) ───────────────────────────┐
 PCM1808 ──▶│ I2S RX DMA ─▶ audio_task (prio 22, IRAM) ─▶ I2S TX DMA ─▶ PCM5102A          │
  (ADC)     │                │  ▲  dsp_engine_process(): int32→float, chain, dither    │
            │      tud_audio_read │ tud_audio_write  (lock-free SPSC TinyUSB FIFOs)       │
            └────────────────┼──┼──────────────────────────────────────────────────────┘
            ┌────────────────┼──┼───────────── core 0 ───────────────────────────────────┐
  USB host ◀┼─▶ TinyUSB task (prio 20): UAC2 + MIDI, FIFO_COUNT feedback, IN flow ctrl   │
            │   ctrl task (prio 8): MIDI CC ⇄ params, BLE-MIDI bridge                     │
            │   ui task (prio 5, 60 Hz): input → ui model → gfx → OLED (async I2C)        │
            │   storage task (prio 3): debounced NVS writes in audio-safe windows         │
            │   net (WiFi/httpd/DNS/OTA) and NimBLE host: only exist when enabled         │
            └──────────────────────────────────────────────────────────────────────────────┘
```

Shared state goes through `params` (an X-macro table that is the single source of truth for ~90
parameters). Values are 32-bit floats written with plain aligned stores, which are atomic on Xtensa.
Each consumer (store, MIDI, web, BLE, UI) has its own atomic dirty bitmask. The audio engine reads
parameter targets once per block and smooths them internally. No locks are shared with the audio task.

Modules (ESP-IDF components under `firmware/components/`):

| component | role | host-portable |
|---|---|---|
| `board` | the single pin/config header + Kconfig (clock mode, OLED type) | yes (header) |
| `params` | parameter table, ranges, units, formatting, MIDI CC map, presets (de)serialisation, factory presets | yes |
| `dsp` | all DSP blocks + `dsp_engine` (chain, smoothing, meters, profiling, spectrum tap) | yes |
| `audio` | I2S full-duplex setup, both clock modes, audio task, XSMT pop control | no |
| `usbdev` | TinyUSB config, UAC2+MIDI composite descriptors, UAC2 control requests, USB-MIDI | no |
| `gfx` | 1-bit framebuffer, primitives, custom pixel fonts, dithers | yes |
| `ui` | screens, menus, tweening, gestures, acceleration, screensaver | yes |
| `display` | SSD1306 / SH1106 driver, dirty-band flush, brightness curve | no |
| `input` | PCNT quadrature encoder + debounced switch | no |
| `storage` | NVS settings + 8 presets, deferred/audio-safe writes | no |
| `net` | WiFi AP/STA, captive portal DNS, httpd + embedded web app, OTA | no |
| `ble` | NimBLE BLE-MIDI peripheral | no |
| `led` | WS2812 status LED (RMT) | no |

The project's USB component is named `usbdev`, not `usb`, because IDF already ships a component called `usb`
(it holds the PHY driver we need), and a same-named project component would shadow it.

## 3. Task / core layout

| task | core | prio | stack | period | notes |
|---|---|---|---|---|---|
| audio | 1 | 22 | 6 KB (internal) | I2S block (32 frames = 0.667 ms) | only task on core 1 besides IDF idle/ipc |
| tinyusb (`tud_task`) | 0 | 20 | 4 KB | event driven | ISO ISR-path copies into FIFOs |
| ctrl (MIDI/BLE bridge) | 0 | 8 | 4 KB | 2 ms | |
| ui + display | 0 | 5 | 6 KB | 16.7 ms (60 fps target, ≥30 fps guaranteed) | I2C async, never touches audio |
| input | 0 | 9 | 3 KB | 1 ms poll | PCNT read, switch debounce, timestamps |
| storage | 0 | 3 | 4 KB | 250 ms | flash writes only in "audio-safe windows" |
| httpd / dns / wifi | 0 | 4–5 | per IDF | — | created on enable, deleted on disable |
| NimBLE host | 0 | 5 | 4 KB | — | created on enable, deleted on disable |

FreeRTOS tick: 1000 Hz. `CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0`, WiFi/BT/LWIP/esp_timer pinned to core 0.

## 4. Clocking strategy (highest priority)

### 4.1 What the ESP32-S3 can do (from IDF v5.5.1 source)
* `soc/esp32s3/include/soc/clk_tree_defs.h`: I2S clock sources are **PLL_F160M**, **XTAL (40 MHz)** and
  **EXTERNAL** (MCLK pin as input). There is no APLL on the S3.
* `hal/esp32s3/include/hal/i2s_ll.h`: `MCLK = I2S_SCLK / (N + b/a)`, with N ≤ 255 and a/b ≤ 511. The fraction is
  realised by a pulse-swallowing pattern (`div_x/div_y/div_z/div_yn1`). Every MCLK period is N or N+1
  source cycles, so period jitter is 1 source cycle peak-to-peak (6.25 ns at 160 MHz). The time-interval
  error (TIE) stays below one cycle peak-to-peak (6.12 ns at MCLK).
* `BCLK = MCLK / bclk_div` (integer). In slave mode the driver forces an internal `bclk_div ≥ 8` (internal
  oversampling of BCK only).

### 4.2 Choosing the cleanest internal configuration
For 48 kHz we need MCLK = k·fs. Neither source divides cleanly:

| source | k | MCLK | divider | pattern fundamental (= gcd(src, MCLK)) | TIE step |
|---|---|---|---|---|---|
| PLL_F160M | 256 | 12.288 MHz | 13 + 1/48 | **256 kHz** | 6.25 ns (7.7 % of T_MCLK) |
| PLL_F160M | 384 | 18.432 MHz | 8 + 49/72 | 256 kHz | 6.25 ns (11.5 %) |
| PLL_F160M | 512 | 24.576 MHz | 6 + 49/96 | 256 kHz | 6.25 ns (15.4 %) |
| XTAL 40 MHz | 256 | 12.288 MHz | 3 + 49/192 | **64 kHz** | 25 ns (30.7 %) |

(XTAL at 512·fs is impossible: IDF requires source > 1.99 × MCLK.)

Decision: **PLL_F160M, MCLK = 256·fs, 32-bit slots, BCLK = MCLK/4 = 64·fs exactly.**
* The fractional pattern repeats at 256 kHz, so the clock error is a deterministic sawtooth whose spurs sit at
  256 kHz and its harmonics, far above the audio band. In the PCM1808 these components produce sidebands
  around 256 kHz that the decimation filter (stopband ≥ 0.6·fs) removes. The PCM5102A is run from its
  **internal PLL locked to BCK** (SCK tied to GND), and that PLL's loop filter attenuates the 256 kHz phase
  sawtooth further.
* Why 256·fs rather than 384/512·fs: BCK and the PCM1808 sampling instants show *identical* jitter for all three
  multiples (1.80 ns rms, lines at n·256 kHz). Their edges are simply the source edges nearest the ideal times, so
  only the source clock matters. 256·fs still wins on MCLK itself: lowest frequency, smallest relative step
  (6.25 ns of 81 ns) and best duty (46.2–53.8 %, i.e. 6/13–7/13).
* Quantified (`docs/clocking/clock_jitter.md`): the raw jitter SNR limit for a full-scale 20 kHz sine is 72.9 dB.
  After the PCM1808 decimation filter (≥65 dB stopband) the in-band limit is 137.9 dB. The DAC side, assuming a
  50 kHz PCM5102A PLL bandwidth (not published), gives 103.2 dB with no in-band spurs (all sidebands at 256 kHz ± f0).
  XTAL@256fs: 7.22 ns rms, 64 kHz lines: 125.9 dB ADC / 68.6 dB DAC. External 1 ps rms oscillator: 138 dB raw.
* The S3 hardware also has a 240 MHz PLL source that IDF 5.5.1 does not expose (~1.19 ns rms, 384 kHz lines, ≈10 dB
  better after the DAC PLL). It would need a register poke and is noted as a future option only.
* XTAL looks attractive (a crystal is cleaner than the PLL), but at 40 MHz the pattern fundamental drops to 64 kHz
  and each step is 4× larger, so it loses.
* `tools/clock_jitter.py` simulates the actual IDF divider sequence for every option and reports the TIE spectrum.
  The table above is reproduced by that script (§12).

### 4.3 Low-jitter mode (build-time: `CONFIG_TWOFOLD_CLOCK_EXT_OSC`)
A 12.288 MHz CMOS oscillator (e.g. SiT1602/ECS-2520MV, ±25 ppm, < 1 ps rms phase jitter) drives
**PCM1808 SCKI** and **PCM5102A SCK**, each through a 33 Ω series resistor near the oscillator.
PCM1808 `MD1=MD0=1` makes it **master at 256·fs**: it outputs BCK = 64·fs and LRCK = fs, which go to the ESP32 (I2S **slave**, both TX and RX)
and to the PCM5102A (slave, SCK present so it bypasses its PLL). The ESP32 MCLK pin is unused (GPIO left as input).
Both converters then run from a crystal oscillator with no fractional division anywhere.

### 4.4 Full duplex, one clock domain
One I2S peripheral (I2S0) runs with TX and RX channels created together (`i2s_new_channel(&cfg, &tx, &rx)`),
sharing BCK/WS in both modes. The audio task blocks on `i2s_channel_read()` for one block, which paces the whole
pipeline to the converter clock. It then processes and `i2s_channel_write()`s exactly one block into a TX ring
pre-filled with 2 blocks of silence. Because TX and RX run off the same BCK, the ring can neither drift nor
underflow in steady state.

### 4.5 USB rate feedback follows whichever clock is master
UAC2 asynchronous mode, using TinyUSB 0.21's `AUDIO_FEEDBACK_METHOD_FIFO_COUNT` on the OUT FIFO and its
level-based IN flow control (47/48/49-frame packets). The FIFOs are filled and drained by the audio task, which is
paced only by the I2S clock. Both loops therefore regulate against the actual converter clock, whether that is the
S3 PLL (internal mode) or the external oscillator (low-jitter mode), with no code change and nothing
mode-specific to calibrate. The OUT FIFO targets half-full (4 ms). Feedback is clamped to ±1 frame.

## 5. Pin map (single source: `firmware/components/board/include/board_config.h`)

> Superseded at bring-up: the board is now the ESP32-S3 SuperMini with the kit wiring (see §14 and docs/wiring.md). The original S3-Zero table follows.

Waveshare ESP32-S3-Zero (ESP32-S3FH4R2) exposes GPIO1–18, 21 (onboard WS2812), 38–45, 43/44 (UART0), 19/20 (USB).
GPIO33–37 are not broken out. Avoided: strapping pins 0/3/45/46, USB 19/20, UART0 43/44 (kept for logs),
JTAG 39–42 (kept free).

| signal | GPIO | dir | notes |
|---|---|---|---|
| OLED SDA | 1 | io | 4.7 kΩ pull-up to 3V3 (most modules have them) |
| OLED SCL | 2 | out | 400 kHz default, 1 MHz option (`CONFIG_TWOFOLD_I2C_FAST`) |
| I2S MCLK | 4 | out | → PCM1808 SCKI (internal clock mode only) |
| I2S BCK | 5 | out / in (ext osc) | → PCM5102A BCK, PCM1808 BCK |
| I2S LRCK | 6 | out / in (ext osc) | → PCM5102A LCK, PCM1808 LRC |
| I2S DOUT | 7 | out | → PCM5102A DIN |
| I2S DIN | 8 | in | ← PCM1808 DOUT |
| DAC XSMT | 9 | out | PCM5102A soft-mute: low during boot/clock changes |
| ENC A | 10 | in | PCNT ch0, pull-up, 10 nF to GND recommended |
| ENC B | 11 | in | PCNT ch1, pull-up, 10 nF to GND recommended |
| ENC SW | 12 | in | to GND, pull-up, debounced in firmware |
| WS2812 | 21 | out | onboard status LED (very dim) |
| UART0 TX/RX | 43/44 | — | console/logs (USB PHY belongs to TinyUSB) |
| spare | 13–18, 38 | — | |

Fixed straps: PCM1808 `FMT=0` (I2S), `MD1=MD0=0` (slave) or `=1/1` (master 256fs, low-jitter mode).
PCM5102A: `FMT=0` (I2S), `FLT=0` (normal latency FIR), `DEMP=0`, `SCK=GND` (internal) or oscillator (low-jitter).

## 6. Memory budget

ESP32-S3: 512 KB SRAM shared by IRAM and DRAM, plus 2 MB quad PSRAM at 80 MHz.
A PSRAM cache-line fill (64 B over QSPI) costs about 1.6–2 µs, roughly 450 CPU cycles. Anything accessed by many
read/write streams per sample (the reverb tank) must therefore stay in internal RAM.

| item | where | size | why |
|---|---|---|---|
| DSP code (`dsp` component, `noflash` linker mapping) + audio task + I2S ISR | IRAM | ~28 KB | no flash-cache misses in the audio path |
| Reverb tank (Dattorro, run at 24 kHz, ~18.2k samples) — *revised, see §14* | internal first (short lines), long delay lines spill to PSRAM if internal free < 96 KB reserve | ~73 KB | short allpasses always internal; long lines are sequential block copies |
| Reverb pre-delay (150 ms mono) | PSRAM | 29 KB | 2 sequential streams: cheap |
| Delay lines (2 × 1.55 s) | PSRAM | 2 × 298 KB | ≤6 sequential streams; est. ≤4 % CPU in cache misses |
| Chorus lines (2 × 2048) | internal | 16 KB | |
| Engine scratch, smoothers, limiter, oversampler | internal | ~12 KB | |
| I2S DMA (3 × 32 frames × 8 B × 2 dirs) | internal | 1.5 KB | |
| TinyUSB FIFOs (OUT 8 ms, IN 8 ms) | internal | ~7 KB | |
| Framebuffers (front/back) + spectrum FFT (1024 cplx) | internal | 2 KB + 12 KB | |
| WiFi (when enabled) | internal ~55 KB + PSRAM (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`) | | allocated only on enable |
| NimBLE (when enabled) | internal ~35 KB + PSRAM (`MEM_ALLOC_MODE_EXTERNAL`) | | allocated only on enable |

Big DSP buffers are allocated at boot, before any radio exists, so later radio enables cannot fragment them.
Measured free-heap figures are in the README (diagnostics screen and `idf.py size`).

## 7. CPU budget at 48 kHz (240 MHz → 5000 cycles per stereo frame = 100 % of one core)

Estimates (est.) are replaced by measurements in README §Measurements.

| block | est. cycles/frame | est. % core |
|---|---|---|
| I/O conversion, input gain, peak/clip, meters | 60 | 1.2 |
| EQ: 10 TDF-II biquads + trims (+ coefficient re-design while moving) | 150 | 3.0 |
| Saturation: 4× oversampled (2-stage polyphase half-band), asym. waveshaper, 2 ch | 300 | 6.0 |
| Chorus/ensemble: 3 voices × 2 ch, cubic interpolation | 150 | 3.0 |
| Delay: 2 heads (+ crossfade heads), filtered feedback, PSRAM | 300 | 6.0 |
| Plate reverb (Dattorro, modulated; tank at 24 kHz + half-band decimate/interpolate) | 250 (+~100 if long lines in PSRAM) | 5.0 (+2) |
| Bus compressor (stereo-linked, log-domain, soft knee) | 120 | 2.4 |
| Monitor mix + master volume + lookahead limiter | 150 | 3.0 |
| TPDF dither × 4 channels, float→int24 | 40 | 0.8 |
| **total, everything on** | **≈1520–1620** | **≈30–32 %** (budget: < 60 %) |

**Measured on the board (§14):** 52.6 % average / 60.8 % worst block with every effect on (radios on or off). The estimate
above was ~4× too optimistic: the in-order Xtensa FPU sustains well under one instruction per cycle on this code.

Measurement: `dsp_engine` brackets each stage with `esp_cpu_get_cycle_count()` and accumulates per-block
avg/max over 1 s windows. These are shown on the hidden Diagnostics screen and in the web API.

## 8. Audio path details
* Format: USB 24-bit in 4-byte subslots (left-justified), I2S 32-bit slots, so all I/O is int32 left-justified.
  Internally float32 at 1.0 = 0 dBFS.
* Playback: USB → EQ (in trim, 5 bands, out trim) → Saturation → Chorus → Delay → Reverb → Bus Comp →
  [direct monitor blend] → master volume → limiter → TPDF dither → 24-bit → DAC.
* Record: ADC → input mode (stereo/mono/L/R) → input gain → peak/clip detect → TPDF dither (bypassed when
  bit-transparent at 0 dB) → USB.
* Direct monitor: crossfade knob (0 % = playback only, 50 % = both −3 dB, 100 % = input only, equal-power).
  The input is taken after input gain. Path option: `DRY` (enters after the effects) or `THRU FX` (enters before
  the EQ). Latency is ADC → DAC through the I2S blocks only, with no USB in the loop.
* Smoothing: every continuous parameter has a one-pole smoother at block rate, interpolated linearly per sample
  for gains. Biquad coefficients are re-designed per block from smoothed (log f, dB, log Q) while moving.
  Discrete changes (delay sync/time, pre-delay, effect on/off, enums) use internal crossfades: two read heads or
  wet/dry equal-power fades. Preset loads raise the glide time to 150 ms (preset epoch), so presets crossfade.
* Safety limiter: 1.0 ms lookahead, peak-hold min filter, smoothed release (gain computed from a
  sliding-window max over the lookahead window, then a 2-stage smoothing), ceiling −0.3 dBFS default. It is
  bit-identical (gain = 1) when the signal stays below the threshold.
* Dither: TPDF (two xorshift32 uniforms, ±1 LSB at 24-bit), independent per channel.
* Denormals: tiny alternating DC offsets in recursive structures, plus flush in smoothers.

## 9. USB
* TinyUSB 0.21 (`espressif/tinyusb`) driven directly. esp_tinyusb's generated `tusb_config.h` has no
  audio class, so we provide our own `tusb_config.h` and init the PHY via `usb_new_phy()`.
* Composite: IAD{ AC(clock source 48k fixed, IT USB-streaming→OT line out, IT line in→OT USB-streaming),
  AS out (alt1 2ch/24bit, iso async EP 0x01 + feedback EP 0x81), AS in (alt1 2ch/24bit, iso async EP 0x82) }
  + MIDI (AC + MS, bulk EP 0x03/0x83).
* No USB feature-unit volume: the encoder is the hardware master volume, and the OS volume stays software
  and bit-transparent at 100 %.
* VID 0x303A (Espressif) with a placeholder PID. A real product must obtain its own PID.

## 10. UI screen map (128×64, 1-bit)

```
BOOT (wordmark build-up, 1.2 s) → HOME
HOME  rotate: volume (accelerated) · press: MENU · double: mute · long: (none)
MENU  EQ · GAIN · EFFECTS · NETWORK · BLUETOOTH · PRESETS · SETTINGS        (long = back)
  EQ        live curve + optional spectrum; rotate=band, press=edit FREQ→GAIN→Q, list: EQ on, trims
  GAIN      input gain + live meter, input mode, monitor mix, monitor path
  EFFECTS   SATURATE · CHORUS · DELAY · REVERB · COMPRESS (each: on/off, visualization, params)
              saturate: transfer curve + live dot   chorus: voice LFO traces
              delay: tap train (ping-pong alternates) reverb: pre-delay gap + decay envelope, damping
              compress: transfer curve, threshold/knee, live GR bar
  NETWORK   status, WiFi on/off, setup portal (SSID/pass/URL), forget network
  BLUETOOTH BLE-MIDI on/off, status, name, help ("no audio over Bluetooth: ESP32-S3 has no BT Classic")
  PRESETS   8 slots: load (crossfade), save (confirm), active/modified marker
  SETTINGS  brightness, dim, screensaver, OLED type, limiter, MIDI, tempo, radios at boot, about
              about → hold 3 s → DIAGNOSTICS (CPU per block, xruns, FIFO/feedback, heap, clock mode)
SCREENSAVER  dim → slow drifting minimal glyph; any input wakes (wake input is swallowed)
```
Fonts: two custom bitmap fonts, a 5×7 label font (caps, digits, symbols) and a large proportional numeral
font (~11×20) for values. Both are designed in-repo as ASCII art and converted at build time.
Motion: critically damped tweens / ease-out cubic for screen slides (≈180 ms), value tweening, 60 fps target.

## 11. Enclosure approach
Parametric OpenSCAD (generated from Python per the enclosure skill), two parts (tray + lid) that print without supports, joined
with M3 screws into heat-set inserts. Modules sit on internal standoffs (M2). The OLED sits behind a window with a
recessed bezel, and the EC11 mounts through the lid (M7 nut, anti-rotation slot). Two 3.5 mm panel jacks for input,
and the DAC module's jack or a panel jack for output (switch). The USB-C cutout is aligned to the S3-Zero. The S3
antenna end faces the opposite end from the audio modules. Rubber-foot recesses. Matching knob. STLs + renders.

## 12. Verification plan
* `host/`: CMake project compiling `params`, `dsp`, `gfx`, `ui` for Linux.
  * `twofold_sim`: renders every screen and transition to PNG and an animated GIF of a navigation script.
  * `dsp_tests`: EQ magnitude vs analytic RBJ target (≤0.05 dB), reverb/delay impulse responses (plots + RT60),
    saturation aliasing (sine sweep; image components ≤ −80 dBc in band), limiter ceiling (never above ceiling
    with full-scale bursts and intersample tests), click detection on every parameter ramp (high-passed
    residual vs threshold), dither statistics, bypass bit-exactness.
* `tools/loopback.py`: latency (MLS cross-correlation), frequency response (log sweep), THD+N (1 kHz, notch),
  noise floor (A-weighted). Run in Studio Mode vs WiFi-on and diff the results.
* `tools/clock_jitter.py`: divider pattern / TIE spectrum analysis behind §4.2.
* Firmware: `idf.py build` clean (-Werror on our components), `idf.py size` for memory.
* CPU: on-device cycle counters. Before hardware exists, an estimate from instruction counts (§14).

## 13. Assumptions
1. The S3-Zero is the FH4R2 variant: quad PSRAM 2 MB at 80 MHz, 4 MB flash at 80 MHz QIO.
2. PCM5102A module = common "GY-PCM5102" purple board: solder jumpers FLT/DEMP/XSMT/FMT on the back, and the
   XSMT jumper is left open so GPIO9 can drive it. SCK pin on header.
3. PCM1808 module exposes FMT, MD0, MD1 and SCKI, runs from 5 V (analog) / 3.3 V (digital) with an onboard LDO, and has
   a line-level input (~1 Vrms full scale for 0 dBFS; no mic preamp).
4. The OLED is the 4-pin I2C variant at address 0x3C. SH1106 modules have 132-column RAM (offset 2).
5. The EC11 has 1 full quadrature cycle per detent (4 counts), 20 or 24 detents, with switch to GND.
6. The host runs a normal OS mixer. On Windows, the in-box usbaudio2.sys (Win10 1703+).
7. 48 kHz only. Supporting 44.1 kHz would need a different MCLK, and on the S3 PLL that is equally fractional;
   OS resamplers handle it fine.
8. NVS partition 24 KB; the flash supports neither auto-suspend nor a guaranteed erase time (see §8 storage policy).
9. We cannot run code on real hardware here. CPU numbers are measured with cycle counters on device. Off
   device, we report a QEMU instruction-count estimate if available, otherwise static estimates, clearly labelled.

### Storage policy (flash vs. audio)
On the ESP32-S3, any flash erase/program disables the cache and stalls both CPUs for its duration (page program ≈0.5 ms,
sector erase ≈45 ms, up to 300+ ms). USB ISO packets arriving meanwhile are lost. "Audio never waits on NVS" is
therefore enforced by *when* we write:
* Changes mark state dirty. The storage task commits only in an **audio-safe window**: (a) no USB stream active
  and output silent, or (b) output silent below −90 dBFS for ≥ 2 s, or (c) an explicit user save/preset store.
  In case (c) the engine first ramps the output to digital silence (5 ms), the write happens, then it ramps back.
* `I2S auto_clear` + `CONFIG_I2S_ISR_IRAM_SAFE`: if the CPU is stalled, the DAC receives zeros, not repeated
  buffers.
* A dirty state older than 10 min is committed with the same soft-mute protection.
* Optional `CONFIG_SPI_FLASH_AUTO_SUSPEND` for flash chips that support it (documented, default off).

## 14. Deviations / log
* (initial) `usbdev` instead of `usb` component name (IDF name clash).
* (initial) esp_tinyusb not used (no audio-class config); TinyUSB used directly with own config.
* Clock analysis (tools/clock_jitter.py) confirmed the §4.2 table and corrected the *rationale*: BCK jitter is
  independent of the MCLK multiple. The decision (PLL_F160M, 256·fs) is unchanged.
* (integration) **Static internal RAM was larger than budgeted.** `idf.py size` shows 124 KB DIRAM left after static
  allocation with WiFi+BT linked (IRAM code incl. 58 KB of DSP). That cannot hold the 146 KB full-rate reverb tank and
  still keep room for the radios. Fix: the tank runs at **24 kHz** (HB1 half-band decimator/interpolator, wet
  bandwidth ~10 kHz like the EMT 250 / Lexicon 224, below where the damping already rolls off). That halves memory
  (~73 KB) and tank CPU. Lines are allocated shortest-first through an allocator that keeps 96 KB internal free for
  stacks + radios and spills only the long sequential delay lines to PSRAM. RT60 accuracy and decorrelation re-verified
  (docs/test-results).
* (integration) Parameter smoothing changed from one one-pole to **two cascaded one-poles** (zero initial slope):
  the host click test caught onset transients on full-range jumps (volume 0 dB → −inf from MIDI/web). Saturation
  tone/tilt coefficients are now interpolated per sample. All 78 parameters pass the click test.
* (integration) The enclosure generator pipeline was replaced by one hand-written, self-contained parametric
  `enclosure/twofold.scad` (the sourced module dimensions were kept) plus `enclosure/check.sh` (OpenSCAD intersection
  checks graded by volume). Front height 26 mm (was 24) so the M3 insert bosses are long enough at the front.
* (bring-up) **Target board changed to the ESP32-S3 SuperMini with the kit / gesture-recorder wiring**: I2S 7/8/9/10,
  I2C 12/13, encoder 4/5/6, MAX98357A enable 11, RGB LED 48, MCLK (PCM1808 only) on spare GPIO21; PCM5102A XSMT
  bridged high (output enable = amp SD only). The kit input is an INMP441 mic (mono, left slot); a PCM1808 still
  drops in on GPIO10 + MCLK 21. §5 below is the original S3-Zero plan; `board_config.h` / docs/wiring.md are current.
* (bring-up) **CPU measured on the board: 130 % with all effects** (the estimate above said ~31 %). Fixed to 52.6 %, step by
  step with on-device sub-stage cycle counters (SysEx 07). The causes and fixes:
  * `-fno-builtin-memcpy` made the float-punning helpers ROM calls per sample;
  * parameters were re-read every block;
  * filter state round-tripped through memory every sample;
  * the chorus used 6 Hermite taps; it now uses 3 on mid/side lines;
  * saturation dropped from 4× to 2× oversampling (Kconfig option).

  WiFi (AP + web) measured at ~96 KB of internal RAM, so the reverb's long lines stay in PSRAM.
* (DSPi review) Features taken from WeebLabs' DSPi after comparing: USB-MIDI SysEx service channel (reboot to the ROM
  bootloader, status, crash report), loudness compensation (ISO 226:2003, follows the volume target), headphone crossfeed
  (mid/side BS2B form, mono bit-exact), the web API over a USB vendor interface (WebUSB/WinUSB; the S3 has only one IN
  endpoint left, so no USB networking), and an SVF/biquad hybrid EQ after measuring float-biquad precision at LF.
* (memory) WiFi + BLE together: ui/input/led/net_dns/net_push stacks moved to PSRAM, spectrum ring to PSRAM,
  g_param_desc forced into flash rodata (a const table with pointers lands in .data.rel.ro = DRAM), WiFi AMPDU/static
  buffers trimmed, BLE scan-duplicate cache off, esp_err_to_name table off. Idle internal heap 125 → 130 KB and both
  radios fit (WiFi AP + web ~73 KB, BLE ~31 KB, 30 KB spare). Both have heap guards (refuse, not crash).
* (initial) The S3 I2C controller has no DMA. The display uses the IDF interrupt-driven asynchronous I2C master
  on core 0 with a dirty-band flush. This meets "never block audio" (different core, no shared locks) without DMA.

## 15. Roadmap (planned variants, not built)

Both variants are build-time options of the same firmware (Kconfig), same PCB wiring plus one DAC module for B. The
hard limit that shapes both is the USB **Full Speed frame budget**: 1 ms frames, at most ~1350 B of isochronous data per
frame (90 % of 1500 B, before packet overhead and bit stuffing), 1023 B per packet. UAC2 audio uses it as:

| configuration                    | OUT B/ms | IN B/ms | total | fits FS? |
|----------------------------------|---------:|--------:|------:|----------|
| today: 2 in / 2 out, 48 k, 4-byte subslots | 384 | 384 | 768 | yes |
| A: 2 in / 2 out, 96 k, 4-byte subslots     | 768 | 768 | 1536 | **no** |
| A: 2 in / 2 out, 96 k, 3-byte subslots     | 576 | 576 | 1152 | yes (tight, ~1220 B with overhead) |
| B: 2 in / 4 out, 48 k, 4-byte subslots     | 768 | 384 | 1152 | yes |
| B: 2 in / 4 out, 48 k, 3-byte subslots     | 576 | 288 |  864 | yes |
| A+B: 2 in / 4 out, 96 k, 3-byte subslots   | 1152 | 576 | 1728 | **no** |

So A and B each fit; A+B does not on one S3 (the S3 has no High Speed USB). No new endpoints are needed for either
(channels and rate live inside the existing isochronous pipes), which matters because all 5 IN endpoints are in use.

### 15.1 Variant A: 96 kHz (`CONFIG_TWOFOLD_FS_96K`)
* **Clock: unchanged.** MCLK stays 12.288 MHz (now 128·fs instead of 256·fs), the same 13+1/48 divider from
  PLL_F160M, so the measured jitter figures of §4 carry over. BCLK = 64·fs = 6.144 MHz, WS = 96 kHz. PCM5102A takes
  128·fs at 96 k (and the kit board runs it from BCK anyway); PCM1808 in slave mode auto-detects 128·fs at double
  rate. *Check both datasheet tables on real parts (§"needs hardware").* The external-oscillator low-jitter mode needs
  no change either (12.288 MHz).
* **USB: 3-byte subslots** (`bSubslotSize=3, bBitResolution=24`) in both directions, because 4-byte packing does not
  fit the frame (table above). The pack/unpack between USB packets and the engine's left-justified int32 becomes a
  byte shuffle on the USB task (≈ 2 cycles per sample, nothing). Feedback value scales with fs (already computed from
  the measured rate).
* **Block size 64 frames** (same 667 µs block period and interrupt rate as now; same latency in ms).
* **DSP budget halves: 2500 cycles per frame.** Today's 48 k figures scaled to 96 k:
  * EQ (5 bands SVF/biquad), compressor, limiter, loudness, crossfeed, dither: ≈ 24 % → fits.
  * Saturation: oversampling becomes unnecessary at 96 k (the 2× halfband pair goes, the shaper runs 1×): ≈ 8 %.
  * Chorus, delay, reverb: **off in this variant.** Their delay lines double in size (chorus lines are internal RAM
    and already hot), and the reverb tank would need a 4:1 decimator to keep its 24 kHz rate. Possible later, not in
    the first cut. UI hides the three panels, the params stay in the table (presets remain compatible).
  * Expected total ≈ 35–40 % of core 1 with everything that is on.
* **Memory:** USB packet buffers grow (576 B per packet, ×2 for double buffering, internal RAM); I2S DMA ring doubles
  (+6 KB internal); limiter lookahead and smoother tables double (small). Net ≈ −10 KB internal, within the 30 KB spare
  with both radios on, but the radio heap guards must be re-checked.
* **Host tests:** every dsp_test takes `--fs`; run the suite at 96 k; `tools/loopback.py --fs 96000`.
* Not in scope: 44.1/88.2 (not a clean divider from the S3 PLL, §4.2); sample-rate switching at run time (the device
  advertises one rate; the OS resamples).

### 15.2 Variant B: 4 outputs (`CONFIG_TWOFOLD_OUT4`)
* **Hardware:** a second PCM5102A module. It shares BCK and LRCK with the first DAC (same wires) and gets its own
  data line DOUT2 on a spare GPIO (candidate: GPIO14). XSMT bridged high like the first.
* **Clocking: one clock domain.** I2S0 keeps full duplex (ADC + DAC1). I2S1 runs **TX only, as a slave** of I2S0's
  BCLK/WS, which are looped back through the GPIO matrix (the pads are outputs for I2S0 and inputs for I2S1). Both
  DACs are then sample-locked with zero drift and a known constant phase; no second divider, no resampler. (TDM on one
  data line is out: the PCM5102A speaks plain I2S only.)
* **USB:** the OUT AudioStreaming interface advertises 4 channels (`bNrChannels=4`, channel config FL FR + two
  "undefined"/aux), 48 k, 4-byte subslots (fits: 768 B/ms). IN stays stereo. Linux, macOS and the Windows 10
  (1703+) class driver all expose a 4-out device; DAWs see outputs 1-2 and 3-4.
* **Routing (new params, settings group, saved in presets):**
  * `out34.src`: *USB 3-4* (second stereo bus from the host, own volume + limiter, no effects: a cue/headphone or
    sub/aux feed), *main dup* (copy of 1-2 after the limiter), *dry monitor* (input blend without effects, for
    tracking), *wet only* (effects return, for an external mixer).
  * `out34.volume`, `out34.lim_on`.
* **DSP cost:** second volume + limiter + dither pair ≈ +6 % of core 1 (limiter is the expensive part; the *main dup*
  and *wet only* routes cost only the dither). All effects stay on at 48 k: 53 % → ~59 %.
* **Memory:** I2S1 DMA ring ≈ 3 KB internal, USB OUT packets 768 B ×2. Negligible.
* **UI:** one extra meter pair on the home screen (narrow bars) and an "OUT 3-4" settings page; the web editor gets
  the routing selector.
* **Enclosure:** two more RCA/TRS jacks on the rear; the SCAD rear panel is parametric on the jack list.

### 15.3 Beyond one S3
* A+B (4 out at 96 k) needs High Speed USB: not this chip. The DSPi-class answer is a different platform, not a variant.
* Effects at 96 k: a second ESP32-S3 as an I2S co-processor (chorus/delay/reverb on chip 2, I2S round trip ≈ 1.3 ms,
  parameters mirrored over UART). Bigger project than A and B together; only worth it if A ships and the effects are
  missed. An ESP32-C3 brings nothing here (no FPU, no USB OTG, no full-duplex I2S).
* S/PDIF out: a WM8804 or a bit-banged biphase encoder on the second I2S's TX, fed from the same clock domain. Cheap
  in hardware, moderate in firmware; shares I2S1 with variant B unless the DAC2 goes on a third data line of I2S0
  (TDM is not available for the PCM5102A, but I2S0 can still drive one stereo line only, so S/PDIF and DAC2 compete
  for I2S1).
* (production) **Tester release work.** OTA rollback enabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`): a new
  image is confirmed by `main/ota_confirm.c` only after 20 s of clean audio (blocks flowing, no I2S errors), the
  otadata write goes under soft-mute in the sys task; no clean audio within 3 min, or a panic, brings the old image
  back. ctrl task stack 4 → 6 KB (1.4 KB was free). Boot-time counter artifacts removed: DMA overflows between
  `i2s_channel_enable()` and the first loop iteration (the I2S log line alone took ~10 ms, longer than the 4-block
  ring) are no longer counted and the log moved before the enable; record drops in the first ~10 ms after a record
  start (the host polls IN a little after SetInterface) are not counted; DMA overflows within 500 ms of our own
  flash writes (storage/OTA call `audio_flash_write_hint()`) count as `flash-stall`, not I2S errors. A fresh boot
  now shows all counters at 0. `tools/release.sh` builds the tester bundle, `docs/QUICKSTART.md` is the one-page
  guide, `tools/loopback.py --rec-device` measures the DAC path through another interface's line inputs.
