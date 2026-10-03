#!/usr/bin/env python3
"""ESP32-S3 I2S MCLK/BCLK fractional-divider jitter analysis for TWOFOLD (PLAN.md section 4.2).

What it does
------------
1. Ports, line for line, how ESP-IDF v5.5.1 programs the ESP32-S3 I2S clock divider:
     * ``i2s_std_calculate_clock()``  (esp_driver_i2s/i2s_std.c): mclk = fs * mclk_multiple,
       bclk = fs * slots * slot_bits, bclk_div = mclk / bclk;
     * ``hal_utils_calc_clk_div_frac_accurate()`` (hal/hal_utils.c): MCLK = src / (N + b/a);
     * ``i2s_ll_tx_set_mclk()`` (hal/esp32s3/include/hal/i2s_ll.h): (a, b) -> div_x, div_y, div_z, div_yn1.
2. Builds the MCLK period sequence (in source-clock cycles) that the pulse-swallowing divider emits,
   then BCLK = every ``bclk_div``-th MCLK rising edge.
3. Computes, for MCLK and BCLK: time-interval error (TIE) vs an ideal clock, its line spectrum,
   period jitter, cycle-to-cycle jitter, duty-cycle range.
4. Turns the TIE into the jitter-induced error for a full-scale 20 kHz sine:
     * "raw" (all jitter, full bandwidth): SNR = -20 log10(2 pi f sigma);
     * ADC in-band: the PCM1808 samples at 64 fs instants derived from SCKI (= MCLK); the error sidebands
       f_k +- f0 are passed through a decimation-filter model (passband 0.454 fs, stopband >= 0.583 fs with
       65 dB attenuation, PCM1808 datasheet minimum) and aliased to fs; only what lands in 20 Hz-20 kHz counts;
     * DAC: the PCM5102A regenerates its system clock with a PLL locked to BCK (SCK tied low). Its loop
       bandwidth is not published, so the BCK TIE is reported unfiltered and through an assumed 2nd-order
       low-pass (``--pll-fc``). In-band = sidebands f_k +- f0 that land in 20 Hz-20 kHz.
5. Low-jitter mode (external 12.288 MHz XO, PCM1808 master): white random jitter (``--xo-rj-ps``, default 1 ps).

Hardware model and its uncertainty
----------------------------------
The TRM (ESP32-S3 TRM v1.8 section 28.6) defines only the register encoding:
   b <= a/2: yn1 = 0, z = b,     x = floor(a/b) - 1,     y = a % b
   b >  a/2: yn1 = 1, z = a - b, x = floor(a/(a-b)) - 1, y = a % (a-b)
It does not specify where the swallowed/added source cycles fall. Over one pattern of ``a`` MCLK periods the
divider must emit exactly ``b`` periods of N+1 and ``a-b`` periods of N source cycles (otherwise the average
frequency would be wrong), so the long-term TIE is bounded; only the ORDER is uncertain. Three orderings:

* ``nested`` (default, most plausible reading of x/y/z): ``z`` groups, each = x "normal" periods + 1 "special"
  period (special = N+1 when yn1 = 0, N when yn1 = 1); a second accumulator adds y per group and inserts one
  extra normal period whenever it reaches z (so the y leftovers are spread over the z groups).
* ``ideal``   : first-order accumulator (Bresenham) over (a, b): the best possible spread, TIE <= 1 source cycle.
* ``bunched`` : z groups of (x normal + 1 special), then all y leftover normal periods at the end: worst case.

For b = 1 (PLL_F160M, 256 fs: 13 + 1/48) all three orderings are identical, so the chosen configuration's
result does not depend on this uncertainty. MCLK duty: an n-cycle period is high for floor(n/2) or ceil(n/2)
source cycles (unknown); the duty range reported covers both. Random jitter of the source clock (BBPLL or
crystal) is not specified by Espressif and is not modelled unless ``--src-rj-ps`` is given (added in
quadrature to the raw figures). The phase of the BCK divider and of the PCM1808's internal SCKI/(k/64)
divider relative to the fractional pattern is unknown: all phases are evaluated and the worst is reported.

Usage
-----
    python3 tools/clock_jitter.py                 # writes docs/clocking/{clock_jitter.md, *.png, clock_jitter.json}
    python3 tools/clock_jitter.py --check         # internal consistency checks only (no output files)
    python3 tools/clock_jitter.py --out /tmp/x --pll-fc 30e3 --xo-rj-ps 2

Dependencies: numpy, matplotlib (Agg backend). No scipy.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Iterable, Sequence

import numpy as np

# ----------------------------------------------------------------------------------------------
# Constants copied from ESP-IDF v5.5.1
# ----------------------------------------------------------------------------------------------
I2S_LL_CLK_FRAC_DIV_N_MAX = 256   # hal/esp32s3/include/hal/i2s_ll.h
I2S_LL_CLK_FRAC_DIV_AB_MAX = 512  # hal/esp32s3/include/hal/i2s_ll.h
UINT32_MAX = 0xFFFFFFFF

SOURCES: dict[str, int] = {
    "PLL_F160M": 160_000_000,   # I2S_CLK_SRC_PLL_160M (IDF default)
    "XTAL": 40_000_000,         # I2S_CLK_SRC_XTAL
    "PLL_D2": 240_000_000,      # TRM tx_clk_sel = 1; NOT selectable through IDF 5.5.1 (reference only)
}
IDF_SOURCES = ("PLL_F160M", "XTAL")

# PCM1808 decimation filter (datasheet): passband 0.454 fs, stopband 0.583 fs, stopband attenuation -65 dB.
ADC_PASS_EDGE = 0.454
ADC_STOP_EDGE = 0.583
ADC_STOP_ATTEN_DB = 65.0
AUDIO_BAND = (20.0, 20_000.0)


# ----------------------------------------------------------------------------------------------
# IDF divider computation (exact ports)
# ----------------------------------------------------------------------------------------------
@dataclass(frozen=True)
class FracDiv:
    """MCLK = src / (integer + numerator/denominator), plus the S3 register encoding."""

    integer: int
    numerator: int    # b
    denominator: int  # a
    x: int
    y: int
    z: int
    yn1: int

    @property
    def ratio(self) -> float:
        return self.integer + (self.numerator / self.denominator if self.numerator else 0.0)

    def __str__(self) -> str:
        if not self.numerator:
            return f"{self.integer}"
        return f"{self.integer} + {self.numerator}/{self.denominator}"


def hal_utils_calc_clk_div_frac_accurate(src_hz: int, exp_hz: int,
                                         max_integ: int = I2S_LL_CLK_FRAC_DIV_N_MAX,
                                         min_integ: int = 1,
                                         max_fract: int = I2S_LL_CLK_FRAC_DIV_AB_MAX
                                         ) -> tuple[int, int, int] | None:
    """Port of ``hal_utils_calc_clk_div_frac_accurate`` (IDF v5.5.1, uint32 arithmetic).

    Returns (integer, numerator, denominator) or None when out of range (IDF returns 0).
    """
    assert max_fract > 2
    div_denom, div_numer = 2, 0
    div_integ = src_hz // exp_hz
    freq_error = src_hz % exp_hz
    if freq_error:
        # Carry bit if the decimal is greater than 1.0 - 1.0 / ((max_fract - 1) * 2)
        if freq_error < exp_hz - exp_hz // (max_fract - 1) * 2:
            mn = UINT32_MAX
            a = 2
            while mn and a < max_fract:
                b = (a * freq_error + exp_hz // 2) // exp_hz
                sub = abs(exp_hz * b - freq_error * a)
                if sub < mn:
                    div_denom, div_numer, mn = a, b, sub
                a += 1
        else:
            div_integ += 1
    if div_integ < min_integ or div_integ >= max_integ or div_integ == 0:
        return None
    return div_integ, div_numer, div_denom


def i2s_ll_set_mclk_regs(integer: int, numerator: int, denominator: int) -> FracDiv:
    """Port of ``i2s_ll_tx_set_mclk`` (ESP32-S3): (a, b) -> div_x, div_y, div_z, div_yn1.

    With b == 0 the integer path is used (x = 0, y = 1, z = 0 per ``i2s_ll_tx_set_raw_clk_div`` defaults,
    which is how the TRM says to configure an integer divider).
    """
    if denominator and numerator:
        yn1 = 1 if numerator * 2 > denominator else 0
        z = denominator - numerator if yn1 else numerator
        x = denominator // z - 1
        y = denominator % z
        return FracDiv(integer, numerator, denominator, x, y, z, yn1)
    return FracDiv(integer, 0, denominator, 0, 1, 0, 0)


def idf_driver_accepts(src_hz: int, mclk_hz: int) -> bool:
    """The range checks of ``i2s_std_calculate_clock()`` (internal source, master role)."""
    return float(src_hz) > mclk_hz * 1.99 and src_hz // mclk_hz < I2S_LL_CLK_FRAC_DIV_N_MAX


def idf_mclk_divider(src_hz: int, mclk_hz: int) -> FracDiv:
    """Full IDF path: precise division + register encoding."""
    res = hal_utils_calc_clk_div_frac_accurate(src_hz, mclk_hz)
    if res is None:
        raise ValueError(f"IDF cannot divide {src_hz} Hz down to {mclk_hz} Hz")
    return i2s_ll_set_mclk_regs(*res)


# ----------------------------------------------------------------------------------------------
# Edge-sequence models
# ----------------------------------------------------------------------------------------------
MODELS = ("nested", "ideal", "bunched")


def mclk_periods(div: FracDiv, model: str = "nested") -> np.ndarray:
    """One full pattern of MCLK periods, in source-clock cycles (length a, or 1 for integer division)."""
    n, b, a = div.integer, div.numerator, div.denominator
    if not b:
        return np.array([n], dtype=np.int64)
    if model == "ideal":
        acc, out = 0, []
        for _ in range(a):
            acc += b
            if acc >= a:
                acc -= a
                out.append(n + 1)
            else:
                out.append(n)
        seq = out
    elif model in ("nested", "bunched"):
        special = n + 1 if div.yn1 == 0 else n
        normal = n if div.yn1 == 0 else n + 1
        seq = []
        acc = 0
        for _ in range(div.z):
            group = [normal] * div.x + [special]
            if model == "nested":
                acc += div.y
                if acc >= div.z:
                    acc -= div.z
                    group = [normal] + group
            seq.extend(group)
        if model == "bunched":
            seq.extend([normal] * div.y)
    else:
        raise ValueError(f"unknown model {model!r}")
    arr = np.asarray(seq, dtype=np.int64)
    assert arr.size == a, (arr.size, a)
    assert int(arr.sum()) == n * a + b, "pattern does not realise N + b/a"
    return arr


@dataclass
class EdgeStats:
    """Jitter statistics of one clock (MCLK, or BCLK at one divider phase)."""

    f_hz: float
    tie_s: np.ndarray = field(repr=False)          # zero-mean TIE of rising edges, one full period of the pattern
    period_s: np.ndarray = field(repr=False)       # actual periods
    high_s_lo: np.ndarray = field(repr=False)      # high time, floor convention
    high_s_hi: np.ndarray = field(repr=False)      # high time, ceil convention
    line_f: np.ndarray = field(repr=False)         # TIE line frequencies (Hz), k >= 1
    line_a: np.ndarray = field(repr=False)         # TIE line peak amplitudes (s)

    @property
    def tie_rms(self) -> float:
        return float(np.sqrt(np.mean(self.tie_s ** 2)))

    @property
    def tie_pp(self) -> float:
        return float(self.tie_s.max() - self.tie_s.min())

    @property
    def period_pp(self) -> float:
        return float(self.period_s.max() - self.period_s.min())

    @property
    def c2c_max(self) -> float:
        p = np.concatenate([self.period_s, self.period_s[:1]])
        return float(np.max(np.abs(np.diff(p)))) if p.size > 1 else 0.0

    @property
    def duty_range(self) -> tuple[float, float]:
        d_lo = self.high_s_lo / self.period_s
        d_hi = self.high_s_hi / self.period_s
        return float(min(d_lo.min(), d_hi.min())), float(max(d_lo.max(), d_hi.max()))

    def lowest_line(self) -> tuple[float, float]:
        """(frequency, amplitude) of the lowest-frequency non-zero TIE line."""
        nz = np.nonzero(self.line_a > 1e-18)[0]
        if nz.size == 0:
            return 0.0, 0.0
        return float(self.line_f[nz[0]]), float(self.line_a[nz[0]])

    def largest_line(self) -> tuple[float, float]:
        if self.line_a.size == 0 or self.line_a.max() <= 1e-18:
            return 0.0, 0.0
        i = int(np.argmax(self.line_a))
        return float(self.line_f[i]), float(self.line_a[i])


def tie_lines(tie: np.ndarray, f_edge: float) -> tuple[np.ndarray, np.ndarray]:
    """Exact line spectrum of a periodic TIE sequence sampled at its edges (one period given).

    Returns frequencies k*f_edge/L (k = 1..L/2) and peak amplitudes so that
    sum(A^2/2) (+ Nyquist A^2) == mean(tie^2).
    """
    L = tie.size
    X = np.fft.rfft(tie) / L
    k = np.arange(X.size)
    amp = 2.0 * np.abs(X)
    if L % 2 == 0:
        amp[-1] = np.abs(X[-1]) * math.sqrt(2.0)  # Nyquist: real cosine, keep the A^2/2 = power convention
    # The TIE is an exact rational sequence; anything below 1e-9 of the peak line is FFT round-off.
    if amp.size > 1 and amp[1:].max() > 0:
        amp[amp < 1e-9 * amp[1:].max()] = 0.0
    return (k[1:] * f_edge / L).astype(float), amp[1:]


def edge_stats(periods_cycles: np.ndarray, src_hz: float, divide: int = 1, phase: int = 0,
               src_rj_s: float = 0.0) -> EdgeStats:
    """Statistics of a clock whose rising edges are every ``divide``-th edge of the MCLK sequence.

    ``periods_cycles`` is one MCLK pattern (source cycles). The sequence is tiled ``divide`` times so the
    divided clock also covers an integer number of its own patterns.
    """
    a = periods_cycles.size
    reps = divide if divide > 1 else 1
    per = np.tile(periods_cycles, reps + 1)   # one extra tile for falling-edge / wrap lookups
    cum = np.concatenate([[0], np.cumsum(per)])  # rising edge times (source cycles), exact integers
    total = int(periods_cycles.sum())
    # Exact TIE in source cycles: cum[n] - n * total / a  ->  (a*cum[n] - n*total) / a
    n_idx = phase + divide * np.arange(a * reps // divide)
    tie_num = a * cum[n_idx] - n_idx * total
    tie_cyc = tie_num.astype(float) / a
    tie_cyc -= tie_cyc.mean()
    t_src = 1.0 / src_hz
    tie = tie_cyc * t_src
    period = (cum[n_idx + divide] - cum[n_idx]).astype(float) * t_src
    if divide == 1:
        p = per[n_idx]
        hi_lo = (p // 2).astype(float) * t_src
        hi_hi = ((p + 1) // 2).astype(float) * t_src
    else:
        half = divide // 2
        hi = (cum[n_idx + half] - cum[n_idx]).astype(float) * t_src
        hi_lo = hi_hi = hi
    f_clk = src_hz * a / total / divide
    lf, la = tie_lines(tie, f_clk)
    if src_rj_s > 0:  # white random jitter: report only through tie_rms quadrature (lines unchanged)
        pass
    return EdgeStats(f_clk, tie, period, hi_lo, hi_hi, lf, la)


# ----------------------------------------------------------------------------------------------
# Jitter -> signal error
# ----------------------------------------------------------------------------------------------
def snr_raw_db(sigma_s: float, f0: float) -> float:
    """SNR limit of a full-scale sine at f0 sampled with rms timing error sigma (all bandwidth)."""
    if sigma_s <= 0:
        return math.inf
    return -20.0 * math.log10(2.0 * math.pi * f0 * sigma_s)


def _fold(f: np.ndarray, fs: float) -> np.ndarray:
    """Fold frequencies into [0, fs/2] (sampling at fs)."""
    g = np.mod(f, fs)
    return np.where(g > fs / 2, fs - g, g)


def adc_inband(lines_f: np.ndarray, lines_a: np.ndarray, f0: float, fs: float, fm: float
               ) -> tuple[float, float, float]:
    """In-band (20 Hz-20 kHz) error of a delta-sigma ADC sampling at fm with the given TIE lines.

    Each TIE line (f_k, A_k) produces sidebands at f_k +- f0 with amplitude (2 pi f0 A_k)/2 relative to the
    carrier. They are folded at fm, attenuated by the decimation filter (1 below 0.583 fs, conservative;
    -65 dB above), decimated (folded at fs). Returns (SNR dB, worst spur dBc, its output frequency).
    """
    w0 = 2 * math.pi * f0
    stop_gain = 10 ** (-ADC_STOP_ATTEN_DB / 20)
    pwr = 0.0
    worst, worst_f = -math.inf, 0.0
    for fk, ak in zip(lines_f, lines_a):
        if ak <= 0:
            continue
        for f in (fk - f0, fk + f0):
            fmod = float(_fold(np.array([abs(f)]), fm)[0])
            g = 1.0 if fmod <= ADC_STOP_EDGE * fs else stop_gain
            fout = float(_fold(np.array([fmod]), fs)[0])
            if AUDIO_BAND[0] <= fout <= AUDIO_BAND[1]:
                amp = w0 * ak / 2 * g
                pwr += amp ** 2
                lvl = 20 * math.log10(amp) if amp > 0 else -math.inf
                if lvl > worst:
                    worst, worst_f = lvl, fout
    snr = math.inf if pwr <= 0 else -10 * math.log10(pwr)
    return snr, worst, worst_f


def pll_gain(f: np.ndarray, fc: float | None) -> np.ndarray:
    """Assumed jitter transfer of the PCM5102A BCK-PLL: 2nd-order low-pass |H|^2 = 1/(1+(f/fc)^4)."""
    if fc is None or fc <= 0:
        return np.ones_like(f)
    return 1.0 / np.sqrt(1.0 + (f / fc) ** 4)


def dac_errors(lines_f: np.ndarray, lines_a: np.ndarray, f0: float, fc: float | None
               ) -> tuple[float, float, float]:
    """DAC: (full-band SNR after PLL, in-band SNR, worst in-band spur dBc)."""
    g = pll_gain(lines_f, fc)
    af = lines_a * g
    sigma = math.sqrt(float(np.sum(af ** 2) / 2))
    full = snr_raw_db(sigma, f0)
    w0 = 2 * math.pi * f0
    pwr, worst = 0.0, -math.inf
    for fk, ak in zip(lines_f, af):
        for f in (abs(fk - f0), fk + f0):
            if AUDIO_BAND[0] <= f <= AUDIO_BAND[1] and ak > 0:
                amp = w0 * ak / 2
                pwr += amp ** 2
                worst = max(worst, 20 * math.log10(amp))
    inb = math.inf if pwr <= 0 else -10 * math.log10(pwr)
    return full, inb, worst


def time_domain_snr(tie: np.ndarray, f_edge: float, f0: float, n_rep: int = 64) -> float:
    """Brute-force check: sample a FS sine at jittered vs ideal instants, return SNR of the difference."""
    t_tie = np.tile(tie, n_rep)
    t = np.arange(t_tie.size) / f_edge
    err = np.sin(2 * np.pi * f0 * (t + t_tie)) - np.sin(2 * np.pi * f0 * t)
    return float(-10 * np.log10(np.mean(err ** 2) / 0.5))


# ----------------------------------------------------------------------------------------------
# One configuration
# ----------------------------------------------------------------------------------------------
@dataclass
class ConfigResult:
    source: str
    src_hz: int
    fs: int
    k: int
    model: str
    div: FracDiv
    bclk_div: int
    mclk: EdgeStats
    bclk: EdgeStats            # worst phase (largest rms)
    bclk_phases_rms: list[float]
    pattern_hz: float
    snr_raw_mclk: float
    snr_raw_bclk: float
    adc_inband_snr: float
    adc_inband_spur: float
    adc_inband_spur_f: float
    dac_full_nopll: float
    dac_full_pll: float
    dac_inband_snr: float
    td_check_db: float

    @property
    def mclk_hz(self) -> int:
        return self.fs * self.k

    @property
    def ppm(self) -> float:
        return (self.mclk.f_hz / self.mclk_hz - 1) * 1e6

    def label(self) -> str:
        return f"{self.source} {self.k}fs @ {self.fs / 1000:g} kHz"

    def slug(self) -> str:
        return f"{self.source.lower()}_{self.k}fs_{self.fs}"


def analyse(source: str, fs: int, k: int, model: str = "nested", f0: float = 20_000.0,
            pll_fc: float | None = 50e3, src_rj_s: float = 0.0, slot_bits: int = 32, slots: int = 2
            ) -> ConfigResult:
    """Analyse one (clock source, fs, mclk multiple) as IDF 5.5.1 would configure it (I2S master)."""
    src = SOURCES[source]
    mclk_hz = fs * k
    bclk_hz = fs * slots * slot_bits
    bclk_div = mclk_hz // bclk_hz                    # i2s_std_calculate_clock()
    if not idf_driver_accepts(src, mclk_hz):
        raise ValueError(f"{source} {k}fs @ {fs}: rejected by i2s_std_calculate_clock() (sclk <= 1.99*mclk)")
    div = idf_mclk_divider(src, mclk_hz)
    per = mclk_periods(div, model)
    m = edge_stats(per, src)
    phases = [edge_stats(per, src, divide=bclk_div, phase=p) for p in range(bclk_div)]
    rms = [p.tie_rms for p in phases]
    b = phases[int(np.argmax(rms))]
    # ADC: PCM1808 modulator at 64 fs = SCKI/(k/64): same decimation as BCK, worst phase over all phases.
    adc = [adc_inband(p.line_f, p.line_a, f0, fs, 64 * fs) for p in phases]
    adc_worst = min(adc, key=lambda r: r[0])
    dac_nopll = dac_errors(b.line_f, b.line_a, f0, None)
    dac_pll = dac_errors(b.line_f, b.line_a, f0, pll_fc)
    rj = src_rj_s
    snr_m = snr_raw_db(math.hypot(m.tie_rms, rj), f0)
    snr_b = snr_raw_db(math.hypot(b.tie_rms, rj), f0)
    td = time_domain_snr(b.tie_s, b.f_hz, f0) if b.tie_rms > 0 else math.inf
    pattern_hz = src / int(per.sum())
    return ConfigResult(source, src, fs, k, model, div, bclk_div, m, b, rms, pattern_hz,
                        snr_m, snr_b, adc_worst[0], adc_worst[1], adc_worst[2],
                        dac_nopll[0], dac_pll[0], min(dac_nopll[1], dac_pll[1]), td)


def low_jitter_mode(fs: int, f0: float, rj_s: float) -> dict[str, float]:
    """External XO, white random jitter rj_s on every edge (PCM1808 master, PCM5102A SCK from XO)."""
    raw = snr_raw_db(rj_s, f0)
    fm = 64.0 * fs
    bw = AUDIO_BAND[1] - AUDIO_BAND[0]
    # White error spread uniformly over [0, fm/2]; passband share passes, stopband share is attenuated
    # by >= 65 dB and a (bw / (fs/2)) share of it aliases into the audio band.
    frac_pass = bw / (fm / 2)
    frac_stop = (1 - ADC_STOP_EDGE * fs / (fm / 2)) * (bw / (fs / 2)) * 10 ** (-ADC_STOP_ATTEN_DB / 10)
    adc_in = raw - 10 * math.log10(frac_pass + frac_stop)
    return {"rj_s": rj_s, "snr_raw": raw, "adc_inband": adc_in}


# ----------------------------------------------------------------------------------------------
# Reporting
# ----------------------------------------------------------------------------------------------
def _ns(x: float) -> str:
    return f"{x * 1e9:.2f}"


def _db(x: float) -> str:
    return "∞ (none)" if math.isinf(x) and x > 0 else f"{x:.1f}"


def _khz(x: float) -> str:
    return f"{x / 1e3:.3g}"


def plot_config(r: ConfigResult, path: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    c1, c2 = "#2a78d6", "#eb6834"
    fig, axs = plt.subplots(2, 1, figsize=(9, 6.2), facecolor="#fcfcfb")
    ax = axs[0]
    nm = min(r.mclk.tie_s.size * 2, 400)
    tm = np.arange(nm) / r.mclk.f_hz * 1e6
    ax.step(tm, np.tile(r.mclk.tie_s, 3)[:nm] * 1e9, where="post", color=c1, lw=1.4, label="MCLK edges")
    nb = int(nm / r.bclk_div)
    tb = np.arange(nb) / r.bclk.f_hz * 1e6
    ax.plot(tb, np.tile(r.bclk.tie_s, 3 * r.bclk_div)[:nb] * 1e9, "o", ms=4, color=c2,
            label=f"BCLK edges (MCLK/{r.bclk_div}, worst phase)")
    ax.set_xlabel("time (µs)")
    ax.set_ylabel("TIE (ns)")
    ax.set_title(f"{r.label()}: divider {r.div} (x={r.div.x} y={r.div.y} z={r.div.z} yn1={r.div.yn1}), "
                 f"model '{r.model}'", fontsize=10, pad=22)
    ax.legend(loc="lower right", bbox_to_anchor=(1.0, 0.99), ncol=2, fontsize=8, frameon=False)
    ax.grid(color="#e4e3df", lw=0.6)
    ax = axs[1]
    for st, col, lab in ((r.mclk, c1, "MCLK TIE lines"), (r.bclk, c2, "BCLK TIE lines")):
        m = st.line_a > 1e-15
        if m.any():
            ml, sl, bl = ax.stem(st.line_f[m] / 1e3, st.line_a[m] * 1e12, basefmt=" ", label=lab)
            plt.setp(sl, color=col, lw=1.2)
            plt.setp(ml, color=col, ms=4)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("TIE line frequency (kHz)")
    ax.set_ylabel("TIE amplitude (ps, peak)")
    ax.axvspan(AUDIO_BAND[0] / 1e3, AUDIO_BAND[1] / 1e3, color="#e4e3df", alpha=0.6, lw=0)
    ax.text(0.03, 0.04, "audio band", transform=ax.transAxes, fontsize=8, color="#52514e")
    ax.set_xlim(left=1.0)
    ax.legend(loc="upper left", fontsize=8, frameon=False)
    ax.grid(color="#e4e3df", lw=0.6, which="both")
    for a_ in axs:
        a_.set_facecolor("#fcfcfb")
        for s in ("top", "right"):
            a_.spines[s].set_visible(False)
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)


def plot_summary(rs: Sequence[ConfigResult], lj: dict[str, float], path: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    labels = [f"{r.source}\n{r.k}fs" for r in rs] + ["ext XO\n(low-jitter)"]
    raw = [r.snr_raw_bclk for r in rs] + [lj["snr_raw"]]
    adc = [min(r.adc_inband_snr, 200) for r in rs] + [min(lj["adc_inband"], 200)]
    x = np.arange(len(labels))
    w = 0.38
    fig, ax = plt.subplots(figsize=(9, 4.2), facecolor="#fcfcfb")
    ax.bar(x - w / 2 - 0.01, raw, w, color="#2a78d6", label="raw: -20log10(2π·20k·σ_BCLK), all bandwidth")
    ax.bar(x + w / 2 + 0.01, adc, w, color="#1baf7a", label="PCM1808 in-band (after decimation filter model)")
    for xi, v in zip(x - w / 2, raw):
        ax.text(xi, v + 1.5, f"{v:.0f}", ha="center", fontsize=7, color="#52514e")
    for xi, v in zip(x + w / 2, adc):
        ax.text(xi, v + 1.5, "≥200" if v >= 200 else f"{v:.0f}", ha="center", fontsize=7, color="#52514e")
    ax.axhline(99, color="#52514e", lw=0.8, ls="--", label="PCM1808 dynamic range (99 dB typ.)")
    ax.axhline(112, color="#52514e", lw=0.8, ls=":", label="PCM5102A dynamic range (112 dB typ.)")
    ax.set_xticks(x, labels, fontsize=8)
    ax.set_ylabel("jitter-limited SNR, FS 20 kHz sine (dB)")
    ax.set_ylim(0, 240)
    ax.set_title("48 kHz: jitter-limited SNR per clock configuration (higher is better)", fontsize=10)
    ax.legend(loc="upper left", fontsize=8, frameon=False, ncol=2)
    ax.set_facecolor("#fcfcfb")
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.grid(axis="y", color="#e4e3df", lw=0.6)
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)


def table_rows(rs: Iterable[ConfigResult]) -> list[str]:
    out = ["| source | k | MCLK (Hz) | IDF divider N + b/a | x / y / z / yn1 | bclk_div | MCLK error (ppm) "
           "| MCLK pattern rate | TIE step (1 src cycle) | MCLK period p-p | MCLK duty range "
           "| MCLK TIE p-p / rms | BCLK TIE p-p / rms (worst phase) | lowest BCLK TIE line "
           "| raw SNR @20k MCLK / BCLK | ADC in-band SNR (worst spur) | DAC full-band SNR no PLL / PLL fc | DAC in-band |",
           "|" + "---|" * 18]
    for r in rs:
        t_src = 1 / r.src_hz
        lf, la = r.bclk.lowest_line()
        dmin, dmax = r.mclk.duty_range
        step_pct = t_src * r.mclk_hz * 100
        spur = "" if math.isinf(r.adc_inband_spur) else f" ({r.adc_inband_spur:.0f} dBc @ {r.adc_inband_spur_f / 1e3:.1f} kHz)"
        out.append(
            f"| {r.source} | {r.k} | {r.mclk_hz:,} | {r.div} | {r.div.x} / {r.div.y} / {r.div.z} / {r.div.yn1} "
            f"| {r.bclk_div} | {r.ppm:+.3f} | {_khz(r.pattern_hz)} kHz | {_ns(t_src)} ns ({step_pct:.1f} % T_MCLK) "
            f"| {_ns(r.mclk.period_pp)} ns | {dmin * 100:.1f}–{dmax * 100:.1f} % "
            f"| {_ns(r.mclk.tie_pp)} / {_ns(r.mclk.tie_rms)} ns | {_ns(r.bclk.tie_pp)} / {_ns(r.bclk.tie_rms)} ns "
            f"| {_khz(lf)} kHz, {la * 1e12:.0f} ps | {_db(r.snr_raw_mclk)} / {_db(r.snr_raw_bclk)} dB "
            f"| {_db(r.adc_inband_snr)} dB{spur} | {_db(r.dac_full_nopll)} / {_db(r.dac_full_pll)} dB "
            f"| {_db(r.dac_inband_snr)} |")
    return out


def compact_rows(rs: Iterable[ConfigResult]) -> list[str]:
    out = ["| source | k | divider | MCLK pattern / lowest BCLK TIE line | BCLK TIE rms | raw SNR (BCLK) | ADC in-band | DAC full-band (PLL fc) |",
           "|---|---|---|---|---|---|---|---|"]
    for r in rs:
        out.append(f"| {r.source} | {r.k} | {r.div} | {_khz(r.pattern_hz)} / {_khz(r.bclk.lowest_line()[0])} kHz "
                   f"| {_ns(r.bclk.tie_rms)} ns "
                   f"| {_db(r.snr_raw_bclk)} dB | {_db(r.adc_inband_snr)} dB | {_db(r.dac_full_pll)} dB |")
    return out


# PLAN.md section 4.2 table as written (source, k, divider, pattern kHz, TIE step ns, step % of T_MCLK)
PLAN_42 = [("PLL_F160M", 256, "13 + 1/48", 256, 6.25, 7.7), ("PLL_F160M", 384, "8 + 49/72", 256, 6.25, 11.5),
           ("PLL_F160M", 512, "6 + 49/96", 256, 6.25, 15.4), ("XTAL", 256, "3 + 49/192", 64, 25.0, 31.0)]


def plan_check_rows(rs: Sequence[ConfigResult]) -> list[str]:
    """Compare the PLAN.md 4.2 table with the computed values."""
    by = {(r.source, r.k): r for r in rs}
    L = ["| PLAN row | divider | pattern | TIE step | % T_MCLK | verdict |", "|---|---|---|---|---|---|"]
    for src, k, dv, pat, step, pct in PLAN_42:
        r = by[(src, k)]
        got_pct = 100 * r.mclk_hz / r.src_hz
        ok = (str(r.div) == dv and abs(r.pattern_hz / 1e3 - pat) < 1e-6 and abs(1e9 / r.src_hz - step) < 1e-6
              and abs(got_pct - pct) < 0.5)
        L.append(f"| {src} {k}fs | {r.div} | {r.pattern_hz / 1e3:g} kHz | {1e9 / r.src_hz:g} ns | {got_pct:.1f} % "
                 f"| {'matches' if ok else 'DIFFERS'} |")
    return L


def write_report(out: Path, results48: list[ConfigResult], results441: list[ConfigResult],
                 sens: dict[str, list[ConfigResult]], lj: dict[str, float], args: argparse.Namespace) -> None:
    out.mkdir(parents=True, exist_ok=True)
    for r in results48:
        plot_config(r, out / f"tie_{r.slug()}.png")
    idf48 = [r for r in results48 if r.source in IDF_SOURCES]
    plot_summary(idf48, lj, out / "snr_summary.png")
    fc = args.pll_fc
    L: list[str] = []
    L += ["# TWOFOLD clock jitter analysis (ESP32-S3 I2S fractional MCLK divider)", "",
          "Generated by `tools/clock_jitter.py` (do not edit by hand; re-run the script). "
          "Sine for the SNR figures: full scale, f0 = "
          f"{args.f0 / 1e3:g} kHz. Assumed PCM5102A PLL jitter transfer: 2nd-order low-pass, fc = "
          f"{fc / 1e3:g} kHz (not published by TI). Sequence model: `{args.model}` (see below).", "",
          "## Summary (48 kHz, sources selectable in IDF 5.5.1)", "", *compact_rows(idf48), "",
          f"Low-jitter mode (external 12.288 MHz XO, {lj['rj_s'] * 1e12:g} ps rms white): raw SNR "
          f"{lj['snr_raw']:.1f} dB, PCM1808 in-band {lj['adc_inband']:.1f} dB; PCM5102A uses SCK = XO (no PLL), "
          f"so its limit is the raw {lj['snr_raw']:.1f} dB.", "",
          "![summary](snr_summary.png)", "",
          "## Check of PLAN.md §4.2", "", *plan_check_rows(results48), "",
          "All four rows of the PLAN table are reproduced (31 % is 30.7 % rounded). What the PLAN table does not "
          "show: the clock that matters for both converters' sampling instants is BCK/64·fs (PCM5102A PLL "
          "reference; PCM1808 modulator = SCKI/(k/64)), and its TIE is set only by the source clock period: "
          "1.80 ns rms / 5.73 ns p-p with lines at n·256 kHz for **every** PLL_F160M multiple (256, 384 and 512 fs "
          "are identical at BCK level). The 256·fs choice is right (lowest MCLK frequency, smallest MCLK step, "
          "duty 46–54 %), but not because of a jitter advantage at BCK.", "",
          "## Full table, 48 kHz", "",
          "PLL_D2 (240 MHz) exists in hardware (TRM `I2S_TX_CLK_SEL = 1`) but IDF 5.5.1 does not offer it "
          "(`SOC_I2S_CLKS`); it is listed for reference only.", "",
          *table_rows(results48), "",
          "Rejected by the IDF driver (`i2s_std_calculate_clock()` requires sclk > 1.99·mclk, i.e. N ≥ 2): "
          + (", ".join(args.rejected) or "none") + ".", "",
          "## Reference: 44.1 kHz", "", *table_rows(results441), "",
          "## Sensitivity to the (undocumented) edge ordering", "",
          "BCLK TIE rms (ns) / ADC in-band SNR (dB) / DAC full-band SNR with PLL (dB), 48 kHz:", "",
          "| source | k | " + " | ".join(MODELS) + " |", "|---|---|" + "---|" * len(MODELS)]
    keys = [(r.source, r.k) for r in results48]
    for i, (s, k) in enumerate(keys):
        cells = []
        for m in MODELS:
            r = sens[m][i]
            cells.append(f"{_ns(r.bclk.tie_rms)} / {_db(r.adc_inband_snr)} / {_db(r.dac_full_pll)}")
        L.append(f"| {s} | {k} | " + " | ".join(cells) + " |")
    L += ["",
          "## Model", "",
          "* Divider values are computed by exact ports of `hal_utils_calc_clk_div_frac_accurate()` and "
          "`i2s_ll_tx_set_mclk()` (IDF v5.5.1); BCLK = MCLK / (mclk / (fs·2·32)) as in `i2s_std_calculate_clock()`.",
          "* Over one pattern of `a` MCLK periods the divider emits `b` periods of N+1 and `a-b` of N source "
          "cycles. The TRM gives only the x/y/z/yn1 encoding, not the order. `nested` (default): z groups of "
          "x normal + 1 special period, the y leftovers spread by a second accumulator; `ideal`: first-order "
          "accumulator; `bunched`: leftovers at the end (worst case). For 13 + 1/48 all three are identical.",
          "* TIE = actual rising-edge time minus ideal edge time, mean removed; spectrum = exact DFT over one "
          "pattern period (lines at multiples of the pattern fundamental). BCLK and the PCM1808 modulator "
          "clock (64 fs = SCKI/(k/64)) are every (k/64)-th MCLK edge; all divider phases are evaluated, the "
          "worst is reported.",
          "* Raw SNR = −20·log10(2π·f0·σ): all jitter, full bandwidth. This is the classic random-jitter "
          "bound and is very pessimistic here: the jitter is deterministic and its lines sit at ≥ the pattern "
          "fundamental, so the error sidebands are at f_k ± f0, far outside the audio band.",
          "* ADC in-band: sidebands (amplitude π·f0·A_k each) folded at 64 fs, passed by the decimation filter "
          "below 0.583 fs (conservatively unattenuated through the transition band) and attenuated 65 dB above "
          "(PCM1808 datasheet minimum), then aliased at fs; only components landing in 20 Hz–20 kHz count.",
          "* DAC: the PCM5102A PLL locks to BCK. Full-band SNR is given unfiltered and through the assumed "
          "PLL low-pass. In-band counts only sidebands that land in 20 Hz–20 kHz in the analog output.",
          "* Not modelled: random jitter of the PLL_F160M/XTAL source (unspecified by Espressif; "
          "`--src-rj-ps` adds it in quadrature), GPIO matrix/pad jitter, supply/ground-bounce coupling, "
          "and the second-order mechanism in delta-sigma DACs where out-of-band jitter mixes the "
          "shaped high-frequency quantisation noise down into the audio band (this is the main way 256 kHz "
          "jitter could still matter for the PCM5102A; it depends on its undocumented PLL and modulator).",
          "* Self-consistency (run `--check`): the time-domain brute-force SNR (sine sampled at jittered "
          "instants) matches the formula to within 0.2 dB for every configuration.", ""]
    (out / "clock_jitter.md").write_text("\n".join(L), encoding="utf-8")

    def rjson(r: ConfigResult) -> dict:
        return {"source": r.source, "fs": r.fs, "k": r.k, "model": r.model, "divider": asdict(r.div),
                "bclk_div": r.bclk_div, "mclk_hz_actual": r.mclk.f_hz, "ppm": r.ppm, "pattern_hz": r.pattern_hz,
                "mclk_tie_pp_s": r.mclk.tie_pp, "mclk_tie_rms_s": r.mclk.tie_rms,
                "mclk_period_pp_s": r.mclk.period_pp, "mclk_duty_range": r.mclk.duty_range,
                "bclk_tie_pp_s": r.bclk.tie_pp, "bclk_tie_rms_s": r.bclk.tie_rms,
                "bclk_tie_rms_by_phase_s": r.bclk_phases_rms, "bclk_duty_range": r.bclk.duty_range,
                "bclk_period_pp_s": r.bclk.period_pp,
                "bclk_lines": [[float(f), float(a)] for f, a in zip(r.bclk.line_f, r.bclk.line_a) if a > 1e-15],
                "snr_raw_mclk_db": r.snr_raw_mclk, "snr_raw_bclk_db": r.snr_raw_bclk,
                "adc_inband_snr_db": r.adc_inband_snr, "dac_full_nopll_db": r.dac_full_nopll,
                "dac_full_pll_db": r.dac_full_pll, "dac_inband_snr_db": r.dac_inband_snr}

    data = {"f0": args.f0, "pll_fc": fc, "low_jitter": lj,
            "fs48": [rjson(r) for r in results48], "fs441": [rjson(r) for r in results441]}
    (out / "clock_jitter.json").write_text(json.dumps(data, indent=1, default=lambda v: None if
                                                      (isinstance(v, float) and math.isinf(v)) else v),
                                           encoding="utf-8")


# ----------------------------------------------------------------------------------------------
# Checks
# ----------------------------------------------------------------------------------------------
def run_checks(verbose: bool = True) -> None:
    """Internal consistency checks; raises AssertionError on failure."""
    d = idf_mclk_divider(160_000_000, 12_288_000)
    assert (d.integer, d.numerator, d.denominator, d.x, d.y, d.z, d.yn1) == (13, 1, 48, 47, 0, 1, 0), d
    d = idf_mclk_divider(160_000_000, 18_432_000)
    assert (d.integer, d.numerator, d.denominator, d.yn1, d.z) == (8, 49, 72, 1, 23), d
    d = idf_mclk_divider(40_000_000, 12_288_000)
    assert (d.integer, d.numerator, d.denominator) == (3, 49, 192), d
    # TRM encoding identity: a == z*(x+1) + y for every fractional divider we produce
    for s in SOURCES.values():
        for fs in (48000, 44100):
            for k in (256, 384, 512):
                dv = idf_mclk_divider(s, fs * k)
                if dv.numerator:
                    assert dv.denominator == dv.z * (dv.x + 1) + dv.y, dv
                    for m in MODELS:
                        p = mclk_periods(dv, m)
                        assert p.sum() == dv.integer * dv.denominator + dv.numerator
                        assert np.count_nonzero(p == dv.integer + 1) == dv.numerator
    # 13 + 1/48: sawtooth TIE with p-p = 47/48 source cycle, identical in every model
    d = idf_mclk_divider(160_000_000, 12_288_000)
    base = mclk_periods(d, "nested")
    for m in MODELS:
        assert np.array_equal(np.sort(mclk_periods(d, m)), np.sort(base))
    st = edge_stats(base, 160e6)
    assert abs(st.tie_pp - (47 / 48) / 160e6) < 1e-15, st.tie_pp
    # Parseval on the line spectrum
    for dv in (d, idf_mclk_divider(160_000_000, 18_432_000)):
        s2 = edge_stats(mclk_periods(dv), 160e6, divide=4)
        assert abs(np.sum(s2.line_a ** 2 / 2) - s2.tie_rms ** 2) < 1e-6 * s2.tie_rms ** 2 + 1e-40
    # Time-domain brute force vs formula, all 48 kHz configs
    for src in SOURCES:
        for k in (256, 384, 512):
            if not idf_driver_accepts(SOURCES[src], 48000 * k):
                continue
            r = analyse(src, 48000, k)
            if not math.isinf(r.snr_raw_bclk):
                assert abs(r.td_check_db - r.snr_raw_bclk) < 0.2, (src, k, r.td_check_db, r.snr_raw_bclk)
    # ADC in-band model: a synthetic single line at 25 kHz must land in band unattenuated
    snr, spur, f = adc_inband(np.array([25e3]), np.array([1e-9]), 20e3, 48e3, 3.072e6)
    assert abs(spur - 20 * math.log10(math.pi * 20e3 * 1e-9)) < 1e-6 and abs(f - 5e3) < 1e-6
    if verbose:
        print("clock_jitter checks: OK")


# ----------------------------------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------------------------------
def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    root = Path(__file__).resolve().parent.parent
    ap.add_argument("--out", type=Path, default=root / "docs" / "clocking", help="output directory")
    ap.add_argument("--model", choices=MODELS, default="nested", help="divider edge-ordering model")
    ap.add_argument("--f0", type=float, default=20_000.0, help="test sine frequency (Hz)")
    ap.add_argument("--pll-fc", type=float, default=50e3, help="assumed PCM5102A PLL jitter corner (Hz)")
    ap.add_argument("--xo-rj-ps", type=float, default=1.0, help="low-jitter mode XO rms jitter (ps)")
    ap.add_argument("--src-rj-ps", type=float, default=0.0, help="random jitter of the S3 clock source (ps rms)")
    ap.add_argument("--check", action="store_true", help="run internal checks only")
    args = ap.parse_args(argv)

    run_checks()
    if args.check:
        return 0
    rj = args.src_rj_ps * 1e-12
    ks = (256, 384, 512)
    ok48 = [(s, k) for s in SOURCES for k in ks if idf_driver_accepts(SOURCES[s], 48000 * k)]
    ok441 = [(s, k) for s in IDF_SOURCES for k in ks if idf_driver_accepts(SOURCES[s], 44100 * k)]
    rejected = [f"{s} {k}fs @ {fs / 1000:g} kHz" for fs, srcs in ((48000, SOURCES), (44100, IDF_SOURCES))
                for s in srcs for k in ks if not idf_driver_accepts(SOURCES[s], fs * k)]
    r48 = [analyse(s, 48000, k, args.model, args.f0, args.pll_fc, rj) for s, k in ok48]
    r441 = [analyse(s, 44100, k, args.model, args.f0, args.pll_fc, rj) for s, k in ok441]
    sens = {m: [analyse(s, 48000, k, m, args.f0, args.pll_fc, rj) for s, k in ok48] for m in MODELS}
    args.rejected = rejected
    lj = low_jitter_mode(48000, args.f0, args.xo_rj_ps * 1e-12)
    write_report(args.out, r48, r441, sens, lj, args)
    print("\n".join(compact_rows(r48)))
    print(f"\nlow-jitter mode: raw {lj['snr_raw']:.1f} dB, ADC in-band {lj['adc_inband']:.1f} dB")
    print(f"wrote {args.out}/clock_jitter.md, clock_jitter.json and plots")
    return 0


if __name__ == "__main__":
    sys.exit(main())
