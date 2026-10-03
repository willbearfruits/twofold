#!/usr/bin/env python3
"""TWOFOLD loopback measurement suite (Linux, ALSA / PipeWire through PortAudio).

Connect the TWOFOLD line outputs to its line inputs (L->L, R->R) and run:

    python3 tools/loopback.py --list-devices            # see what PortAudio sees
    python3 tools/loopback.py --label studio            # full suite, saves <out>/studio.json + plots
    python3 tools/loopback.py --label wifi              # same, after enabling WiFi on the device
    python3 tools/loopback.py --report studio wifi      # markdown diff report of two saved runs
    python3 tools/loopback.py --compare                 # both runs interactively + report
    python3 tools/loopback.py --drift 120               # 2 h sine, detects clicks / sample slips
    python3 tools/loopback.py --selftest                # no device needed: synthetic verification

Measurements (48 kHz, 24-bit samples in int32 containers, duplex stream):
  (a) round-trip latency: MLS (order 15) cross-correlation, sub-sample peak interpolation; repeated
      ``--latency-runs`` times, each with a freshly opened stream, so the spread is the latency jitter;
  (b) frequency response: Farina exponential sine sweep 10 Hz-22 kHz, inverse-filter deconvolution,
      linear IR windowed and divided by the reference (the stimulus deconvolved with itself);
      reports the deviation over 20 Hz-20 kHz relative to 1 kHz;
  (c) THD+N at 1 kHz, -1 dBFS and -20 dBFS: 7-term Blackman-Harris window (-180 dB side lobes), the
      fundamental is removed in the FFT domain (+-10 bins), residual integrated over 20 Hz-20 kHz;
      THD from harmonics 2..N below 20 kHz; reported in dB and %;
  (d) dynamic range (AES17-like): THD+N of a -60 dBFS 1 kHz tone, A-weighted, + 60 dB;
      idle noise floor (unweighted and A-weighted, dBFS);
  (e) inter-channel crosstalk L->R and R->L at 1 kHz;
  (f) ``--drift MINUTES``: plays a continuous sine, fits it block by block and reports residual clicks and
      whole-sample slips (phase steps). With the device's async feedback working there must be none.

dBFS convention: a full-scale sine is 0 dBFS (AES17).

Install (sounddevice is only needed for real measurements, not for --selftest):
    python3 -m pip install --user sounddevice numpy matplotlib
    # sounddevice needs the PortAudio library: Arch: pacman -S portaudio; Debian/Ubuntu: apt install libportaudio2
    # (inside a venv: python3 -m venv .venv && .venv/bin/pip install sounddevice numpy matplotlib)

Prefer the raw ALSA ``hw:`` device (bit-exact, no resampling). If PipeWire holds the device, either stop
it for the session (``systemctl --user stop pipewire pipewire-pulse wireplumber``, restart afterwards) or use
the PipeWire/pulse device with the TWOFOLD sink/source set to 48 kHz and the OS volumes at 100 %.
On the TWOFOLD itself: DSP effects off (or bypassed), master volume at 0 dB, input gain 0 dB, direct monitor 0 %.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import math
import queue
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Protocol, Sequence

import numpy as np

FS = 48_000
CHANNELS = 2
INT32_SCALE = 2.0 ** 31
BAND = (20.0, 20_000.0)
DEFAULT_OUT = Path(__file__).resolve().parent / "loopback_results"


# ==============================================================================================
# Signal generation
# ==============================================================================================
def db_to_amp(db: float) -> float:
    """dBFS (sine peak convention) -> peak amplitude."""
    return 10.0 ** (db / 20.0)


def mls(order: int = 15) -> np.ndarray:
    """Maximum-length sequence (+-1) of length 2**order - 1 from a Fibonacci LFSR."""
    taps = {10: (10, 7), 11: (11, 9), 12: (12, 11, 10, 4), 13: (13, 12, 11, 8), 14: (14, 13, 12, 2),
            15: (15, 14), 16: (16, 15, 13, 4), 17: (17, 14)}[order]
    n = (1 << order) - 1
    state = 1
    out = np.empty(n, dtype=np.float64)
    for i in range(n):
        bit = 0
        for t in taps:  # polynomial x^order + ... ; right-shift Fibonacci form taps bit (order - t)
            bit ^= (state >> (order - t)) & 1
        out[i] = 1.0 if (state & 1) else -1.0
        state = (state >> 1) | (bit << (order - 1))
    return out


@dataclass(frozen=True)
class Sweep:
    """Farina exponential sine sweep and its inverse filter."""

    f1: float = 10.0
    f2: float = 22_000.0
    seconds: float = 6.0
    fs: int = FS
    level_db: float = -6.0

    @property
    def rate(self) -> float:
        """L = T / ln(f2/f1)."""
        return self.seconds / math.log(self.f2 / self.f1)

    def signal(self) -> np.ndarray:
        n = int(round(self.seconds * self.fs))
        t = np.arange(n) / self.fs
        L = self.rate
        x = np.sin(2 * np.pi * self.f1 * L * (np.exp(t / L) - 1.0))
        fi, fo = int(0.05 * self.fs), int(0.002 * self.fs)  # fades: 50 ms in (10 Hz), 2 ms out (22 kHz)
        x[:fi] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(fi) / fi)
        x[n - fo:] *= 0.5 + 0.5 * np.cos(np.pi * np.arange(fo) / fo)
        return x * db_to_amp(self.level_db)

    def inverse(self) -> np.ndarray:
        """Time-reversed sweep with a -6 dB/octave envelope (Farina 2000)."""
        x = self.signal()
        t = np.arange(x.size) / self.fs
        return x[::-1] * np.exp(-t / self.rate)


def tone(f0: float, seconds: float, level_db: float, fs: int = FS, fade_s: float = 0.01) -> np.ndarray:
    n = int(round(seconds * fs))
    x = db_to_amp(level_db) * np.sin(2 * np.pi * f0 * np.arange(n) / fs)
    nf = int(fade_s * fs)
    ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(nf) / nf)
    x[:nf] *= ramp
    x[n - nf:] *= ramp[::-1]
    return x


# ==============================================================================================
# Analysis primitives (pure numpy, all exercised by --selftest)
# ==============================================================================================
BH7 = (0.27105140069342, 0.43329793923448, 0.21812299954311, 0.06592544638803,
       0.01081174209837, 0.00077658482522, 0.00001388721735)


def blackman_harris7(n: int) -> np.ndarray:
    """7-term Blackman-Harris window (periodic), side lobes about -180 dB, main lobe +-7 bins."""
    k = np.arange(n) * 2 * np.pi / n
    w = np.zeros(n)
    for i, a in enumerate(BH7):
        w += ((-1) ** i) * a * np.cos(i * k)
    return w


def a_weighting_db(f: np.ndarray | float) -> np.ndarray:
    """IEC 61672-1 A-weighting in dB (0 dB at 1 kHz)."""
    f = np.asarray(f, dtype=float)
    f2 = f * f
    with np.errstate(divide="ignore"):
        ra = (12194.0 ** 2 * f2 ** 2) / ((f2 + 20.6 ** 2) * np.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2))
                                         * (f2 + 12194.0 ** 2))
        return 20 * np.log10(ra) + 2.0


@dataclass
class Spectrum:
    """One-sided windowed power spectrum normalised so that sum over bins = mean square of the signal."""

    f: np.ndarray
    p: np.ndarray      # mean-square contribution per bin (FS^2)
    df: float
    n: int

    @staticmethod
    def of(x: np.ndarray, fs: int = FS) -> "Spectrum":
        n = x.size
        w = blackman_harris7(n)
        X = np.fft.rfft((x - x.mean()) * w)
        p = 2.0 * np.abs(X) ** 2 / (n * np.sum(w ** 2))
        p[0] /= 2.0
        if n % 2 == 0:
            p[-1] /= 2.0
        return Spectrum(np.fft.rfftfreq(n, 1 / fs), p, fs / n, n)

    def band_mask(self, lo: float = BAND[0], hi: float = BAND[1]) -> np.ndarray:
        return (self.f >= lo) & (self.f <= hi)

    def lobe(self, f0: float, half: int = 10) -> slice:
        k = int(round(f0 / self.df))
        return slice(max(k - half, 0), min(k + half + 1, self.f.size))

    def peak_near(self, f0: float, search_hz: float = 20.0) -> float:
        m = (self.f > f0 - search_hz) & (self.f < f0 + search_hz)
        idx = np.nonzero(m)[0]
        k = idx[np.argmax(self.p[idx])]
        # parabolic interpolation on log power
        if 0 < k < self.p.size - 1:
            a, b, c = np.log(self.p[k - 1:k + 2] + 1e-300)
            d = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
            return float((k + d) * self.df)
        return float(k * self.df)

    def tone_power(self, f0: float, half: int = 10) -> float:
        return float(np.sum(self.p[self.lobe(f0, half)]))

    def level_dbfs_per_bin(self) -> np.ndarray:
        """Display level: 0 dBFS for a FS sine's peak bin region (power -> dB re FS sine power 1/2)."""
        return 10 * np.log10(2 * self.p + 1e-30)


