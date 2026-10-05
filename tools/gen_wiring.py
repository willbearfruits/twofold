#!/usr/bin/env python3
"""Generate docs/wiring.md from firmware/components/board/include/board_config.h.

The header is the single source of truth for pins. Every pin define carries a comment of the form

    #define BOARD_PIN_X   <gpio>   /* PIN: <signal> | <module pin(s), comma separated> | <notes> */

This script parses those comments and emits, deterministically (no timestamps, so re-running is a no-op):
  * the S3-Zero GPIO -> module pin table (both clock modes),
  * a per-module view (which header pin goes where),
  * power and ground wiring,
  * the fixed strapping of PCM1808 (FMT, MD0, MD1) and PCM5102A (FLT, DEMP, XSMT, FMT, SCK) for the internal
    clock mode and the low-jitter external 12.288 MHz oscillator mode (PLAN.md 4.3),
  * ASCII block diagrams of both modes.

Usage:
    python3 tools/gen_wiring.py            # (re)write docs/wiring.md, prints "unchanged" if identical
    python3 tools/gen_wiring.py --check    # exit 1 if docs/wiring.md is out of date (CI)
    python3 tools/gen_wiring.py --stdout   # print instead of writing
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "firmware" / "components" / "board" / "include" / "board_config.h"
OUT = ROOT / "docs" / "wiring.md"

PIN_RE = re.compile(
    r"^\s*#define\s+(?P<macro>BOARD_PIN_\w+)\s+(?P<gpio>\d+)\s*/\*\s*PIN:\s*(?P<signal>[^|]*?)\s*\|"
    r"\s*(?P<module>[^|]*?)\s*\|\s*(?P<notes>.*?)\s*\*/\s*$")
DEFINE_RE = re.compile(r"^\s*#define\s+(?P<macro>BOARD_\w+)\s+(?P<val>[^\s/]+)")


@dataclass(frozen=True)
class Pin:
    macro: str
    gpio: int
    signal: str
    modules: tuple[str, ...]   # e.g. ("PCM1808 BCK", "PCM5102A BCK")
    notes: str


def parse_header(text: str) -> tuple[list[Pin], dict[str, str]]:
    """Return the PIN-annotated defines (in file order) and all plain BOARD_* defines."""
    pins: list[Pin] = []
    defines: dict[str, str] = {}
    for line in text.splitlines():
        m = PIN_RE.match(line)
        if m:
            mods = tuple(s.strip() for s in m["module"].split(",") if s.strip())
            pins.append(Pin(m["macro"], int(m["gpio"]), m["signal"].strip(), mods, m["notes"].strip()))
            continue
        d = DEFINE_RE.match(line)
        if d:
            defines.setdefault(d["macro"], d["val"])
    if not pins:
        raise SystemExit(f"error: no '/* PIN: ... */' annotated defines found in {HEADER}")
    gp = [p.gpio for p in pins]
    dup = sorted({g for g in gp if gp.count(g) > 1})
    if dup:
        raise SystemExit(f"error: GPIO(s) assigned twice in {HEADER.name}: {dup}")
    return pins, defines


# Direction of each I2S/DAC signal per clock mode (from PLAN.md 4.3). Pins not listed are mode-independent.
MODE_DIR = {
    "BOARD_PIN_I2S_MCLK": ("out (MCLK 12.288 MHz)", "unused: GPIO left as input, not connected"),
    "BOARD_PIN_I2S_BCLK": ("out (BCK 3.072 MHz)", "in (BCK from PCM1808)"),
    "BOARD_PIN_I2S_WS": ("out (LRCK 48 kHz)", "in (LRCK from PCM1808)"),
    "BOARD_PIN_I2S_DOUT": ("out", "out"),
    "BOARD_PIN_I2S_DIN": ("in", "in"),
    "BOARD_PIN_DAC_XSMT": ("out", "out"),
    "BOARD_PIN_AMP_SD": ("out", "out"),
}
DEFAULT_DIR = {
    "BOARD_PIN_I2C_SDA": "io (open drain)", "BOARD_PIN_I2C_SCL": "out (open drain)",
    "BOARD_PIN_ENC_A": "in (pull-up)", "BOARD_PIN_ENC_B": "in (pull-up)", "BOARD_PIN_ENC_SW": "in (pull-up)",
    "BOARD_PIN_LED_WS2812": "out (RMT)",
}


def _cell(s: str) -> str:
    return s.replace("|", "/")


def gpio_table(pins: list[Pin]) -> list[str]:
    L = ["| GPIO | signal | connects to (module pin) | dir, internal clock | dir, ext. oscillator | notes |",
         "|---|---|---|---|---|---|"]
    for p in sorted(pins, key=lambda q: q.gpio):
        d_int, d_ext = MODE_DIR.get(p.macro, (DEFAULT_DIR.get(p.macro, "-"),) * 2)
        conn = "<br>".join(p.modules)
        if p.macro == "BOARD_PIN_I2S_MCLK":
            conn += " *(internal mode only)*"
        L.append(f"| **GPIO{p.gpio}** | {_cell(p.signal)} | {_cell(conn)} | {d_int} | {d_ext} | {_cell(p.notes)} |")
    return L


def module_view(pins: list[Pin]) -> list[str]:
    """Per module: header pin -> S3 GPIO (only signal pins from the header)."""
    by_mod: dict[str, list[tuple[str, Pin]]] = {}
    for p in pins:
        for m in p.modules:
            if m.lower().startswith("onboard"):
                continue  # on the board itself, nothing to wire
            parts = m.split(None, 1)
            mod, pin = (parts[0], parts[1]) if len(parts) == 2 else (m, "")
            by_mod.setdefault(mod, []).append((pin, p))
    L: list[str] = []
    for mod in sorted(by_mod):
        L += [f"**{mod}**", "", "| module pin | GPIO | signal |", "|---|---|---|"]
        for pin, p in sorted(by_mod[mod], key=lambda t: t[0]):
            L.append(f"| {pin or '-'} | GPIO{p.gpio} | {p.signal} |")
        L.append("")
    return L


POWER = """\
| from | to | notes |
|---|---|---|
| board **5V** (USB VBUS) | PCM1808 module **+5V** (VCC) | analog supply; the module's LDO makes its 3.3 V. If your module has *no* LDO, feed its 3.3V pin from board 3V3 instead |
| board **5V** (USB VBUS) | PCM5102A module **VIN** | GY-PCM5102 has its own LDOs (3.3–5 V input); 5 V gives the cleanest AVDD |
| board **3V3** | OLED **VCC** | |
| board **3V3** | 12.288 MHz oscillator **VDD** and **OE/ST** *(low-jitter mode only)* | 100 nF + 1 µF ceramic right at the oscillator VDD pin |
| board **GND** | PCM1808 GND, PCM5102A GND, OLED GND, EC11 C (common) and switch leg, oscillator GND | star ground at the board GND pin; keep the audio modules' grounds short and together, run the USB/digital return away from the analog jacks |
| — | PCM1808 module 3.3V pin | leave open when the module regulates its own 3.3 V (do not back-feed the module LDO) |

