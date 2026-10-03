# TWOFOLD tools

Host-side Python tools for analysis, measurement and documentation. They use only the standard library,
`numpy` and `matplotlib`. `loopback.py` also uses `sounddevice`, but only at runtime and only for real
measurements. `scipy` is not used.

```
python3 -m pip install --user numpy matplotlib            # all tools
python3 -m pip install --user sounddevice                 # loopback.py on real hardware
# sounddevice needs PortAudio: Arch `pacman -S portaudio`, Debian/Ubuntu `apt install libportaudio2`
```

| tool | what it does | writes |
|---|---|---|
| `clock_jitter.py` | ESP32-S3 I2S fractional MCLK divider: IDF 5.5.1 divider math, edge sequence, TIE/jitter spectra, jitter-limited SNR (PLAN.md §4.2) | `docs/clocking/` |
| `loopback.py` | Loopback measurement suite through the device: latency, FR, THD+N, DR, noise, crosstalk, long-run click/slip test, Studio-vs-WiFi diff report | `tools/loopback_results/` (or `--out`) |
| `gen_wiring.py` | Wiring tables, power/ground, converter strapping for both clock modes, and ASCII block diagrams, generated from `board_config.h` | `docs/wiring.md` |

## clock_jitter.py

```
python3 tools/clock_jitter.py            # regenerate docs/clocking/clock_jitter.md, .json, *.png
python3 tools/clock_jitter.py --check    # internal consistency checks only
python3 tools/clock_jitter.py --model bunched --pll-fc 20e3 --xo-rj-ps 2 --out /tmp/cj
```

* Exact ports of `hal_utils_calc_clk_div_frac_accurate()`, `i2s_ll_tx_set_mclk()` (x/y/z/yn1) and the
  range checks and `bclk_div` logic of `i2s_std_calculate_clock()`.
* The TRM documents only the register encoding, not where the N+1 periods fall. Three orderings are modelled:
  `nested` (default), `ideal` and `bunched` (worst case). They give the same result for PLL_F160M at
  256·fs (13 + 1/48).
* Reports MCLK and BCLK TIE (p-p, rms, line spectrum), period jitter, duty-cycle range, and three SNR figures
  for a full-scale 20 kHz sine: the raw limit −20·log10(2π f σ), the PCM1808 in-band error after a
  decimation-filter model, and the PCM5102A error with and without an assumed BCK-PLL jitter low-pass.
  It also covers the external-oscillator low-jitter mode (`--xo-rj-ps`).
* `--check` asserts the IDF ports on known values, the TRM identity a = z(x+1)+y, Parseval on the line
  spectra, and that a brute-force time-domain simulation matches the formula to within 0.2 dB.

## loopback.py

Cable the line outputs to the line inputs (L→L, R→R). On the device: DSP bypassed, master volume 0 dB,
input gain 0 dB, direct monitor 0 %. Use the raw ALSA `hw:` device if possible (bit-exact). If PipeWire
holds it, stop it for the session (`systemctl --user stop pipewire pipewire-pulse wireplumber`) or use
the PipeWire device at 48 kHz with OS volumes at 100 %.

```
python3 tools/loopback.py --selftest                 # synthetic verification, no device (run this first)
python3 tools/loopback.py --list-devices
python3 tools/loopback.py --label studio             # full suite -> studio.json/.md/.png
python3 tools/loopback.py --label wifi               # after enabling WiFi on the device
python3 tools/loopback.py --report studio wifi       # diff -> compare_studio_vs_wifi.md/.png
python3 tools/loopback.py --compare                  # both runs with prompts, then the diff
python3 tools/loopback.py --drift 180                # 3 h continuous sine: clicks / sample slips / xruns
python3 tools/loopback.py --label studio --drift-minutes 30   # suite + 30 min drift run
```

The device is found automatically: it is the first duplex PortAudio device whose name contains "TWOFOLD",
with `hw:` entries preferred. Override with `--device <index|substring>`.

| measurement | method |
|---|---|
| latency | MLS (order 15) cross-correlation with band-limited sub-sample peak refinement; `--latency-runs` separate streams give the min/max/std spread; also reports L/R skew and polarity |
| frequency response | Farina exponential sweep (10 Hz–22 kHz, 6 s) deconvolved with the inverse filter. The linear IR is windowed and divided by the stimulus' own deconvolution. Reports max deviation 20 Hz–20 kHz re 1 kHz and loop gain |
| THD+N / THD | 1 kHz at −1 and −20 dBFS, 65536-point 7-term Blackman-Harris. The fundamental is removed in the FFT domain (±10 bins), the residual is integrated over 20 Hz–20 kHz, harmonics up to 20 kHz are summed. Reported in dB and % |
| dynamic range | AES17-like: THD+N of a −60 dBFS tone, A-weighted (IEC 61672 curve implemented in numpy), + 60 dB |
| noise floor | idle channels, 20 Hz–20 kHz, unweighted and A-weighted, in dBFS (full-scale sine = 0 dBFS) |
| crosstalk | 1 kHz on one channel. The other channel's 1 kHz component is reported relative to it, both directions |
| drift / clicks | Continuous sine. Each 50 ms block is least-squares fitted with phase referenced to the absolute sample index. A residual spike is reported as a *click*, a stable whole-sample phase step as a *slip* (+ = samples lost), and PortAudio status flags as *xruns*. With working async feedback all three must be zero over hours |

`--selftest` drives the real measurement code against a `SyntheticDevice` whose truth is known. The device
has a 3-tap FIR with an analytic response, a 517.3-sample fractional delay, 2nd/3rd-order nonlinearity,
−100 dBFS white noise and −80 dB crosstalk. The test checks that every figure is recovered within
tolerance: latency ±0.03 samples, FR ±0.1 dB, THD/THD+N ±0.3 dB, DR and noise ±0.3 dB, crosstalk ±0.2 dB.
It also checks that the drift detector finds an injected dropped sample, an injected duplicated sample and a
−60 dBFS one-sample spike at the right times, with no false events on a clean run, and that the diff report
flags a degraded noise floor. It also verifies the A-weighting against IEC table values and the MLS
properties.

## gen_wiring.py

```
python3 tools/gen_wiring.py            # (re)write docs/wiring.md; prints "unchanged" when nothing changed
python3 tools/gen_wiring.py --check    # exit 1 if docs/wiring.md is stale (for CI)
python3 tools/gen_wiring.py --stdout
```

Parses every `#define BOARD_PIN_* <gpio> /* PIN: signal | module pin(s) | notes */` in
`firmware/components/board/include/board_config.h`. Output is deterministic, with no timestamps, so
re-running is idempotent. Fails if a GPIO is assigned twice. To change wiring, edit the header comments
and re-run the script.