def to_dbfs(mean_square: float) -> float:
    """Mean square -> dBFS (FS sine = 0 dBFS)."""
    return 10 * math.log10(2 * mean_square + 1e-300)


def thd_n(x: np.ndarray, f0: float, fs: int = FS, weighting: str | None = None, n_harm: int = 20,
          half: int = 10) -> dict[str, Any]:
    """THD+N (fundamental notched in the FFT domain) and THD by harmonics, 20 Hz-20 kHz.

    Returns ratios in dB and %, the fundamental level (dBFS), measured frequency and per-harmonic dBc.
    """
    sp = Spectrum.of(x, fs)
    fm = sp.peak_near(f0)
    p_fund = sp.tone_power(fm, half)
    band = sp.band_mask()
    resid = sp.p.copy()
    resid[sp.lobe(fm, half)] = 0.0
    if weighting == "A":
        resid = resid * 10 ** (a_weighting_db(np.maximum(sp.f, 1e-3)) / 10)
    p_res = float(np.sum(resid[band]))
    harm: list[dict[str, float]] = []
    p_h = 0.0
    for k in range(2, n_harm + 1):
        fk = k * fm
        if fk > BAND[1] or fk > fs / 2 - half * sp.df:
            break
        pk = sp.tone_power(fk, half)
        p_h += pk
        harm.append({"k": k, "f": fk, "dbc": 10 * math.log10(pk / p_fund + 1e-300)})
    ratio = math.sqrt(p_res / p_fund)
    thd = math.sqrt(p_h / p_fund) if harm else 0.0
    return {"f0_measured": fm, "fund_dbfs": to_dbfs(p_fund), "thdn_db": 20 * math.log10(ratio + 1e-300),
            "thdn_pct": 100 * ratio, "thd_db": 20 * math.log10(thd + 1e-300), "thd_pct": 100 * thd,
            "harmonics": harm, "weighting": weighting or "none", "residual_dbfs": to_dbfs(p_res)}


def noise_floor(x: np.ndarray, fs: int = FS) -> dict[str, float]:
    """Idle-channel noise, 20 Hz-20 kHz, unweighted and A-weighted, dBFS."""
    sp = Spectrum.of(x, fs)
    band = sp.band_mask()
    p = float(np.sum(sp.p[band]))
    pa = float(np.sum((sp.p * 10 ** (a_weighting_db(np.maximum(sp.f, 1e-3)) / 10))[band]))
    return {"unweighted_dbfs": to_dbfs(p), "a_weighted_dbfs": to_dbfs(pa)}


def tone_level_ratio_db(victim: np.ndarray, aggressor: np.ndarray, f0: float, fs: int = FS) -> float:
    """Level of the f0 component in ``victim`` relative to ``aggressor`` (dB) - crosstalk."""
    sv, sa = Spectrum.of(victim, fs), Spectrum.of(aggressor, fs)
    fm = sa.peak_near(f0)
    return 10 * math.log10(sv.tone_power(fm) / sa.tone_power(fm) + 1e-300)


def xcorr_lag(ref: np.ndarray, rec: np.ndarray) -> tuple[float, float, int]:
    """Lag (samples, sub-sample) of ``ref`` inside ``rec`` from the |cross-correlation| peak.

    The integer peak is refined by evaluating the band-limited (DFT-interpolated) correlation on a
    0.01-sample grid around it, which is exact for a band-limited delay (parabolic fits are biased).
    Returns (lag, peak-to-median ratio in dB, polarity +-1).
    """
    n = ref.size + rec.size
    nfft = 1 << (n - 1).bit_length()
    C = np.fft.rfft(rec, nfft) * np.conj(np.fft.rfft(ref, nfft))
    c = np.fft.irfft(C, nfft)[: rec.size]
    a = np.abs(c)
    k = int(np.argmax(a))
    tau = k + np.arange(-100, 101) * 0.01
    kk = np.arange(C.size)
    wgt = np.full(C.size, 2.0)
    wgt[0] = 1.0
    if nfft % 2 == 0:
        wgt[-1] = 1.0
    best, lag = -1.0, float(k)
    for chunk in np.array_split(tau, 8):  # bounded memory: (25 x nfft/2) complex per chunk
        e = np.exp(2j * np.pi * np.outer(chunk, kk) / nfft)
        v = np.abs(np.real(e @ (C * wgt))) / nfft
        i = int(np.argmax(v))
        if v[i] > best:
            best, lag = float(v[i]), float(chunk[i])
    snr = 20 * math.log10(a[k] / (np.median(a) + 1e-30) + 1e-30)
    return lag, snr, (1 if c[k] >= 0 else -1)