Logic levels: all I2S/control signals are 3.3 V (ESP32-S3 IO, PCM1808 DVDD = 3.3 V, PCM5102A DVDD = 3.3 V).
Signal integrity: keep MCLK/BCK wires short (< 10 cm) and twisted with a ground wire; a 22–33 Ω series resistor at
the driving end of MCLK and BCK (S3 GPIO4/5 in internal mode) damps ringing on flying leads.
"""

STRAPS = """\
| converter pin | internal clock (default) | low-jitter mode (`CONFIG_TWOFOLD_CLOCK_EXT_OSC`) | meaning |
|---|---|---|---|
| PCM1808 **FMT** | GND (0) | GND (0) | I2S data format |
| PCM1808 **MD0** | GND (0) | 3V3 (1) | MD1:MD0 = 00: slave; 11: master, SCKI = 256·fs |
| PCM1808 **MD1** | GND (0) | 3V3 (1) | (see MD0) |
| PCM1808 **SCKI** | ← S3 GPIO21 (MCLK 12.288 MHz) | ← oscillator via **33 Ω** | system clock |
| PCM1808 **BCK / LRC** | inputs ← S3 GPIO7 / GPIO8 | **outputs** → S3 GPIO7 / GPIO8 and PCM5102A BCK / LCK | bit / frame clock |
| PCM5102A **FMT** | GND (L) | GND (L) | I2S data format |
| PCM5102A **FLT** | GND (L) | GND (L) | normal-latency FIR filter |
| PCM5102A **DEMP** | GND (L) | GND (L) | de-emphasis off |
| PCM5102A **XSMT** | 3V3 (H) — bridged on the kit board | 3V3 (H) | high = play. Optional: wire it to a free GPIO and set BOARD_PIN_DAC_XSMT for a hardware soft-mute at boot |
| PCM5102A **SCK** | **GND** (internal PLL locks to BCK) | ← oscillator via **33 Ω** (PLL bypassed) | system clock |

