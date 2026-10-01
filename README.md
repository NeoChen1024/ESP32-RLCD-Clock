# RLCD Time Scale Monitor

A Waveshare ESP32-S3 RLCD 4.2 based **time-scale instrument** — a reflective
monochrome 400×300 display showing local time, UTC, MJD(TAI), GPS week/TOW,
ISO week date, sync state and telemetry, in a fixed-width instrument aesthetic.

Time comes from Wi-Fi SNTP, with ESP32 system-clock holdover and the
PCF85063A RTC. There is no GNSS or PPS. A three-state trust model
(INVALID / TRUSTED / RTC_HOLD) decides what the face shows; see
[docs/time.md](docs/time.md).

## Repository layout

```
AGENTS.md                                            env, hardware facts, bring-up gotchas
rlcd_time_scale_monitor_implementation_notes.md      active progress and agreed scope
common/                                              shared pure-C render, clock and storage code
  time_model.{h,c} render_faces.{h,c} frame_export.{h,c} display_geometry.h
  clock_health.{h,c} sensor_health.{h,c} storage_files.{h,c}
  tz_rule leap_table sha1 wav_format audio_source (WAV/FLAC decoding)
host/                                                host-first simulator (SDL3 + u8g2)
  src/    host platform code only: main, host_time glue, SDL3 backend
  tests/  thirteen host tests, including sensor freshness, RTC policy, TZ/leap parsing, WAV/FLAC decoding, Wi-Fi secrets and config selection
firmware/                                            ESP-IDF v6.0.2 target firmware
  main/       app, CLI, Wi-Fi/SNTP/HTTP, storage/config, display, sensors, audio
  components/ u8g2 (submodule) + u8g2_st7305 SPI backend
u8g2/                                                u8g2 submodule (drawing engine)
contrib/dr_libs/                                     dr_libs submodule (dr_flac FLAC decoder)
docs/                                                architecture overview and subsystem contracts
```

Fetch both submodules before building: `git submodule update --init`.

The render path is shared: `common/time_model.c`, `render_faces.c` and
`frame_export.c` are compiled **verbatim** into both the host simulator and
the firmware (the platform hook `time_model_now()` lives in per-platform
glue). u8g2 is the only drawing engine; SDL3 only presents
the final 1-bit framebuffer.

## Host simulator

The host simulator is the active development surface for layout and
time-scale math.

```sh
cmake -S host -B host/build -DCMAKE_BUILD_TYPE=Release
cmake --build host/build -j
ctest --test-dir host/build --output-on-failure   # thirteen host tests
host/build/rlcd_host                              # window (15 Hz cap, nearest-neighbor)
host/build/rlcd_host --scale 2
host/build/rlcd_host --pbm out.pbm                # headless single-frame dump
host/build/rlcd_host --bmp out.bmp
host/build/rlcd_host --png out.png
```

See `host/README.md` for simulator details and the design notes for the full
spec.

## Firmware (ESP32-S3 RLCD)

