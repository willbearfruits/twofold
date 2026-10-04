#!/usr/bin/env python3
"""
TWOFOLD web editor mock server (Python 3 stdlib only).

Serves the real web app (firmware/components/net/web/index.html, re-read on every request so
you can edit it live) with a fake implementation of the device API (see
firmware/components/net/API.md): parameters parsed from params_table.h, factory presets parsed
from factory_presets.c, simulated meters/CPU, presets, CC map, WiFi setup flow and OTA.
Live data via a hand-rolled WebSocket (/ws, RFC 6455 subset); the page falls back to polling
when it is opened with ?nows.

  python3 host/web_mock/server.py               # http://127.0.0.1:8817/
  python3 host/web_mock/server.py --ap          # start in WiFi setup (captive portal) mode
  python3 host/web_mock/server.py --host 0.0.0.0 --port 8000   # reachable from a phone
"""
import argparse
import base64
import hashlib
import json
import math
import random
import re
import select
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "firmware/components/net/web/index.html"
TABLE = ROOT / "firmware/components/params/include/params_table.h"
FACTORY = ROOT / "firmware/components/params/factory_presets.c"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


# ------------------------------------------------------------------ parameter table
def fnum(s):
    return float(s.strip().rstrip("f").rstrip("."))


def parse_table():
    text = TABLE.read_text()
    enums = {m.group(1): re.findall(r'"([^"]*)"', m.group(2)) for m in re.finditer(r"E\((E_\w+),([^)]*)\)", text)}
    rx = re.compile(r'X\((P_\w+),\s*"([^"]*)",\s*"([^"]*)",\s*"([^"]*)",\s*PT_(\w+),\s*PC_(\w+),\s*([^,]+),\s*([^,]+),'
                    r'\s*([^,]+),\s*([^,]+),\s*PF_(\w+),\s*(\w+),\s*(\w+)\)')
    params = []
    for m in rx.finditer(text):
        pid, key, name, unit, typ, curve, mn, mx, df, st, flags, en, cc = m.groups()
        d = {"id": pid, "key": key, "name": name, "unit": unit, "type": typ.lower(), "curve": curve.lower(),
             "min": fnum(mn), "max": fnum(mx), "def": fnum(df), "step": fnum(st), "flags": flags.lower(),
             "cc": None if cc == "NO_CC" else int(cc)}
        if typ == "ENUM":
            d["enum"] = enums.get(en, [])
        params.append(d)
    return params


def parse_factory(params):
    text = FACTORY.read_text()
    arrays = {}
    for m in re.finditer(r"preset_kv_t\s+(\w+)\[\]\s*=\s*\{(.*?)\};", text, re.S):
        kv = re.findall(r"KV\((P_\w+),\s*(-?[\d.]+)f?", m.group(2))
        arrays[m.group(1)] = [(k, float(v)) for k, v in kv]
    by_id = {p["id"]: p for p in params}
    slots = []
    for m in re.finditer(r'\{\s*"([^"]+)",\s*(\w+),\s*N\(\w+\)\s*\}', text):
        vals = {p["key"]: p["def"] for p in params if p["flags"] == "preset"}
        for pid, v in arrays.get(m.group(2), []):
            if pid in by_id:
                vals[by_id[pid]["key"]] = v
        slots.append({"name": m.group(1), "values": vals})
    while len(slots) < 8:
        slots.append({"name": f"SLOT {len(slots) + 1}", "values": {p["key"]: p["def"] for p in params if p["flags"] == "preset"}})
    return slots[:8]