GY-PCM5102 boards set FLT/DEMP/XSMT/FMT with solder jumpers H1L–H4L on the back: bridge FLT, DEMP and FMT to L,
and bridge XSMT (H3) to H (as on the kit board). Its SCK pad is often bridged to GND on the
front: keep that bridge for the internal mode, remove it for the low-jitter mode.
"""

DIAG_INT = """\
INTERNAL CLOCK MODE (default): ESP32-S3 is I2S master, PLL_F160M / (13 + 1/48) -> MCLK. Kit board wiring.

                       +--------------------------------+
   USB-C ==============| ESP32-S3 SuperMini             |         +-------------------+
                       |  GPIO7  BCK    3.072 MHz  ---->|---+---->| SCK    INMP441    |<== mic (L slot)
                       |  GPIO8  LRCK      48 kHz  ---->|---|-+-->| WS     (L/R = GND)|
                       |  GPIO10 DIN               <----|---|-|---| SD                |
                       |                                |   | |   +-------------------+
                       |                                |   | |   +-------------------+
                       |                                |   +-|-->| BCK               |
                       |                                |   | +-->| LCK   PCM5102A    |==> line out L/R
                       |  GPIO9  DOUT              ---->|---|-|-->| DIN   (SCK = GND, |
                       |                                |   | |   |  XSMT = H)        |
                       |                                |   | |   +-------------------+
                       |                                |   +-|-->| BCLK  MAX98357A   |==> speaker
                       |                                |     +-->| LRC               |
                       |  GPIO9  DOUT (same line)  ---->|-------->| DIN               |
                       |  GPIO11 AMP SD            ---->|-------->| SD (high = on)    |
                       |                                |         +-------------------+
                       |  GPIO21 MCLK -> PCM1808 SCKI (only if a PCM1808 replaces the mic on GPIO10)
                       |  GPIO12/13  I2C  ---> OLED     |
                       |  GPIO4/5/6  <--- EC11 A/B/SW   |
                       +--------------------------------+
"""

DIAG_EXT = """\
LOW-JITTER MODE (CONFIG_TWOFOLD_CLOCK_EXT_OSC): 12.288 MHz XO, PCM1808 is master, ESP32-S3 + PCM5102A are slaves

     +-------------+   33R    +-------------------+
     | 12.288 MHz  |---/\\/\\---| SCKI              |
     |  XO (3V3)   |          |      PCM1808      |<== line in L/R
     |             |   33R    |  (master 256fs,   |
     |             |---/\\/\\-+ |   MD1:0 = 11)     |
     +-------------+        | |  BCK  LRC  DOUT   |
                            | +---+----+-----+----+
                            |     |    |     |
   +------------------------|-----|----|-----|-----+
   | ESP32-S3 SuperMini     |     |    |     |     |
   |  GPIO7 BCK  (in)  <----|-----+    |     |     |     GPIO21 (MCLK): not connected
   |  GPIO8 LRCK (in)  <----|-----|----+     |     |
   |  GPIO10 DIN       <----|-----|----|-----+     |
   |  GPIO9 DOUT  ----+     |     |    |           |
   |  XSMT = H ---+   |     |     |    |           |
   +--------------|---|-----|-----|----|-----------+
                  |   |     |     |    |
              +---v---v-----v-----v----v----+
              | XSMT DIN   SCK   BCK  LCK   |
              |  PCM5102A (slave,           |==> line out L/R
              |  SCK from XO: no PLL)       |
              +-----------------------------+
