#!/usr/bin/env python3
"""TWOFOLD over USB, no WiFi: the web API through the device's vendor interface (what the web editor's
"Connect over USB" uses). libusb-1.0 via ctypes, so nothing to install.

  tools/twofold_usb.py get /api/state
  tools/twofold_usb.py post /api/param '{"key":"out.volume","value":-12}'
  tools/twofold_usb.py get /api/info | python3 -m json.tool
  tools/twofold_usb.py watch              # meters + CPU, 5 Hz

Linux: install tools/99-twofold.rules (sudo cp ... /etc/udev/rules.d/, udevadm control --reload, replug) so a
regular user may open the device. Windows binds WinUSB automatically (MS OS 2.0 descriptor). macOS needs nothing.

Framing (firmware/components/net/net_usb.c): u32 LE length + "METHOD /path\\n" + body -> u32 LE length +
"STATUS\\n" + JSON.
"""
import ctypes as C
import json
import struct
import sys
import time

VID, PID, ITF, EP_OUT, EP_IN = 0x303A, 0x4A57, 5, 0x04, 0x84


class Usb:
    def __init__(self):
        self.lib = C.CDLL("libusb-1.0.so.0")
        self.lib.libusb_open_device_with_vid_pid.restype = C.c_void_p
        self.lib.libusb_strerror.restype = C.c_char_p
        self.ctx = C.c_void_p()
        self._chk(self.lib.libusb_init(C.byref(self.ctx)))
        self.h = C.c_void_p(self.lib.libusb_open_device_with_vid_pid(self.ctx, VID, PID))
        if not self.h.value:
            sys.exit("TWOFOLD not found or not openable (Linux: install tools/99-twofold.rules)")
        self.lib.libusb_set_auto_detach_kernel_driver(self.h, 1)
        self._chk(self.lib.libusb_claim_interface(self.h, ITF))

    def _chk(self, r):
        if r < 0:
            raise OSError(self.lib.libusb_strerror(r).decode())
        return r

    def _xfer(self, ep, data, timeout=2000):
        buf = (C.c_ubyte * len(data)).from_buffer_copy(data) if ep & 0x80 == 0 else (C.c_ubyte * len(data))()
        n = C.c_int(0)
        self._chk(self.lib.libusb_bulk_transfer(self.h, ep, buf, len(data), C.byref(n), timeout))
        return bytes(buf[: n.value])

    def rpc(self, method, path, body=None):
        payload = (f"{method} {path}\n" + (json.dumps(body) if body is not None else "")).encode()
        self._xfer(EP_OUT, struct.pack("<I", len(payload)) + payload)
        got = b""
        length = None
        while length is None or len(got) < length:
            got += self._xfer(EP_IN, bytes(4096))
            if length is None and len(got) >= 4:
                length = struct.unpack("<I", got[:4])[0]
                got = got[4:]
        status, _, text = got[:length].decode(errors="replace").partition("\n")
        j = json.loads(text) if text else {}
        if int(status) >= 400 or j.get("ok") is False:
            raise RuntimeError(f"{status}: {j.get('error', text)}")
        return j

    def close(self):
        self.lib.libusb_release_interface(self.h, ITF)
        self.lib.libusb_close(self.h)


def main(argv):
    if len(argv) < 2 or argv[1] not in ("get", "post", "watch"):
        print(__doc__)
        return 2
    u = Usb()
    try:
        if argv[1] == "watch":
            while True:
                m = u.rpc("GET", "/api/meters")
                print(f"\rin {m['in'][0]:6.1f} {m['in'][1]:6.1f}  out {m['out'][0]:6.1f} {m['out'][1]:6.1f} dBFS  "
                      f"GR {m.get('gr', 0):4.1f}  CPU {m.get('cpu', 0):5.1f} %   ", end="", flush=True)
                time.sleep(0.2)
        body = json.loads(argv[3]) if len(argv) > 3 else ({} if argv[1] == "post" else None)
        t0 = time.time()
        r = u.rpc(argv[1].upper(), argv[2], body)
        print(json.dumps(r, indent=1))
        print(f"({(time.time() - t0) * 1000:.0f} ms)", file=sys.stderr)
    finally:
        u.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
