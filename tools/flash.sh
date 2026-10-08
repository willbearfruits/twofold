#!/usr/bin/env bash
# Flash the TWOFOLD with no buttons: if the app is running, ask it (USB-MIDI SysEx "BOOT") to reboot into the ROM
# download mode, flash firmware/build, then start the app again. Falls back to asking for BOOT+RST.
set -uo pipefail
cd "$(dirname "$0")/../firmware"
. ~/esp/esp-idf/export.sh >/dev/null 2>&1
app()  { lsusb | grep -q 303a:4a57; }
rom()  { [ -e /dev/ttyACM0 ] && lsusb | grep -q 303a:1001; }
if app && ! rom; then
  PORT=$(amidi -l | awk '/TWOFOLD/{print $2; exit}')
  echo "app running: requesting bootloader over MIDI ($PORT)"
  amidi -p "$PORT" -S 'F0 7D 7E 42 4F 4F 54 F7'
fi
for i in $(seq 1 ${WAIT:-20}); do rom && break; sleep 1; done
rom || { echo "no bootloader port: hold BOOT, tap RST, then re-run (or WAIT=600 tools/flash.sh)"; exit 1; }
sleep 1
idf.py -p /dev/ttyACM0 flash 2>&1 | grep -E "Hash of data|Done|rror|fatal" || exit 1
sleep 1
for a in 1 2; do esptool.py --chip esp32s3 -p /dev/ttyACM0 --before no_reset run >/dev/null 2>&1
  for i in $(seq 1 8); do app && break; sleep 1; done; app && break; done
app || { esptool.py --chip esp32s3 -p /dev/ttyACM0 --before no_reset --after watchdog_reset chip_id >/dev/null 2>&1
  for i in $(seq 1 15); do app && break; sleep 1; done; }
app && echo "TWOFOLD running" || { echo "app did not come back (press RST)"; exit 1; }
