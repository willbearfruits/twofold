# Notices

TWOFOLD is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License
as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See [LICENSE](LICENSE).

Copyright (C) 2026 Yaniv Schonfeld (willbearfruits).

The code in this repository is TWOFOLD's own: the firmware components under `firmware/components/` and
`firmware/main/`, the web editor, the host tests and simulator, the tools, the enclosure and the docs. The pixel fonts
in `firmware/components/gfx/fonts/` were drawn for TWOFOLD. Nothing third-party is copied into the repository; the
libraries below are fetched at build time and end up in the firmware images.

## What the firmware images carry besides TWOFOLD

The release images (`twofold-*-full.bin`, `twofold-*-ota.bin`, and `docs/flash/`) are built with ESP-IDF 5.5.1 and
link these:

- **ESP-IDF** (Espressif Systems), the framework, drivers and bootloader. Apache License 2.0.
  https://github.com/espressif/esp-idf. Parts of ESP-IDF that the image uses carry their own licences:
  - **FreeRTOS** kernel (Amazon.com, Inc. and contributors): MIT.
  - **lwIP** TCP/IP stack (Swedish Institute of Computer Science and contributors): BSD-3-Clause.
  - **Mbed TLS** (The Mbed TLS Contributors), used by the WiFi supplicant: Apache-2.0.
  - **NimBLE** Bluetooth LE host (Apache Software Foundation), for BLE-MIDI: Apache-2.0.
  - **cJSON** (Dave Gamble and cJSON contributors), for the web API: MIT.
  - **newlib** C library (Red Hat and others): a set of BSD-style licences, listed in ESP-IDF's
    `components/newlib` and in newlib's `COPYING.NEWLIB`.
  - Espressif's precompiled WiFi, Bluetooth controller and PHY libraries, shipped with ESP-IDF under its terms.
- **TinyUSB** 0.21 (Ha Thach, tinyusb.org, and contributors), the USB audio, MIDI and vendor classes: MIT.
  https://github.com/hathach/tinyusb, via the `espressif/tinyusb` component.
- **esp-dsp** 1.8 (Espressif Systems): Apache-2.0. https://github.com/espressif/esp-dsp
- **led_strip** 3.0 (Espressif Systems), the status LED driver: Apache-2.0.
  https://github.com/espressif/idf-extra-components
- **mdns** 1.14 (Espressif Systems), so the device answers as `twofold.local`: Apache-2.0.
  https://github.com/espressif/esp-protocols

The exact versions are pinned in [firmware/dependencies.lock](firmware/dependencies.lock).

## The project page

`docs/index.html` loads the IBM Plex Sans and IBM Plex Mono fonts from Google Fonts (IBM Corp., SIL Open Font License
1.1) and the browser flasher from **ESP Web Tools** (Nabu Casa / ESPHome, Apache-2.0,
https://github.com/esphome/esp-web-tools). Neither is copied into the repository.

## Written from published work

These are algorithms TWOFOLD implements from papers and public descriptions; no code was taken from them.

- **Plate reverb** after Jon Dattorro, "Effect Design, Part 1: Reverberator and Other Filters", J. Audio Eng. Soc.
  45(9), 1997. Delay lengths rescaled from 29761 Hz to 24 kHz (`firmware/components/dsp/reverb.c`).
- **EQ filter design** from Robert Bristow-Johnson's "Cookbook formulae for audio EQ biquad filter coefficients"
  (`rbj.c`); the low-frequency bands use a trapezoidal state-variable filter matched to the same responses (`svf.c`).
- **Headphone crossfeed** in the style of Benjamin Bauer's 1961 stereophonic-earphone filter, as popularised by
  the bs2b project (`xfeed.c`).
- **Loudness compensation** from the equal-loudness contours of ISO 226:2003 (`loud.c`).