ESP-IDF v6.0.2, a standalone `esp32s3` project in `firmware/`. The flash
holds a 4 MiB factory app and an 8 MiB FAT partition, with no OTA slots
([storage](docs/storage.md#volumes)).

```sh
source /opt/esp-idf/export.sh
cd firmware
idf.py -p /dev/ttyACM0 build flash  # replace with the port found on this host
```

First-time setup on either volume, over HTTP or by copying files onto the
SD card:

- **Known Wi-Fi networks:** copy `wifi.json.example` to `secrets/wifi.json`
  ([network](docs/network.md#known-networks)).
- **Optional config:** copy `config.json.example` to
  `config/<UTC timestamp>.json` for the time zone, NTP server and audio
  volume ([storage](docs/storage.md#configuration-versions)).
- **Optional leap table:** an IERS `leap-seconds.list` as
  `time/leap-seconds.list` ([time](docs/time.md#time-scales)).

Real `config.json`, `wifi.json`, `config/` and `secrets/` are git-ignored.
Until Wi-Fi is configured, use `wifi connect` on the serial console.

### Serial CLI (USB-Serial/JTAG)

| Command | Purpose |
| --- | --- |
| `wifi status \| connect "<ssid>" [pw] \| disconnect \| reconnect \| reset` | [Wi-Fi modes](docs/network.md#wi-fi-modes) |
| `ntp status \| server <host> \| reset \| resync` | [SNTP sources](docs/time.md#time-sources) and time state |
| `rtc status` | [RTC checks](docs/time.md#rtc) |
| `tz [<POSIX rule> \| ±HH:MM \| minutes \| reset]` | [Local time](docs/time.md#local-time) |
| `leap [status] \| reload` | [Leap table](docs/time.md#time-scales) |
| `config [status] \| reload \| cleanup [sd\|flash] [confirm]` | [Config versions](docs/storage.md#configuration-versions) |
| `audio play [sd\|flash] "<file>" [loop] \| stop \| volume [0-100\|reset] \| status` | [Playback](docs/audio.md) |
| `sd status \| mount \| unmount \| ls \| cat \| test \| format` | SD card (`format` erases it; unmount before removal) |
| `flash status \| mount \| init` | Internal FAT |
| `sensor`, `http` | Telemetry readout, HTTP endpoints |

### HTTP (port 80)

The homepage `/`, the file manager `/files`, `/status` and framebuffer
snapshots are served directly. The file and audio APIs:

```sh
curl http://<device>/status
curl -T config.json http://<device>/fs/flash/config/20260923T120000000Z.json
curl -T wifi.json   http://<device>/fs/flash/secrets/wifi.json   # write-only
curl -T alarm.flac  http://<device>/fs/sd/sounds/alarm.flac
curl -T leap-seconds.list http://<device>/fs/sd/time/leap-seconds.list
curl http://<device>/fs/sd/cleanup                               # preview
curl -X POST http://<device>/fs/sd/cleanup                       # delete older versions
curl -X POST -d '{"file":"alarm.flac","loop":true}' http://<device>/audio/play
curl -X POST http://<device>/audio/stop
curl -X POST -d '{"level":70}' http://<device>/audio/volume       # or {"reset":true}
curl -X DELETE http://<device>/fs/sd/sounds/alarm.flac
```

Endpoints are listed in [network](docs/network.md#http-endpoints). File
rules, limits and deadlines are in
[storage](docs/storage.md#http-file-api), and the playback API in
[audio](docs/audio.md#control). The API is unauthenticated; use it on a
trusted LAN only.

## Project documentation

- [Implementation notes](rlcd_time_scale_monitor_implementation_notes.md):
  active progress, remaining work and agreed design boundaries.
- [Architecture](docs/architecture.md): overview of shared code, tasks,
  locks and data flow, with links to the subsystem contracts:
  - [time](docs/time.md)
  - [storage](docs/storage.md)
  - [audio](docs/audio.md)
  - [network](docs/network.md)
  - [events](docs/events.md)
- [Hardware notes](docs/hardware_notes.md): board wiring, panel behavior
  and RTC power boundary.
- [AGENTS.md](AGENTS.md): the local bring-up runbook.

## Status

- **Host simulator**: working — single face renders all time-scale fields
  from the system clock, sync/Wi-Fi/battery states exercisable from the
  keyboard, headless PBM/BMP/PNG export, 15 Hz frame cap, thirteen CTest targets.
- **Firmware**: working — full UI ported to the panel and verified on
  hardware (time/sensors/telemetry all live), Wi-Fi + SNTP + HTTP +
  CLI bring-up complete, 1 Hz second-aligned refresh.
- **Not yet**: scheduled events ([design](docs/events.md)),
  leap-table status on the face, RTC drift calibration, custom icon fonts,
  low-battery visual polish —
  see [remaining work](rlcd_time_scale_monitor_implementation_notes.md#remaining-work).
