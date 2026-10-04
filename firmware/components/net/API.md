# TWOFOLD web API

Served by `esp_http_server` while WiFi is on (`http://twofold.local/` in station mode, `http://192.168.4.1/`
in setup AP mode). All bodies are JSON unless noted. Parameter keys are the stable `key` strings from
[`params_table.h`](../params/include/params_table.h). All inputs are length- and range-checked, and values are
clamped and quantised by `params_set()`.

## REST
| method | path | request | response / effect |
|---|---|---|---|
| GET | `/api/info` | | device, firmware version, IDF version, build date/sha, OTA partitions (running / next / max size / progress) |
| GET | `/api/params` | | `{fs, count, params:[{key,name,unit,type,curve,min,max,def,step,flags,cc,enum?,value}]}`: the UI is built from this |
| GET | `/api/state` | | all parameter values + status (usb/play/rec, preset, pending writes, wifi/ble, ota, total CPU avg/max) |
| GET | `/api/meters` | | `in/out/play` peaks (dBFS), clip / over counters, comp + limiter GR, CPU |
| POST | `/api/param` | `{key, value}` or `{values:{key:value,…}}` | sets parameters (source = WEB) |
| GET | `/api/presets` | | 8 slots `{slot, name}`, active slot, dirty flag |
| POST | `/api/preset/load` | `{slot}` | crossfaded load |
| POST | `/api/preset/save` | `{slot, name?}` | current sound → slot (brief soft-mute while flash is written) |
| POST | `/api/preset/rename` | `{slot, name}` | |
| GET | `/api/preset/export?slot=N` | | preset as JSON `{name, values:{key:value}}` |
| POST | `/api/preset/import` | `{slot, name, values:{…}}` | unknown keys ignored |
| GET / POST | `/api/ccmap` | POST: `{key, cc}` (cc 0–127, or null to unmap) | MIDI channel + `map:{key:cc}` |
| POST | `/api/ccmap/reset`, `/api/ccmap/save` | | defaults / persist (soft-muted write) |
| GET | `/api/wifi` | | mode, ssid, ip, rssi, scan results |
| POST | `/api/wifi/scan` | | start a scan (results via GET `/api/wifi`) |
| POST | `/api/wifi/connect` | `{ssid, pass}` | save credentials (soft-muted) and switch to station mode |
| POST | `/api/wifi/forget` | | erase credentials |
| POST | `/api/cmd` | `{cmd, arg?}` | `studio` / `wifi_off` (radios off), `wifi_setup`, `ble` (arg 0/1), `flush` (commit pending settings), `reboot`, `factory_reset` |
| POST | `/api/ota` | raw `.bin` (`application/octet-stream`) | streamed to the inactive OTA slot. Audio is soft-muted throughout; validated, boot partition switched, reboot |

Captive-portal probes (`/hotspot-detect.html`, `/generate_204`, `/connecttest.txt`, `/ncsi.txt`, `/redirect`,
`/canonical.html`, `/success.txt`, …) redirect to the setup page while in AP mode. A built-in DNS server answers
every query with the AP address.

## WebSocket `/ws`
* client → device: `{"t":"set","k":"eq.b2.gain","v":3.5}` or `{"t":"set","values":{…}}`
* device → client, ~15 Hz: `{"t":"u","m":{meters…},"p":{changed params…},"s":{status…}}`. Status is sent at 1 Hz
  or on change. One frame is in flight at a time, so a slow client delays updates but never loses parameter changes.