# ------------------------------------------------------------------ simulated device
class Device:
    def __init__(self, ap_mode):
        self.lock = threading.RLock()
        self.params = parse_table()
        self.pk = {p["key"]: p for p in self.params}
        self.values = {p["key"]: p["def"] for p in self.params}
        self.changed = {}                       # key -> seq of last change
        self.seq = 1
        self.epoch = 0
        self.factory = parse_factory(self.params)
        self.slots = json.loads(json.dumps(self.factory))
        self.active = 0
        self.dirty = False
        self.cc = {p["key"]: p["cc"] for p in self.params}
        self.t0 = time.time()
        self.fw = "1.0.0"
        self.reboot_until = 0
        self.ota = None
        self.ble = "off"
        w = {"state": "setup" if ap_mode else "connected", "mode": "ap" if ap_mode else "sta",
             "ssid": "" if ap_mode else "Studio 5G", "ip": "" if ap_mode else "192.168.1.42", "rssi": -54,
             "saved": not ap_mode, "error": "", "scanning": False, "networks": [], "t": 0}
        self.wifi = w
        self.apply_preset(0, epoch=False)
        self.dirty = False

    # -- params
    def quant(self, d, v):
        if not isinstance(v, (int, float)) or math.isnan(v) or math.isinf(v):
            v = d["def"]
        v = min(max(float(v), d["min"]), d["max"])
        return float(round(v)) if d["type"] != "float" else v

    def set(self, key, v, src="web"):
        d = self.pk.get(key)
        if not d:
            return None
        v = self.quant(d, v)
        with self.lock:
            if key == "net.ble":
                self.ble = "advertising" if v >= .5 else "off"
            if self.values[key] != v:
                self.values[key] = v
                self.seq += 1
                self.changed[key] = self.seq
                if d["flags"] == "preset" and src == "web":
                    self.dirty = True
        return v

    def apply_preset(self, slot, epoch=True):
        with self.lock:
            for k, v in self.slots[slot]["values"].items():
                self.set(k, v, src="preset")
            self.active = slot
            self.dirty = False
            if epoch:
                self.epoch += 1

    # -- status / meters
    def wifi_tick(self):
        w = self.wifi
        t = time.time()
        if w["scanning"] and t - w["t"] > 1.8:
            w["scanning"] = False
            w["networks"] = [
                {"ssid": "Studio 5G", "rssi": -48, "auth": "wpa2/3", "ch": 36 % 14 or 11},
                {"ssid": "Studio", "rssi": -52, "auth": "wpa2", "ch": 6},
                {"ssid": "FRITZ!Box 7590 KL", "rssi": -67, "auth": "wpa2", "ch": 1},
                {"ssid": "Vodafone-8C2A", "rssi": -74, "auth": "wpa2", "ch": 11},
                {"ssid": "cafe guest", "rssi": -83, "auth": "open", "ch": 6},
            ]
        if w["state"] == "connecting" and t - w["t"] > 3:
            w.update(state="connected", ip="192.168.1.42", rssi=-55, error="", saved=True, t=t)
        if w["state"] == "connected" and w["mode"] == "apsta" and t - w["t"] > 20:
            w["mode"] = "sta"

    def status(self):
        with self.lock:
            self.wifi_tick()
            w = self.wifi
            cpu = self.cpu()
            return {"usb": True, "play": True, "rec": False, "preset": self.active, "preset_dirty": self.dirty,
                    "pending": False, "wifi": w["state"], "ssid": w["ssid"], "ip": w["ip"] or "192.168.4.1",
                    "rssi": w["rssi"], "ble": self.ble, "ota": self.ota is not None, "ota_pct": self.ota or 0,
                    "cpu": round(cpu[0], 1), "cpu_max": round(cpu[1], 1)}

    def stages(self):
        v = self.values
        on = lambda k: v.get(k, 0) >= .5
        return [("INPUT", 1.2), ("EQ", 3.0 if on("eq.on") else .1), ("SAT", 6.0 if on("sat.on") else .1),
                ("CHORUS", 3.0 if on("cho.on") else .1), ("DELAY", 6.0 if on("dly.on") else .1),
                ("REVERB", 8.0 if on("rev.on") else .1), ("COMP", 2.4 if on("cmp.on") else .1), ("OUTPUT", 3.0), ("DITHER", .8)]

    def cpu(self):
        s = sum(x for _, x in self.stages())
        j = .4 * math.sin(time.time() * 1.7)
        return s + j, s * 1.18 + 1.5

    def meters(self):
        t = time.time() - self.t0
        v = self.values
        bpm = v.get("sys.tempo", 120)
        beat = (t * bpm / 60) % 1
        env = math.exp(-beat * 5)
        base = -17 + 5 * math.sin(t * .35) + 7 * env
        def ch(off):
            return base + off + random.uniform(-1.2, 1.2)
        play = [ch(0), ch(-1.5)]
        inl = -30 + v.get("in.gain", 0) + 6 * math.sin(t * .9) + random.uniform(-2, 2)
        inp = [inl, inl - 2 if v.get("in.mode", 0) == 0 else inl]
        gr = 0.0
        if v.get("cmp.on", 0) >= .5:
            over = max(play) - v.get("cmp.thresh", -12)
            gr = max(0.0, over * (1 - 1 / max(1, v.get("cmp.ratio", 2))))
        vol = v.get("out.volume", -20) + 20
        out = [p + vol - gr + (v.get("cmp.makeup", 0) if v.get("cmp.on", 0) >= .5 else 0) for p in play]
        lim = 0.0
        ceil = v.get("lim.ceiling", -.3)
        if v.get("lim.on", 1) >= .5:
            mx = max(out)
            if mx > ceil:
                lim = mx - ceil
                out = [o - lim for o in out]
        if v.get("out.mute", 0) >= .5:
            out = [-100, -100]
        r = lambda a: [round(max(-100, x), 1) for x in a]
        cpu = self.cpu()
        clip = int(t // 9) if v.get("in.gain", 0) > 18 else 0
        return {"in": r(inp), "out": r(out), "play": r(play), "clip": [clip, clip], "over": [0, 0],
                "gr": round(gr, 1), "lim": round(lim, 1), "cpu": round(cpu[0], 1), "cpu_max": round(cpu[1], 1)}

    def info(self):
        cpu = self.cpu()
        up = int(time.time() - self.t0) + 3 * 3600 + 17 * 60
        with self.lock:
            self.wifi_tick()
            w = dict(self.wifi, hostname="twofold.local", ap_ssid="TWOFOLD-7C3A" if self.wifi["mode"] in ("ap", "apsta") else "",
                     ap_ip="192.168.4.1" if self.wifi["mode"] in ("ap", "apsta") else "")
            w.pop("t", None)
        return {"device": "TWOFOLD", "fw": self.fw, "project": "twofold", "idf": "v5.5.1", "built": "Oct  3 2026",
                "built_time": "10:42:07", "sha": "3fa9c1e07b2d4e11",
                "ota": {"supported": True, "running": "ota_0", "next": "ota_1", "max_size": 0x1F0000,
                        "active": self.ota is not None, "pct": self.ota or 0},
                "mac": "34:85:18:A1:7C:3A", "chip_rev": 2, "uptime": up,
                "heap": {"int_free": 61344, "int_min": 48212, "int_largest": 31744, "psram_free": 1043221},
                "wifi": w,
                "audio": {"rate": 48000, "measured_fs": 48000.02, "usb_feedback": 48.0004, "xrun_play": 0, "xrun_rec": 0, "i2s_errors": 0},
                "cpu": {"avg": round(cpu[0], 1), "max": round(cpu[1], 1),
                        "stages": [{"name": n, "avg": round(x, 2), "max": round(x * 1.2, 2)} for n, x in self.stages()]},
                "status": self.status()}


DEV = None


# ------------------------------------------------------------------ HTTP / WebSocket
class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "TWOFOLD-mock"

    def log_message(self, fmt, *args):
        if "/api/meters" not in (args[0] if args else ""):
            super().log_message(fmt, *args)

    # -- responses
    def send(self, code, body=b"", ctype="application/json", extra=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, separators=(",", ":")).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if body and self.command != "HEAD":
            self.wfile.write(body)

    def err(self, code, msg):
        self.send(code, {"ok": False, "error": msg})

    def body_json(self, limit=8192):
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0 or n > limit:
            return None
        try:
            j = json.loads(self.rfile.read(n))
            return j if isinstance(j, dict) else None
        except ValueError:
            return None

    def rebooting(self):
        if time.time() < DEV.reboot_until:
            self.close_connection = True
            self.err(503, "rebooting")
            return True
        return False

    # -- GET
    def do_GET(self):
        u = urlparse(self.path)
        p = u.path
        if p == "/ws" and self.headers.get("Upgrade", "").lower() == "websocket":
            return self.websocket()
        if self.rebooting():
            return
        if p in ("/", "/index.html"):
            return self.send(200, WEB.read_bytes(), "text/html; charset=utf-8")
        if p == "/favicon.ico":
            return self.send(204)
        if p in ("/hotspot-detect.html", "/generate_204", "/connecttest.txt", "/ncsi.txt"):
            if DEV.wifi["mode"] in ("ap", "apsta"):
                return self.send(302, "<a href='/'>TWOFOLD setup</a>", "text/html", {"Location": "/"})
            return self.err(404, "not found")
        d = DEV
        if p == "/api/params":
            return self.send(200, {"fs": 48000, "count": len(d.params),
                                   "params": [{k: v for k, v in x.items() if k != "id"} for x in d.params]})
        if p == "/api/state":
            with d.lock:
                return self.send(200, {"seq": d.seq, "epoch": d.epoch, "values": dict(d.values), "status": d.status()})
        if p == "/api/meters":
            return self.send(200, d.meters())
        if p == "/api/info":
            return self.send(200, d.info())
        if p == "/api/presets":
            return self.send(200, self.presets())
        if p == "/api/preset/export":
            q = parse_qs(u.query)
            try:
                slot = int(q.get("slot", ["-1"])[0])
            except ValueError:
                slot = -1
            if not 0 <= slot < 8:
                return self.err(400, "slot must be 0..7")
            s = d.slots[slot]
            return self.send(200, {"format": "twofold-preset", "version": 1, "name": s["name"], "slot": slot,
                                   "fw": d.fw, "params": s["values"]})
        if p == "/api/ccmap":
            return self.send(200, self.ccmap())
        if p == "/api/wifi":
            info = d.info()["wifi"]
            return self.send(200, info)
        return self.err(404, "not found")

    def presets(self):
        d = DEV
        return {"active": d.active, "dirty": d.dirty, "pending": False,
                "slots": [{"slot": i, "name": s["name"]} for i, s in enumerate(d.slots)]}

    def ccmap(self):
        return {"ch": int(DEV.values.get("midi.ch", 0)), "map": dict(DEV.cc)}

    # -- POST
    def do_POST(self):
        p = urlparse(self.path).path
        if self.rebooting():
            return
        d = DEV
        if p == "/api/ota":
            return self.ota()
        j = self.body_json()
        if j is None:
            return self.err(400, "invalid JSON")
        if p == "/api/param":
            out = {}
            vals = j.get("values") if isinstance(j.get("values"), dict) else {j.get("key"): j.get("value")}
            for k, v in vals.items():
                if isinstance(v, bool):
                    v = float(v)
                if not isinstance(k, str) or k not in d.pk:
                    return self.err(404, "unknown parameter")
                if not isinstance(v, (int, float)):
                    return self.err(400, "bad parameter/value")
                out[k] = d.set(k, v)
            return self.send(200, {"ok": True, "values": out})
        if p.startswith("/api/preset/"):
            op = p.rsplit("/", 1)[1]
            slot = j.get("slot")
            if not isinstance(slot, int) or not 0 <= slot < 8:
                return self.err(400, "slot must be 0..7")
            name = re.sub(r"[^A-Z0-9 \-_.+/#&()!?:']", "", str(j.get("name") or "").upper()).strip()[:15]
            if op == "load":
                d.apply_preset(slot)
            elif op == "save":
                with d.lock:
                    d.slots[slot]["values"] = {k: v for k, v in d.values.items() if d.pk[k]["flags"] == "preset"}
                    if name:
                        d.slots[slot]["name"] = name
                    d.active, d.dirty = slot, False
                time.sleep(.05)
            elif op == "rename":
                if not name:
                    return self.err(400, "invalid name")
                d.slots[slot]["name"] = name
            elif op == "import":
                if j.get("format") not in (None, "twofold-preset") or not isinstance(j.get("params"), dict):
                    return self.err(400, "expected {slot, params:{...}} (format twofold-preset)")
                vals, used = {}, 0
                for k, desc in d.pk.items():
                    if desc["flags"] != "preset":
                        continue
                    v = j["params"].get(k)
                    if isinstance(v, (int, float)):
                        used += 1
                        vals[k] = d.quant(desc, v)
                    else:
                        vals[k] = desc["def"]
                d.slots[slot] = {"name": name or f"IMPORT {slot + 1}", "values": vals}
                return self.send(200, {"ok": True, "slot": slot, "used": used, "ignored": len(j["params"]) - used})
            else:
                return self.err(404, "not found")
            return self.send(200, self.presets())
        if p == "/api/ccmap":
            k, cc = j.get("key"), j.get("cc", -2)
            if k not in d.pk:
                return self.err(404, "unknown parameter")
            if cc is None or cc == -1:
                cc = None
            elif not isinstance(cc, int) or not 0 <= cc <= 127:
                return self.err(400, "cc must be 0..127 or null")
            if cc is not None:
                for kk in d.cc:
                    if d.cc[kk] == cc:
                        d.cc[kk] = None
            d.cc[k] = cc
            return self.send(200, self.ccmap())
        if p == "/api/ccmap/reset":
            d.cc = {x["key"]: x["cc"] for x in d.params}
            return self.send(200, self.ccmap())
        if p == "/api/ccmap/save":
            return self.send(200, {"ok": True})
        if p == "/api/wifi/scan":
            d.wifi.update(scanning=True, t=time.time())
            return self.send(200, {"ok": True})
        if p == "/api/wifi/connect":
            ssid, pw = j.get("ssid"), j.get("pass", "")
            if not isinstance(ssid, str) or not 1 <= len(ssid.encode()) <= 32 or not isinstance(pw, str) or (pw and not 8 <= len(pw) <= 64):
                return self.err(400, "SSID 1-32 bytes, password empty or 8-64 characters")
            d.wifi.update(state="connecting", ssid=ssid, mode="apsta" if d.wifi["mode"] != "sta" else "sta", t=time.time(), error="")
            return self.send(200, {"ok": True})
        if p == "/api/wifi/forget":
            d.wifi.update(state="setup", mode="ap", ssid="", ip="", saved=False, error="network forgotten")
            return self.send(200, {"ok": True})
        if p == "/api/cmd":
            c = j.get("cmd")
            if c in ("studio", "wifi_off"):
                print("mock: Studio Mode requested (WiFi would turn off now)")
            elif c == "reboot":
                d.reboot_until = time.time() + 4
            elif c == "factory_reset":
                d.slots = json.loads(json.dumps(d.factory))
                d.cc = {x["key"]: x["cc"] for x in d.params}
                d.apply_preset(0)
            elif c == "ble":
                d.set("net.ble", 1 if j.get("arg") else 0)
            elif c not in ("flush", "wifi_setup"):
                return self.err(400, "unknown cmd")
            return self.send(200, {"ok": True})
        return self.err(404, "not found")

    def ota(self):
        d = DEV
        n = int(self.headers.get("Content-Length") or 0)
        if n < 288:
            return self.err(400, "file too small to be a firmware image")
        if n > 0x1F0000:
            return self.err(413, "image larger than the OTA partition")
        first = self.rfile.read(min(n, 4096))
        if first[0] != 0xE9:
            self.rfile.read(n - len(first))
            return self.err(400, "not an ESP firmware image")
        got = len(first)
        d.ota = 0
        while got < n:
            chunk = self.rfile.read(min(16384, n - got))
            if not chunk:
                d.ota = None
                return
            got += len(chunk)
            d.ota = got * 100 // n
            time.sleep(.01)
        d.ota = None
        major, minor, patch = (d.fw.split(".") + ["0", "0"])[:3]
        d.fw = f"{major}.{minor}.{int(patch) + 1}"
        d.reboot_until = time.time() + 4
        return self.send(200, {"ok": True, "version": d.fw, "rebooting": True})

    # -- WebSocket (server side of RFC 6455: unfragmented text frames, ping/pong, close)
    def websocket(self):
        key = self.headers.get("Sec-WebSocket-Key", "")
        acc = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", acc)
        self.end_headers()
        self.wfile.flush()
        sock = self.connection
        d = DEV
        last = d.seq
        tick, nxt = 0, time.time()
        try:
            while True:
                tmo = max(0.0, nxt - time.time())
                r, _, _ = select.select([sock], [], [], tmo)
                if r and not self.ws_read(sock):
                    break
                if time.time() >= nxt:
                    nxt += 1 / 15
                    if time.time() < d.reboot_until:
                        break
                    with d.lock:
                        ch = {k: d.values[k] for k, s in d.changed.items() if s > last}
                        last = d.seq
                    msg = {"t": "u", "m": d.meters()}
                    if ch:
                        msg["p"] = ch
                    if tick % 15 == 0:
                        msg["s"] = d.status()
                    tick += 1
                    self.ws_send(sock, json.dumps(msg, separators=(",", ":")).encode())
        except (OSError, ConnectionError):
            pass
        self.close_connection = True

    @staticmethod
    def recv_exact(sock, n):
        b = b""
        while len(b) < n:
            c = sock.recv(n - len(b))
            if not c:
                raise ConnectionError
            b += c
        return b

    def ws_read(self, sock):
        h = self.recv_exact(sock, 2)
        op, ln = h[0] & 0x0F, h[1] & 0x7F
        if ln == 126:
            ln = struct.unpack(">H", self.recv_exact(sock, 2))[0]
        elif ln == 127:
            ln = struct.unpack(">Q", self.recv_exact(sock, 8))[0]
        mask = self.recv_exact(sock, 4) if h[1] & 0x80 else b"\0\0\0\0"
        if ln > 65536:
            return False
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(self.recv_exact(sock, ln)))
        if op == 8:
            return False
        if op == 9:
            self.ws_send(sock, data, 0xA)
        if op == 1:
            try:
                m = json.loads(data)
            except ValueError:
                return True
            if isinstance(m, dict) and m.get("t") == "set":
                vals = m.get("values") if isinstance(m.get("values"), dict) else {m.get("k"): m.get("v")}
                for k, v in vals.items():
                    if isinstance(k, str) and isinstance(v, (int, float)):
                        DEV.set(k, float(v))
        return True

    @staticmethod
    def ws_send(sock, data, op=1):
        n = len(data)
        hdr = bytes([0x80 | op, n]) if n < 126 else (bytes([0x80 | op, 126]) + struct.pack(">H", n) if n < 65536
                                                    else bytes([0x80 | op, 127]) + struct.pack(">Q", n))
        sock.sendall(hdr + data)


def main():
    global DEV
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8817)
    ap.add_argument("--ap", action="store_true", help="start in WiFi setup (SoftAP) mode")
    a = ap.parse_args()
    DEV = Device(a.ap)
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    srv.daemon_threads = True
    print(f"TWOFOLD mock: http://{a.host}:{a.port}/  ({len(DEV.params)} params, {'AP setup' if a.ap else 'station'} mode)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
