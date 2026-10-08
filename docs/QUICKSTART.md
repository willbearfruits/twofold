# TWOFOLD quick start (for testers)

TWOFOLD is a USB audio interface: 48 kHz / 24-bit, stereo in and out, plus a USB-MIDI port, with an effects chain
and a small screen and knob. It needs no driver: it uses the audio class driver built into Linux, macOS and Windows 10/11.

## Plug it in
* Connect it to a USB port that can supply 500 mA (a direct port, not a passive hub). It boots in about a second.
* Your computer shows a new audio device **TWOFOLD** (input and output, 48 kHz, 24-bit) and a MIDI port of the same
  name. Select it as the output device in the OS or in your DAW. In the DAW, set the sample rate to 48 kHz.
* Audio out: the line output (RCA / 3.5 mm) and, on boards with the amplifier, the speaker terminals.
* The radios (WiFi, Bluetooth) are **off** by default. This is "Studio Mode", the cleanest state. Nothing needs them.

## The knob
* Turn: volume on the home screen, scroll in menus. Press: enter / select. Hold (1 s): back / menu.
* MENU → EFFECTS: EQ, saturation, chorus, delay, reverb, compressor, loudness, crossfeed. Each has an on/off and a
  few parameters; the screen animates what the effect does.
* MENU → PRESETS: 8 slots. Load with a press, hold to save the current settings into a slot.
* MENU → SETTINGS: input source and gain, direct monitor mix, limiter, display, FACTORY RESET, ABOUT (firmware version).

Settings are saved automatically a few seconds after you change them (while audio is silent, to avoid clicks).
Unplugging within ~3 s of a change can lose that one change.

## The editor (every parameter, meters, presets, firmware update)
Two ways, both optional:
1. **USB, no WiFi** (Chrome or Edge): open `editor.html` from this folder and click **Connect over USB**.
   Windows binds its driver automatically. Linux: copy `99-twofold.rules` to `/etc/udev/rules.d/`, run
   `sudo udevadm control --reload`, replug. macOS needs nothing.
2. **WiFi**: MENU → NETWORK → WIFI on. The device opens an access point `TWOFOLD-xxxx`; the password is shown on
   the screen. Join it and the editor opens by itself (or go to http://192.168.4.1). To use your own network:
   NETWORK → JOIN A NETWORK, then the editor is at http://twofold.local. WiFi adds a little noise on cheap power
   supplies and uses CPU on the control core only; turn it off when you don't need it.

## MIDI
The USB-MIDI port passes MIDI through and accepts control changes for parameters (mappable in the editor →
MIDI). BLE-MIDI (Bluetooth) can be enabled under MENU → NETWORK → BLUETOOTH.

## If something goes wrong
* **No sound**: check the OS output device and that the DAW is at 48 kHz; check the volume on the home screen.
* **Clicks / dropouts**: note what you were doing (WiFi on? which effects?) and the OS. Please send the status line:
  from the repo run `tools/twofold_status.sh`, or send the MIDI SysEx `F0 7D 03 F7` to the TWOFOLD port with any
  MIDI tool and copy the reply.
* **It restarted by itself**: it keeps a crash report with a backtrace. `tools/crash_report.sh`, or SysEx
  `F0 7D 01 F7`, returns it. Please include it in your report.
* **Stuck / strange state**: unplug and replug. Then MENU → SETTINGS → FACTORY RESET if needed.
* **Firmware update**: editor → Device → Firmware → choose `twofold-*-ota.bin`. Audio mutes during the write.
  The new firmware runs on probation and the old one returns automatically if the new one fails to stream audio.
  USB reflashing instructions are in `FLASH.md`.

## What to report
OS and version, what you plugged it into (port / hub), what worked, what didn't, and the status line or crash report
if you have one. The things most worth trying: hours of playback without a click, a Windows and a macOS machine,
the editor over USB, a preset save/load, WiFi on and off while playing.