def fft_convolve(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    n = a.size + b.size - 1
    nfft = 1 << (n - 1).bit_length()
    return np.fft.irfft(np.fft.rfft(a, nfft) * np.fft.rfft(b, nfft), nfft)[:n]


def _ir_window(ir: np.ndarray, peak: int, pre: int, post: int) -> np.ndarray:
    seg = np.zeros(pre + post)
    lo, hi = peak - pre, peak + post
    src = ir[max(lo, 0):min(hi, ir.size)]
    seg[max(0, -lo):max(0, -lo) + src.size] = src
    w = np.ones(pre + post)
    tp, tq = pre // 2, post // 4
    w[:tp] = 0.5 - 0.5 * np.cos(np.pi * np.arange(tp) / tp)
    w[-tq:] = 0.5 + 0.5 * np.cos(np.pi * np.arange(tq) / tq)
    return seg * w


def sweep_response(sweep: Sweep, rec: np.ndarray, offset: int, pre_s: float = 0.05, post_s: float = 0.5
                   ) -> dict[str, Any]:
    """Farina deconvolution. ``offset`` = index of the sweep start in the stimulus.

    Returns frequency grid, magnitude (dB, absolute loop gain), deviation 20 Hz-20 kHz re 1 kHz, the
    linear-IR peak position (latency estimate) and the 2nd/3rd harmonic distortion IR levels.
    """
    x, inv = sweep.signal(), sweep.inverse()
    ir = fft_convolve(rec, inv)
    ref = fft_convolve(x, inv)
    pk_ref = int(np.argmax(np.abs(ref)))
    pk = int(np.argmax(np.abs(ir)))
    pre, post = int(pre_s * sweep.fs), int(post_s * sweep.fs)
    nfft = 1 << 18
    H = np.fft.rfft(_ir_window(ir, pk, pre, post), nfft)
    R = np.fft.rfft(_ir_window(ref, pk_ref, pre, post), nfft)
    f = np.fft.rfftfreq(nfft, 1 / sweep.fs)
    # Undo the window placement: both are windowed around their own peaks, so |H|/|R| is the response
    mag = 20 * np.log10(np.abs(H) / (np.abs(R) + 1e-30) + 1e-30)
    band = (f >= BAND[0]) & (f <= BAND[1])
    ref1k = float(np.interp(1000.0, f, mag))
    dev = mag[band] - ref1k
    # Harmonic IRs sit L*ln(k) before the linear one
    harm = {}
    for k in (2, 3):
        dk = int(round(sweep.rate * math.log(k) * sweep.fs))
        seg = np.abs(ir[max(pk - dk - pre // 2, 0): max(pk - dk + pre // 2, 1)])
        harm[f"h{k}_ir_db"] = 20 * math.log10((seg.max() if seg.size else 0) / (abs(ir[pk]) + 1e-30) + 1e-30)
    return {"f": f, "mag_db": mag, "gain_1k_db": ref1k, "dev_min_db": float(dev.min()),
            "dev_max_db": float(dev.max()), "dev_abs_db": float(np.max(np.abs(dev))),
            "ir_peak_latency": float(pk - pk_ref - offset), **harm}


def log_resample(f: np.ndarray, y: np.ndarray, n: int = 400, lo: float = 10.0, hi: float = 24_000.0,
                 mode: str = "interp") -> tuple[list[float], list[float]]:
    """Compact a curve for JSON / plots: log-spaced grid, interpolated or max-held per bin."""
    g = np.geomspace(lo, hi, n)
    if mode == "interp":
        return g.tolist(), np.interp(g, f, y).tolist()
    edges = np.sqrt(g[:-1] * g[1:])
    edges = np.concatenate([[lo], edges, [hi]])
    out = []
    for a, b in zip(edges[:-1], edges[1:]):
        m = (f >= a) & (f < b)
        out.append(float(y[m].max()) if m.any() else float(np.interp(0.5 * (a + b), f, y)))
    return g.tolist(), out


# ==============================================================================================
# Long-run click / slip detector
# ==============================================================================================
@dataclass
class ClickEvent:
    t: float
    ch: int
    kind: str          # "click" (residual spike) or "slip" (whole-sample phase step)
    value: float       # residual peak dBFS, or slipped samples (+ = samples lost)


class ClickDetector:
    """Block-wise sine fit on a continuous recording of a known-frequency tone.

    Because the DAC and the ADC share the I2S clock, the recorded tone has exactly the played frequency in
    sample units unless samples are dropped/duplicated somewhere (USB FIFO under/overrun, host xrun).
    Each block (default 50 ms) is least-squares fitted with cos/sin/DC at that frequency (phase referenced
    to the absolute sample index). A residual peak above ``max(abs_floor, k_sigma * typical residual rms)``
    is a click; a stable phase step of a whole number of samples is a slip.
    """

    def __init__(self, f0: float, fs: int = FS, block: int = 2400, settle_s: float = 1.0,
                 k_sigma: float = 12.0, abs_floor: float = 1e-4, channels: int = CHANNELS) -> None:
        self.f0, self.fs, self.block, self.ch = f0, fs, block, channels
        self.w = 2 * math.pi * f0 / fs
        self.settle = int(settle_s * fs)
        self.k_sigma, self.abs_floor = k_sigma, abs_floor
        self.buf = np.zeros((0, channels))
        self.n0 = 0                         # absolute index of buf[0]
        self.events: list[ClickEvent] = []
        self.phi_ref: list[float | None] = [None] * channels
        self.rms_hist: list[list[float]] = [[] for _ in range(channels)]
        self.amp_hist: list[list[float]] = [[] for _ in range(channels)]
        self.pending: list[tuple[int, float] | None] = [None] * channels  # (block start, dev) awaiting confirm
        self.last_flag: list[int] = [-10 ** 12] * channels
        self.blocks = 0
        self.max_dev = [0.0] * channels

    def feed(self, x: np.ndarray) -> None:
        x = np.asarray(x, dtype=float).reshape(-1, self.ch)
        self.buf = np.concatenate([self.buf, x])
        while self.buf.shape[0] >= self.block:
            blk = self.buf[: self.block]
            self._process(blk, self.n0)
            self.buf = self.buf[self.block:]
            self.n0 += self.block

    def _process(self, blk: np.ndarray, n0: int) -> None:
        if n0 < self.settle:
            return
        self.blocks += 1
        n = n0 + np.arange(blk.shape[0])
        ph = np.mod(self.w * n, 2 * np.pi)
        A = np.stack([np.cos(ph), np.sin(ph), np.ones_like(ph)], axis=1)
        coef, *_ = np.linalg.lstsq(A, blk, rcond=None)
        resid = blk - A @ coef
        for c in range(self.ch):
            a, b = coef[0, c], coef[1, c]
            amp = math.hypot(a, b)
            r = resid[:, c]
            rpk = float(np.max(np.abs(r)))
            rrms = float(np.sqrt(np.mean(r ** 2)))
            hist = self.rms_hist[c]
            typical = float(np.median(hist[-200:])) if len(hist) >= 10 else rrms
            thr = max(self.abs_floor, self.k_sigma * typical)
            t = n0 / self.fs
            flagged = rpk > thr
            if flagged:
                if n0 - self.last_flag[c] > self.block:  # merge consecutive flagged blocks
                    self.events.append(ClickEvent(t, c, "click", 20 * math.log10(rpk + 1e-30)))
                self.last_flag[c] = n0
            else:
                hist.append(rrms)
                if len(hist) > 400:
                    del hist[:200]
            if amp < 1e-6:
                continue
            self.amp_hist[c].append(amp)
            phi = math.atan2(-b, a)  # x ~ amp*cos(w n + phi)
            if self.phi_ref[c] is None:
                self.phi_ref[c] = phi
                continue
            dev = (phi - self.phi_ref[c] + math.pi) % (2 * math.pi) - math.pi
            dev_s = dev / self.w
            if flagged:
                continue
            k = round(dev_s)
            if k != 0 and abs(dev_s - k) < 0.15:
                pend = self.pending[c]
                if pend is not None and round(pend[1]) == k:
                    t_ev = pend[0] / self.fs
                    self.events.append(ClickEvent(t_ev, c, "slip", float(k)))
                    self.phi_ref[c] = phi
                    self.pending[c] = None
                else:
                    self.pending[c] = (n0, dev_s)
            else:
                self.pending[c] = None
                self.max_dev[c] = max(self.max_dev[c], abs(dev_s))
                # follow very slow phase wander (none expected with a shared clock)
                self.phi_ref[c] = self.phi_ref[c] + 0.05 * dev

    def summary(self) -> dict[str, Any]:
        amps = [np.array(h) for h in self.amp_hist]
        stab = [float(20 * np.log10(a.max() / a.min())) if a.size > 1 else 0.0 for a in amps]
        floor = [to_dbfs(float(np.median(h)) ** 2) if h else float("nan") for h in self.rms_hist]
        clicks = [e for e in self.events if e.kind == "click"]
        slips = [e for e in self.events if e.kind == "slip"]
        return {"seconds": self.n0 / self.fs, "blocks": self.blocks, "clicks": len(clicks), "slips": len(slips),
                "events": [e.__dict__ for e in self.events], "amplitude_pp_db": stab,
                "residual_floor_dbfs": floor, "max_phase_wander_samples": self.max_dev}


# ==============================================================================================
# Audio back-ends
# ==============================================================================================
class Backend(Protocol):
    fs: int
    name: str

    def playrec(self, out: np.ndarray) -> np.ndarray:
        """Play float (n, 2) in [-1, 1], return the simultaneously recorded float (n, 2)."""

    def duplex_stream(self, seconds: float, gen: Callable[[int, int], np.ndarray],
                      sink: Callable[[np.ndarray], None]) -> dict[str, Any]:
        """Continuous duplex stream: gen(n0, frames) -> (frames, 2); sink(recorded block)."""


def float_to_i32(x: np.ndarray) -> np.ndarray:
    return np.clip(np.round(x * INT32_SCALE), -INT32_SCALE, INT32_SCALE - 1).astype(np.int32)


def i32_to_float(x: np.ndarray) -> np.ndarray:
    return x.astype(np.float64) / INT32_SCALE


def import_sounddevice() -> Any:
    try:
        import sounddevice as sd  # type: ignore[import-not-found]
    except ImportError:
        sys.exit("error: the python module 'sounddevice' is not installed.\n"
                 "  python3 -m pip install --user sounddevice   (needs PortAudio: pacman -S portaudio /"
                 " apt install libportaudio2)\n"
                 "  or: python3 -m venv .venv && .venv/bin/pip install sounddevice numpy matplotlib\n"
                 "--selftest works without it.")
    except OSError as e:  # PortAudio shared library missing
        sys.exit(f"error: sounddevice could not load PortAudio ({e}).\n"
                 "  Arch: sudo pacman -S portaudio    Debian/Ubuntu: sudo apt install libportaudio2")
    return sd


def find_device(sd: Any, override: str | None, pattern: str = "twofold") -> int:
    """Pick the duplex TWOFOLD device; prefer raw ALSA hw: entries. ``override`` = index or substring."""
    devs = list(sd.query_devices())
    if override is not None:
        if override.isdigit():
            return int(override)
        pattern = override.lower()
    cands = [(i, d) for i, d in enumerate(devs) if pattern in d["name"].lower()
             and d["max_input_channels"] >= CHANNELS and d["max_output_channels"] >= CHANNELS]
    if not cands:
        listing = "\n".join(f"  [{i}] {d['name']}  (in {d['max_input_channels']}, out {d['max_output_channels']})"
                            for i, d in enumerate(devs))
        sys.exit(f"error: no duplex device matching '{pattern}' found. Devices:\n{listing}\n"
                 "Use --device <index|name substring>. If the device is busy, stop PipeWire for the session.")
    cands.sort(key=lambda c: (0 if "hw:" in c[1]["name"] else 1, c[0]))
    return cands[0][0]


class SoundDeviceBackend:
    """Real device through python-sounddevice (PortAudio), int32 samples."""

    def __init__(self, device: str | None, fs: int = FS, blocksize: int = 0) -> None:
        self.sd = import_sounddevice()
        self.fs = fs
        self.dev = find_device(self.sd, device)
        self.name = self.sd.query_devices(self.dev)["name"]
        self.blocksize = blocksize
        try:
            self.sd.check_output_settings(device=self.dev, channels=CHANNELS, dtype="int32", samplerate=fs)
            self.sd.check_input_settings(device=self.dev, channels=CHANNELS, dtype="int32", samplerate=fs)
        except Exception as e:  # noqa: BLE001 - PortAudio raises a plain PortAudioError
            sys.exit(f"error: device '{self.name}' rejects {fs} Hz / int32 / {CHANNELS} ch: {e}")

    def playrec(self, out: np.ndarray) -> np.ndarray:
        rec = self.sd.playrec(float_to_i32(out), samplerate=self.fs, channels=CHANNELS, dtype="int32",
                              device=(self.dev, self.dev), blocking=True)
        return i32_to_float(np.asarray(rec))

    def duplex_stream(self, seconds: float, gen: Callable[[int, int], np.ndarray],
                      sink: Callable[[np.ndarray], None]) -> dict[str, Any]:
        q: queue.Queue[np.ndarray] = queue.Queue()
        xruns: list[dict[str, Any]] = []
        pos = {"n": 0}

        def cb(indata: np.ndarray, outdata: np.ndarray, frames: int, _t: Any, status: Any) -> None:
            if status:
                xruns.append({"t": pos["n"] / self.fs, "status": str(status)})
            outdata[:] = float_to_i32(gen(pos["n"], frames))
            q.put(indata.copy())
            pos["n"] += frames

        total = int(seconds * self.fs)
        got, last_print = 0, time.monotonic()
        with self.sd.Stream(device=(self.dev, self.dev), samplerate=self.fs, channels=CHANNELS, dtype="int32",
                            blocksize=self.blocksize, callback=cb):
            while got < total:
                blk = q.get(timeout=5.0)
                sink(i32_to_float(blk))
                got += blk.shape[0]
                if time.monotonic() - last_print > 10:
                    last_print = time.monotonic()
                    print(f"  drift: {got / self.fs / 60:6.1f} / {seconds / 60:.1f} min, xruns {len(xruns)}",
                          flush=True)
        return {"xruns": xruns}


@dataclass
class SyntheticDevice:
    """Known-truth device model for --selftest.

    out -> 3-tap FIR [a, b, a] (known response, 1 sample group delay) -> delay ``latency`` (fractional)
        -> y + h2 y^2 + h3 y^3 -> + white noise (sigma) -> crosstalk (each channel gets ``xtalk`` x the other)
        -> int32 quantisation.
    """

    latency: float = 517.3
    fir: tuple[float, float] = (0.05, 0.9)   # (a, b)
    h2: float = 1e-4
    h3: float = 3e-4
    sigma: float = 7.75e-6
    xtalk: float = 10 ** (-80 / 20)
    seed: int = 1
    fs: int = FS
    name: str = "synthetic"
    latency_jitter: float = 0.0
    slips: list[tuple[int, int]] = field(default_factory=list)   # (sample index, +1 drop / -1 duplicate)
    spikes: list[tuple[int, float]] = field(default_factory=list)  # (sample index, amplitude)

    def __post_init__(self) -> None:
        self.rng = np.random.default_rng(self.seed)
        self.calls = 0

    def fir_mag(self, f: np.ndarray | float) -> np.ndarray:
        a, b = self.fir
        return np.abs(b + 2 * a * np.cos(2 * np.pi * np.asarray(f) / self.fs))

    def _channel(self, out: np.ndarray, delay: float) -> np.ndarray:
        a, b = self.fir
        n = out.shape[0]
        y = np.zeros_like(out)
        y[1:] += b * out[:-1]
        y += a * out
        y[2:] += a * out[:-2]
        # fractional delay by FFT phase shift on a zero-padded buffer (no wrap: pad > delay)
        pad = int(delay) + 64
        nfft = 1 << (n + pad - 1).bit_length()
        Y = np.fft.rfft(y, nfft, axis=0)
        k = np.fft.rfftfreq(nfft)
        y = np.fft.irfft(Y * np.exp(-2j * np.pi * k * delay)[:, None], nfft, axis=0)[:n]
        y = y + self.h2 * y ** 2 + self.h3 * y ** 3
        y = y + self.rng.normal(0.0, self.sigma, y.shape)
        y = y + self.xtalk * y[:, ::-1]
        return y

    def playrec(self, out: np.ndarray) -> np.ndarray:
        self.calls += 1
        d = self.latency + (self.latency_jitter * ((self.calls % 3) - 1))
        q = float_to_i32(self._channel(i32_to_float(float_to_i32(out)), d))
        return i32_to_float(q)

    def duplex_stream(self, seconds: float, gen: Callable[[int, int], np.ndarray],
                      sink: Callable[[np.ndarray], None]) -> dict[str, Any]:
        n = int(seconds * self.fs)
        y = self._channel(gen(0, n), self.latency)
        for idx, kind in sorted(self.slips, reverse=True):
            if kind > 0:
                y = np.delete(y, idx, axis=0)
            else:
                y = np.insert(y, idx, y[idx], axis=0)
        for idx, amp in self.spikes:
            y[idx] += amp
        y = i32_to_float(float_to_i32(y))
        pos = 0
        while pos < y.shape[0]:
            step = int(self.rng.integers(64, 3000))
            sink(y[pos:pos + step])
            pos += step
        return {"xruns": []}


# ==============================================================================================
# Measurement suite
# ==============================================================================================
PRE_S, POST_S = 0.2, 0.8


def _frame(x: np.ndarray, chans: Sequence[int]) -> tuple[np.ndarray, int]:
    """Put a mono stimulus on the given channels with pre/post silence. Returns (stimulus, start index)."""
    pre, post = int(PRE_S * FS), int(POST_S * FS)
    out = np.zeros((pre + x.size + post, CHANNELS))
    for c in chans:
        out[pre:pre + x.size, c] = x
    return out, pre


def measure_latency(be: Backend, runs: int = 5) -> dict[str, Any]:
    seq = 0.5 * mls(15)
    lags: list[list[float]] = [[] for _ in range(CHANNELS)]
    q: list[float] = []
    pol: list[int] = []
    for _ in range(runs):
        stim, off = _frame(seq, range(CHANNELS))
        rec = be.playrec(stim)
        for c in range(CHANNELS):
            lag, snr, p = xcorr_lag(seq, rec[:, c])
            lags[c].append(lag - off)
            q.append(snr)
            pol.append(p)
    allv = np.array(lags).ravel()
    return {"samples": lags, "mean_samples": float(allv.mean()), "ms": float(allv.mean() / be.fs * 1e3),
            "min_samples": float(allv.min()), "max_samples": float(allv.max()),
            "std_samples": float(allv.std()), "lr_skew_samples": float(np.mean(lags[0]) - np.mean(lags[1])),
            "xcorr_peak_db": float(min(q)), "polarity": int(np.sign(np.sum(pol)))}


def measure_fr(be: Backend) -> dict[str, Any]:
    sw = Sweep()
    stim, off = _frame(sw.signal(), range(CHANNELS))
    rec = be.playrec(stim)
    res = []
    for c in range(CHANNELS):
        r = sweep_response(sw, rec[:, c], off)
        gf, gm = log_resample(r.pop("f"), r.pop("mag_db"), mode="interp")
        r["curve_f"], r["curve_db"] = gf, gm
        res.append(r)
    return {"channels": res, "dev_abs_db": max(r["dev_abs_db"] for r in res)}


def _analysis_slice(rec: np.ndarray, latency: int, n: int = 65536) -> slice:
    start = int(PRE_S * FS) + max(latency, 0) + FS  # 1 s after the tone reaches the input
    return slice(start, start + n)


def measure_tones(be: Backend, latency: int, f0: float = 1000.0) -> dict[str, Any]:
    """THD+N (-1, -20 dBFS), DR (-60 dBFS, A-weighted), idle noise and crosstalk."""
    out: dict[str, Any] = {}
    secs = 3.0
    sl = _analysis_slice(np.empty(0), latency)
    spectra: dict[str, Any] = {}
    # -1 dBFS, one channel at a time (gives crosstalk both ways)
    for c, nm in ((0, "L"), (1, "R")):
        stim, _ = _frame(tone(f0, secs, -1.0), [c])
        rec = be.playrec(stim)[sl]
        out[f"thdn_m1_{nm}"] = thd_n(rec[:, c], f0)
        out[f"xtalk_{nm}to{'R' if c == 0 else 'L'}_db"] = tone_level_ratio_db(rec[:, 1 - c], rec[:, c], f0)
        sp = Spectrum.of(rec[:, c])
        spectra[f"m1_{nm}"] = log_resample(sp.f, sp.level_dbfs_per_bin(), n=600, mode="max")
    for lvl, key in ((-20.0, "m20"), (-60.0, "m60")):
        stim, _ = _frame(tone(f0, secs, lvl), range(CHANNELS))
        rec = be.playrec(stim)[sl]
        for c, nm in ((0, "L"), (1, "R")):
            if key == "m20":
                out[f"thdn_m20_{nm}"] = thd_n(rec[:, c], f0)
            else:
                r = thd_n(rec[:, c], f0, weighting="A")
                r["dr_db"] = 60.0 - r["thdn_db"]
                out[f"dr_{nm}"] = r
    stim = np.zeros((int((PRE_S + secs + POST_S) * FS), CHANNELS))
    rec = be.playrec(stim)[sl]
    for c, nm in ((0, "L"), (1, "R")):
        out[f"noise_{nm}"] = noise_floor(rec[:, c])
        sp = Spectrum.of(rec[:, c])
        spectra[f"idle_{nm}"] = log_resample(sp.f, sp.level_dbfs_per_bin(), n=600, mode="max")
    out["spectra"] = spectra
    return out


def measure_drift(be: Backend, minutes: float, f0: float = 1000.0, level_db: float = -6.0) -> dict[str, Any]:
    det = ClickDetector(f0, be.fs)
    amp = db_to_amp(level_db)
    w = 2 * math.pi * f0 / be.fs

    def gen(n0: int, frames: int) -> np.ndarray:
        n = n0 + np.arange(frames, dtype=np.int64)
        s = amp * np.sin(np.mod(w * n, 2 * np.pi))
        return np.repeat(s[:, None], CHANNELS, axis=1)

    info = be.duplex_stream(minutes * 60.0, gen, det.feed)
    res = det.summary()
    res["xruns"] = info.get("xruns", [])
    res["pass"] = res["clicks"] == 0 and res["slips"] == 0 and not res["xruns"]
    return res


def run_suite(be: Backend, label: str, latency_runs: int = 5, drift_minutes: float = 0.0) -> dict[str, Any]:
    t0 = time.time()
    print(f"[{label}] device: {be.name}")
    print(f"[{label}] latency ({latency_runs} runs)...", flush=True)
    lat = measure_latency(be, latency_runs)
    print(f"[{label}] frequency response (sweep)...", flush=True)
    fr = measure_fr(be)
    print(f"[{label}] THD+N / DR / noise / crosstalk...", flush=True)
    tones = measure_tones(be, int(round(lat["mean_samples"])))
    res: dict[str, Any] = {"label": label, "device": be.name, "fs": be.fs,
                           "timestamp": _dt.datetime.now().isoformat(timespec="seconds"),
                           "latency": lat, "fr": fr, "tones": tones}
    if drift_minutes > 0:
        print(f"[{label}] drift / click run ({drift_minutes} min)...", flush=True)
        res["drift"] = measure_drift(be, drift_minutes)
    res["elapsed_s"] = time.time() - t0
    return res


# ==============================================================================================
# Output: JSON, plots, markdown
# ==============================================================================================
METRICS: list[tuple[str, Callable[[dict[str, Any]], float], str, int]] = [
    # (name, getter, unit, better: +1 higher is better, -1 lower is better, 0 neutral)
    ("round-trip latency (mean)", lambda r: r["latency"]["ms"], "ms", -1),
    ("round-trip latency (mean)", lambda r: r["latency"]["mean_samples"], "samples", -1),
    ("latency spread (max-min)", lambda r: r["latency"]["max_samples"] - r["latency"]["min_samples"], "samples", -1),
    ("loop gain @1 kHz L", lambda r: r["fr"]["channels"][0]["gain_1k_db"], "dB", 0),
    ("FR deviation 20-20k (max abs)", lambda r: r["fr"]["dev_abs_db"], "dB", -1),
    ("THD+N -1 dBFS L", lambda r: r["tones"]["thdn_m1_L"]["thdn_db"], "dB", -1),
    ("THD+N -1 dBFS R", lambda r: r["tones"]["thdn_m1_R"]["thdn_db"], "dB", -1),
    ("THD -1 dBFS L", lambda r: r["tones"]["thdn_m1_L"]["thd_db"], "dB", -1),
    ("THD -1 dBFS R", lambda r: r["tones"]["thdn_m1_R"]["thd_db"], "dB", -1),
    ("THD+N -20 dBFS L", lambda r: r["tones"]["thdn_m20_L"]["thdn_db"], "dB", -1),
    ("THD+N -20 dBFS R", lambda r: r["tones"]["thdn_m20_R"]["thdn_db"], "dB", -1),
    ("dynamic range L (A, AES17-like)", lambda r: r["tones"]["dr_L"]["dr_db"], "dB", +1),
    ("dynamic range R (A, AES17-like)", lambda r: r["tones"]["dr_R"]["dr_db"], "dB", +1),
    ("idle noise L (A)", lambda r: r["tones"]["noise_L"]["a_weighted_dbfs"], "dBFS", -1),
    ("idle noise R (A)", lambda r: r["tones"]["noise_R"]["a_weighted_dbfs"], "dBFS", -1),
    ("idle noise L (unweighted 20-20k)", lambda r: r["tones"]["noise_L"]["unweighted_dbfs"], "dBFS", -1),
    ("idle noise R (unweighted 20-20k)", lambda r: r["tones"]["noise_R"]["unweighted_dbfs"], "dBFS", -1),
    ("crosstalk L->R", lambda r: r["tones"]["xtalk_LtoR_db"], "dB", -1),
    ("crosstalk R->L", lambda r: r["tones"]["xtalk_RtoL_db"], "dB", -1),
]


def _jsonable(o: Any) -> Any:
    if isinstance(o, dict):
        return {k: _jsonable(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [_jsonable(v) for v in o]
    if isinstance(o, (np.floating, float)):
        v = float(o)
        return None if not math.isfinite(v) else v
    if isinstance(o, np.integer):
        return int(o)
    if isinstance(o, np.ndarray):
        return _jsonable(o.tolist())
    return o


def save_result(res: dict[str, Any], out: Path) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    p = out / f"{res['label']}.json"
    p.write_text(json.dumps(_jsonable(res), indent=1), encoding="utf-8")
    return p


def _style(ax: Any) -> None:
    ax.set_facecolor("#fcfcfb")
    ax.grid(color="#e4e3df", lw=0.6, which="both")
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)


COLORS = ("#2a78d6", "#eb6834", "#1baf7a")


def plot_results(results: Sequence[dict[str, Any]], path: Path) -> None:
    """2x2 overview; several results are overlaid (one colour per run, solid L / dashed R)."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axs = plt.subplots(2, 2, figsize=(11, 7.5), facecolor="#fcfcfb")
    lo_db, hi_db = 0.0, 0.0
    for i, r in enumerate(results):
        col = COLORS[i % len(COLORS)]
        lab = r["label"]
        for c, ls in ((0, "-"), (1, "--")):
            ch = r["fr"]["channels"][c]
            cf, cd = np.array(ch["curve_f"]), np.array(ch["curve_db"]) - ch["gain_1k_db"]
            axs[0, 0].semilogx(cf, cd, ls, color=col, lw=1.4, label=f"{lab} {'LR'[c]}")
            inb = cd[(cf >= BAND[0]) & (cf <= BAND[1])]
            lo_db, hi_db = min(lo_db, float(inb.min())), max(hi_db, float(inb.max()))
        for key, ls, nm in (("m1_L", "-", "L"), ("m1_R", "--", "R")):
            f, y = r["tones"]["spectra"][key]
            axs[0, 1].semilogx(f, y, ls, color=col, lw=1.0, label=f"{lab} {nm}")
        for key, ls, nm in (("idle_L", "-", "L"), ("idle_R", "--", "R")):
            f, y = r["tones"]["spectra"][key]
            axs[1, 0].semilogx(f, y, ls, color=col, lw=1.0, label=f"{lab} {nm}")
        for c, mk in ((0, "o"), (1, "s")):
            s = r["latency"]["samples"][c]
            axs[1, 1].plot(np.arange(1, len(s) + 1), s, mk + ("-" if c == 0 else "--"), color=col, ms=6, lw=1,
                           label=f"{lab} {'LR'[c]}")
    axs[0, 0].set(title="Frequency response (re 1 kHz)", xlabel="Hz", ylabel="dB", xlim=(10, 24000),
                  ylim=(lo_db - 1.0, hi_db + 1.0))
    axs[0, 0].axvspan(20, 20000, color="#e4e3df", alpha=0.3, lw=0)
    axs[0, 1].set(title="Spectrum, 1 kHz at −1 dBFS (BH7, 65536 pt, max-held)", xlabel="Hz",
                  ylabel="dBFS per bin", xlim=(10, 24000), ylim=(-180, 5))
    axs[1, 0].set(title="Idle channel spectrum", xlabel="Hz", ylabel="dBFS per bin", xlim=(10, 24000))
    axs[1, 1].set(title="Round-trip latency per run", xlabel="run", ylabel="samples")
    axs[1, 1].xaxis.get_major_locator().set_params(integer=True)
    for ax in axs.ravel():
        _style(ax)
        ax.legend(fontsize=7, frameon=False)
    fig.tight_layout()
    fig.savefig(path, dpi=100)
    plt.close(fig)


def _fmt(v: float | None) -> str:
    return "n/a" if v is None or (isinstance(v, float) and not math.isfinite(v)) else f"{v:.2f}"


def summary_md(r: dict[str, Any]) -> list[str]:
    L = [f"| metric | {r['label']} |", "|---|---|"]
    for name, get, unit, _ in METRICS:
        try:
            L.append(f"| {name} | {_fmt(get(r))} {unit} |")
        except (KeyError, TypeError, IndexError):
            L.append(f"| {name} | n/a |")
    for nm in ("L", "R"):
        t = r["tones"][f"thdn_m1_{nm}"]
        L.append(f"| THD+N -1 dBFS {nm} (%) | {t['thdn_pct']:.5f} % |")
        hs = ", ".join(f"H{h['k']} {h['dbc']:.1f}" for h in t["harmonics"][:5])
        L.append(f"| harmonics {nm} (dBc) | {hs} |")
    if "drift" in r:
        d = r["drift"]
        L.append(f"| drift run | {d['seconds'] / 60:.1f} min, clicks {d['clicks']}, slips {d['slips']}, "
                 f"xruns {len(d['xruns'])}: {'PASS' if d['pass'] else 'FAIL'} |")
    return L


def write_single_report(r: dict[str, Any], out: Path) -> Path:
    plot_results([r], out / f"{r['label']}.png")
    md = [f"# TWOFOLD loopback: {r['label']}", "", f"Device `{r['device']}`, {r['timestamp']}.", "",
          *summary_md(r), "", f"![{r['label']}]({r['label']}.png)", ""]
    p = out / f"{r['label']}.md"
    p.write_text("\n".join(md), encoding="utf-8")
    return p


def diff_report(a: dict[str, Any], b: dict[str, Any], out: Path, tol_db: float = 1.0) -> Path:
    """Markdown diff of two runs (e.g. Studio Mode vs WiFi on), with overlay plots."""
    out.mkdir(parents=True, exist_ok=True)
    stem = f"compare_{a['label']}_vs_{b['label']}"
    plot_results([a, b], out / f"{stem}.png")
    L = [f"# TWOFOLD loopback comparison: {a['label']} vs {b['label']}", "",
         f"* {a['label']}: `{a['device']}`, {a['timestamp']}",
         f"* {b['label']}: `{b['device']}`, {b['timestamp']}", "",
         f"Δ = {b['label']} − {a['label']}. Flag ⚠ when the change is worse by more than {tol_db} dB "
         "(or by more than 2 samples of latency spread).", "",
         f"| metric | {a['label']} | {b['label']} | Δ | |", "|---|---|---|---|---|"]
    worse = 0
    for name, get, unit, better in METRICS:
        try:
            va, vb = float(get(a)), float(get(b))
        except (KeyError, TypeError, IndexError):
            continue
        d = vb - va
        lim = 2.0 if unit == "samples" else tol_db
        flag = "⚠" if better != 0 and -better * d > lim else ""
        worse += bool(flag)
        L.append(f"| {name} | {va:.2f} {unit} | {vb:.2f} {unit} | {d:+.2f} | {flag} |")
    for r in (a, b):
        if "drift" in r:
            d = r["drift"]
            L.append(f"| drift ({r['label']}) | {d['seconds'] / 60:.1f} min | clicks {d['clicks']} slips "
                     f"{d['slips']} xruns {len(d['xruns'])} | | {'' if d['pass'] else '⚠'} |")
            worse += not d["pass"]
    L += ["", f"**{worse} metric(s) flagged.**" if worse else "**No metric degraded beyond tolerance.**", "",
          f"![comparison]({stem}.png)", ""]
    p = out / f"{stem}.md"
    p.write_text("\n".join(L), encoding="utf-8")
    return p


def load_result(arg: str, out: Path) -> dict[str, Any]:
    p = Path(arg)
    if not p.suffix:
        p = out / f"{arg}.json"
    return json.loads(p.read_text(encoding="utf-8"))


# ==============================================================================================
# Self-test
# ==============================================================================================
def selftest(verbose: bool = True) -> int:
    """Run every analysis routine on synthetic signals with known truth; assert recovery."""
    fails: list[str] = []

    def check(name: str, got: float, want: float, tol: float) -> None:
        ok = abs(got - want) <= tol
        if verbose:
            print(f"  {'ok ' if ok else 'FAIL'} {name:46s} got {got:12.4f}  want {want:12.4f}  (±{tol})")
        if not ok:
            fails.append(name)

    print("selftest: primitives")
    # A-weighting against IEC 61672 table values
    # (table frequencies are the exact base-10 third-octave values, e.g. "20 Hz" = 10^1.3 = 19.95 Hz)
    for f, want in ((10 ** 1.3, -50.5), (100, -19.1), (1000, 0.0), (10000, -2.5), (10 ** 4.3, -9.3)):
        check(f"A-weighting {f:.5g} Hz", float(a_weighting_db(f)), want, 0.1)
    # MLS is maximal length and balanced
    s = mls(15)
    check("MLS balance (sum = 1)", float(s.sum()), 1.0, 0.0)
    ac = np.fft.irfft(np.abs(np.fft.rfft(s)) ** 2, s.size)
    check("MLS periodic autocorrelation off-peak", float(np.max(np.abs(ac[1:]))), 1.0, 1e-6)
    # Spectrum calibration: sine power and white noise power
    rng = np.random.default_rng(3)
    n = 65536
    x = 0.5 * np.sin(2 * np.pi * 1000 * np.arange(n) / FS)
    sp = Spectrum.of(x)
    check("tone power -> dBFS (0.5 peak = -6.02)", to_dbfs(sp.tone_power(1000)), -6.0206, 0.01)
    wn = rng.normal(0, 1e-3, n)
    check("white noise total power (dB)", 10 * math.log10(Spectrum.of(wn).p.sum()), -60.0, 0.1)
    # THD+N of a pure tone with known harmonics and noise (direct, no device)
    h2, h3, sig = 10 ** (-90 / 20), 10 ** (-100 / 20), 10 ** (-110 / 20)
    t = np.arange(n) / FS
    A = db_to_amp(-1)
    y = A * np.sin(2 * np.pi * 1000 * t) + A * h2 * np.sin(2 * np.pi * 2000 * t) + A * h3 * np.sin(
        2 * np.pi * 3000 * t) + rng.normal(0, sig, n)
    r = thd_n(y, 1000)
    exp_thd = 20 * math.log10(math.hypot(h2, h3))
    noise_band = sig ** 2 * (BAND[1] - BAND[0]) / (FS / 2)
    exp_thdn = 10 * math.log10((A * A * (h2 * h2 + h3 * h3) / 2 + noise_band) / (A * A / 2))
    check("THD (H2 -90, H3 -100 dBc)", r["thd_db"], exp_thd, 0.1)
    check("H2 level", r["harmonics"][0]["dbc"], -90.0, 0.1)
    check("THD+N incl. -110 dB noise", r["thdn_db"], exp_thdn, 0.2)
    check("fundamental level dBFS", r["fund_dbfs"], -1.0, 0.01)
    # Sub-sample xcorr
    ref = mls(13)
    sd_ = SyntheticDevice(latency=0, sigma=0, h2=0, h3=0, xtalk=0, fir=(0.0, 1.0))
    rec = sd_._channel(np.stack([np.r_[np.zeros(100), ref, np.zeros(2000)]] * 2, 1), 123.25)[:, 0]
    lag, _, _ = xcorr_lag(ref, rec)
    check("xcorr sub-sample lag (100 + 1 + 123.25)", lag, 224.25, 0.02)

    print("selftest: full suite on SyntheticDevice")
    dev = SyntheticDevice()
    res = run_suite(dev, "synthetic_a", latency_runs=3)
    lat_true = dev.latency + 1.0  # + FIR group delay
    check("latency mean (samples)", res["latency"]["mean_samples"], lat_true, 0.03)
    check("latency spread (samples)", res["latency"]["max_samples"] - res["latency"]["min_samples"], 0.0, 0.05)
    check("latency ms", res["latency"]["ms"], lat_true / FS * 1e3, 0.003)
    # frequency response
    ch = res["fr"]["channels"][0]
    f_ = np.array(ch["curve_f"])
    m_ = np.array(ch["curve_db"])
    g1k = float(20 * np.log10(dev.fir_mag(1000.0)))
    for f in (20.0, 100.0, 1000.0, 10000.0, 20000.0):
        want = float(20 * np.log10(dev.fir_mag(f))) - g1k
        check(f"FR re 1 kHz @ {f:g} Hz", float(np.interp(f, f_, m_)) - ch["gain_1k_db"], want, 0.1)
    fb = np.linspace(20, 20000, 4000)
    want_dev = float(np.max(np.abs(20 * np.log10(dev.fir_mag(fb)) - g1k)))
    check("FR max |deviation| 20-20k", res["fr"]["dev_abs_db"], want_dev, 0.1)
    check("loop gain @1 kHz", ch["gain_1k_db"], g1k, 0.1)
    check("sweep IR latency estimate", ch["ir_peak_latency"], lat_true, 1.0)
    # THD / THD+N through the device: y = g x + h2 (g x)^2 + h3 (g x)^3 after FIR gain g at 1 kHz
    g = float(dev.fir_mag(1000.0))
    for lvl, key in ((-1.0, "thdn_m1_L"), (-20.0, "thdn_m20_L")):
        a_ = db_to_amp(lvl) * g
        fund = a_ + 0.75 * dev.h3 * a_ ** 3
        H2, H3 = dev.h2 * a_ ** 2 / 2, dev.h3 * a_ ** 3 / 4
        nb = dev.sigma ** 2 * (BAND[1] - BAND[0]) / (FS / 2) * (1 + dev.xtalk ** 2)
        exp_thd = 20 * math.log10(math.hypot(H2, H3) / fund)
        exp_thdn = 10 * math.log10(((H2 ** 2 + H3 ** 2) / 2 + nb) / (fund ** 2 / 2))
        t_ = res["tones"][key]
        if lvl > -10:  # at -20 dBFS the harmonics (-106 dBc) are below the noise in their FFT lobes
            check(f"THD {lvl:g} dBFS", t_["thd_db"], exp_thd, 0.3)
        check(f"THD+N {lvl:g} dBFS", t_["thdn_db"], exp_thdn, 0.3)
        check(f"THD+N {lvl:g} dBFS (%)", t_["thdn_pct"], 100 * 10 ** (exp_thdn / 20), 100 * 10 ** (exp_thdn / 20) * 0.04)
    # dynamic range, A-weighted (independent integration of the weighting over white noise)
    ff = np.linspace(BAND[0], BAND[1], 200_000)
    wa = float(np.mean(10 ** (a_weighting_db(ff) / 10)))
    na = dev.sigma ** 2 * (1 + dev.xtalk ** 2) * (BAND[1] - BAND[0]) / (FS / 2) * wa
    a60 = db_to_amp(-60) * g
    exp_dr = 60.0 - 10 * math.log10(na / (a60 ** 2 / 2))
    check("dynamic range (A) L", res["tones"]["dr_L"]["dr_db"], exp_dr, 0.3)
    exp_idle = to_dbfs(dev.sigma ** 2 * (1 + dev.xtalk ** 2) * (BAND[1] - BAND[0]) / (FS / 2))
    check("idle noise unweighted (dBFS)", res["tones"]["noise_L"]["unweighted_dbfs"], exp_idle, 0.3)
    check("idle noise A-weighted (dBFS)", res["tones"]["noise_R"]["a_weighted_dbfs"], to_dbfs(na), 0.3)
    check("crosstalk L->R", res["tones"]["xtalk_LtoR_db"], 20 * math.log10(dev.xtalk), 0.2)
    check("crosstalk R->L", res["tones"]["xtalk_RtoL_db"], 20 * math.log10(dev.xtalk), 0.2)

    print("selftest: drift / click detector")
    slip_at, dup_at, spike_at = int(10.0123 * FS), int(20.5 * FS), int(25.25 * FS)
    dev_d = SyntheticDevice(latency=300.0, slips=[(slip_at, +1), (dup_at, -1)], spikes=[(spike_at, 1e-3)], seed=7)
    d = measure_drift(dev_d, 0.5)
    ev = d["events"]
    slips_l = [e for e in ev if e["kind"] == "slip" and e["ch"] == 0]
    clicks_l = [e for e in ev if e["kind"] == "click" and e["ch"] == 0]
    check("drift: slips found (L)", len(slips_l), 2, 0)
    if len(slips_l) == 2:
        check("drift: slip 1 = +1 sample", slips_l[0]["value"], 1.0, 0)
        check("drift: slip 1 time (s)", slips_l[0]["t"], slip_at / FS, 0.15)
        check("drift: slip 2 = -1 sample", slips_l[1]["value"], -1.0, 0)
        check("drift: slip 2 time (s)", slips_l[1]["t"], dup_at / FS, 0.15)
    spike_found = [e for e in clicks_l if abs(e["t"] - spike_at / FS) < 0.1]
    check("drift: -60 dBFS 1-sample spike found", len(spike_found), 1, 0)
    check("drift: pass = False", float(d["pass"]), 0.0, 0)
    clean = measure_drift(SyntheticDevice(latency=300.0, seed=11), 0.5)
    check("drift clean: events", float(len(clean["events"])), 0.0, 0)
    check("drift clean: pass = True", float(clean["pass"]), 1.0, 0)

    print("selftest: compare report")
    res_b = run_suite(SyntheticDevice(sigma=dev.sigma * 4, seed=2, latency_jitter=2.0), "synthetic_b",
                      latency_runs=3)
    with tempfile.TemporaryDirectory() as td:
        out = Path(td)
        save_result(res, out)
        save_result(res_b, out)
        ra, rb = load_result("synthetic_a", out), load_result("synthetic_b", out)
        write_single_report(ra, out)
        p = diff_report(ra, rb, out)
        txt = p.read_text(encoding="utf-8")
        row = [ln for ln in txt.splitlines() if ln.startswith("| idle noise L (A)")]
        check("report flags degraded noise (+12 dB)", float(bool(row) and "⚠" in row[0]), 1.0, 0)
        row = [ln for ln in txt.splitlines() if ln.startswith("| FR deviation")]
        check("report does not flag unchanged FR", float(bool(row) and "⚠" not in row[0]), 1.0, 0)
        check("report plot written", float((out / "compare_synthetic_a_vs_synthetic_b.png").exists()), 1.0, 0)
        check("JSON round-trip latency", rb["latency"]["mean_samples"], res_b["latency"]["mean_samples"], 1e-9)

    print(f"selftest: {'PASS' if not fails else 'FAIL'} ({len(fails)} failure(s))")
    for f_name in fails:
        print(f"  failed: {f_name}")
    return 0 if not fails else 1


# ==============================================================================================
# CLI
# ==============================================================================================
def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="TWOFOLD loopback measurement suite",
                                 epilog=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true", help="synthetic verification, no audio device")
    ap.add_argument("--list-devices", action="store_true", help="list PortAudio devices and exit")
    ap.add_argument("--device", help="device index or name substring (default: auto-find 'TWOFOLD')")
    ap.add_argument("--label", help="run the suite once and save <out>/<label>.json/.md/.png")
    ap.add_argument("--compare", action="store_true", help="interactive: Studio Mode run, then WiFi-on run")
    ap.add_argument("--report", nargs=2, metavar=("A", "B"), help="diff two saved runs (labels or .json paths)")
    ap.add_argument("--drift", type=float, metavar="MIN", help="only the long-run click/slip test, MIN minutes")
    ap.add_argument("--drift-minutes", type=float, default=0.0, help="append a drift run of this length to the suite")
    ap.add_argument("--latency-runs", type=int, default=5, help="latency repetitions (default 5)")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help=f"results directory (default {DEFAULT_OUT})")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if args.report:
        a, b = (load_result(x, args.out) for x in args.report)
        print(f"wrote {diff_report(a, b, args.out)}")
        return 0
    if args.list_devices:
        sd = import_sounddevice()
        print(sd.query_devices())
        return 0
    if not (args.label or args.compare or args.drift):
        ap.print_help()
        return 2
    be = SoundDeviceBackend(args.device)
    if args.drift:
        res = measure_drift(be, args.drift)
        lab = args.label or "drift"
        args.out.mkdir(parents=True, exist_ok=True)
        p = args.out / f"{lab}_drift.json"
        p.write_text(json.dumps(_jsonable(res), indent=1), encoding="utf-8")
        print(f"drift: {res['seconds'] / 60:.1f} min, clicks {res['clicks']}, slips {res['slips']}, "
              f"xruns {len(res['xruns'])} -> {'PASS' if res['pass'] else 'FAIL'}  ({p})")
        return 0 if res["pass"] else 1
    if args.compare:
        input("Set the TWOFOLD to Studio Mode (radios off), DSP bypassed, then press Enter...")
        a = run_suite(be, "studio", args.latency_runs, args.drift_minutes)
        save_result(a, args.out)
        write_single_report(a, args.out)
        input("Now enable WiFi on the TWOFOLD (and open the web editor if desired), then press Enter...")
        b = run_suite(be, "wifi", args.latency_runs, args.drift_minutes)
        save_result(b, args.out)
        write_single_report(b, args.out)
        print(f"wrote {diff_report(a, b, args.out)}")
        return 0
    res = run_suite(be, args.label, args.latency_runs, args.drift_minutes)
    save_result(res, args.out)
    p = write_single_report(res, args.out)
    print("\n".join(summary_md(res)))
    print(f"wrote {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
