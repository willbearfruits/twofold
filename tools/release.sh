#!/usr/bin/env bash
# Build a tester/production release into dist/twofold-<version>/:
#   twofold-<ver>-full.bin   everything (bootloader, partition table, OTA data, app): esptool write_flash 0x0
#   twofold-<ver>-ota.bin    app only, for the web editor's Device -> Firmware (OTA, with automatic rollback)
#   editor.html              the web editor, opened locally in Chrome/Edge for "Connect over USB"
#   99-twofold.rules         Linux udev rule for USB access without root
#   SHA256SUMS, QUICKSTART.md, FLASH.md
# Version = git describe (tag releases: git tag -a v1.0.0 -m ... ; then tools/release.sh), or $1.
set -euo pipefail
cd "$(dirname "$0")/.."
VER=${1:-$(git describe --tags --always --dirty)}
. ~/esp/esp-idf/export.sh >/dev/null 2>&1
echo "building $VER"
(cd firmware && idf.py build 2>&1 | grep -E "error|rror:|Project build complete" | head -5)
D="$PWD/dist/twofold-$VER"
rm -rf "$D" && mkdir -p "$D"
(cd firmware/build && esptool.py --chip esp32s3 merge_bin -o "$D/twofold-$VER-full.bin" @flash_args >/dev/null)
cp firmware/build/twofold.bin "$D/twofold-$VER-ota.bin"
cp firmware/components/net/web/index.html "$D/editor.html"
cp tools/99-twofold.rules docs/QUICKSTART.md "$D/"
cat > "$D/FLASH.md" <<MD
# Flashing TWOFOLD $VER over USB

Needs \`esptool\` (\`pip install esptool\`). Plug the board in. If the TWOFOLD firmware is already running, it
shows up as a USB audio device, not a serial port: hold BOOT, tap RESET (or ask it: \`tools/flash.sh\` in the repo
does this with no buttons), then:

    esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 460800 write_flash 0x0 twofold-$VER-full.bin

(Windows: the port is COMx; macOS: /dev/cu.usbmodem*.) Press RESET afterwards if it does not restart by itself.
The full image replaces settings and presets (it writes the empty OTA-data area; NVS is kept).

Updates without a cable: web editor -> Device -> Firmware -> \`twofold-$VER-ota.bin\`. The new image runs once on
probation; if audio does not stream cleanly within 3 minutes (or it crashes) the previous image comes back by itself.
MD
(cd "$D" && sha256sum ./*.bin > SHA256SUMS)
ls -la "$D" | awk 'NR>1{print $5, $9}'
echo "release in $D"