"""


def render(pins: list[Pin], defines: dict[str, str], header_rel: str) -> str:
    fs = defines.get("BOARD_SAMPLE_RATE", "48000")
    k = defines.get("BOARD_MCLK_MULTIPLE", "256")
    xo = defines.get("BOARD_EXT_OSC_HZ", "12288000")
    unused = sorted(set(range(1, 14)) - {p.gpio for p in pins} - {1, 2, 3})   # 1/2/3: the kit's joystick
    L = ["# TWOFOLD wiring", "",
         f"<!-- GENERATED by tools/gen_wiring.py from {header_rel}. Do not edit by hand: change the header's",
         "     `/* PIN: signal | module pin | notes */` comments and re-run the script. -->", "",
         f"Board: ESP32-S3 SuperMini (kit wiring, same as gesture-recorder). Audio: {int(fs) // 1000} kHz, MCLK = {k}·fs "
         f"({int(fs) * int(k) / 1e6:.3f} MHz) in internal mode, external oscillator {int(xo) / 1e6:.3f} MHz "
         "in low-jitter mode (PLAN.md §4).", "",
         "## 1. SuperMini GPIO → modules", "", *gpio_table(pins), "",
         "GPIO1/2/3 stay with the kit's joystick (unused by TWOFOLD). Avoid strapping pins 0/3/45/46, USB 19/20, "
         "UART0 43/44 (console); GPIO48 is the onboard RGB LED.", "",
         "## 2. Per-module view", "", *module_view(pins),
         "Other module pins: PCM1808 FMT/MD0/MD1 and PCM5102A FLT/DEMP/FMT/SCK are strapped (section 4); EC11 C "
         "and the second switch leg go to GND; OLED VCC/GND per section 3.", "",
         "## 3. Power and ground", "", POWER,
         "## 4. Converter strapping, both clock modes", "", STRAPS,
         "## 5. Block diagrams", "", "```", DIAG_INT.rstrip(), "```", "", "```", DIAG_EXT.rstrip(), "```", "",
         "Low-jitter mode notes: one oscillator output drives two loads through **separate 33 Ω** series resistors "
         "placed at the oscillator (star, not daisy-chain). BCK and LRCK come from the PCM1808 and fan out to the "
         "ESP32-S3 and the PCM5102A; the firmware configures I2S0 TX+RX as slave. Switching modes = rewire "
         "MD0/MD1, SCKI, PCM5102A SCK and rebuild with `CONFIG_TWOFOLD_CLOCK_EXT_OSC`.", ""]
    return "\n".join(L)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--header", type=Path, default=HEADER)
    ap.add_argument("--out", type=Path, default=OUT)
    ap.add_argument("--check", action="store_true", help="exit 1 if the output is out of date")
    ap.add_argument("--stdout", action="store_true", help="print instead of writing")
    args = ap.parse_args(argv)
    pins, defines = parse_header(args.header.read_text(encoding="utf-8"))
    try:
        rel = str(args.header.resolve().relative_to(ROOT))
    except ValueError:
        rel = str(args.header)
    text = render(pins, defines, rel)
    if args.stdout:
        sys.stdout.write(text)
        return 0
    old = args.out.read_text(encoding="utf-8") if args.out.exists() else None
    if args.check:
        if old != text:
            print(f"{args.out} is out of date: run tools/gen_wiring.py")
            return 1
        print(f"{args.out} is up to date")
        return 0
    if old == text:
        print(f"{args.out}: unchanged ({len(pins)} pins)")
        return 0
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8")
    print(f"{args.out}: written ({len(pins)} pins)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
