# RLCD Time Scale Monitor

A Waveshare ESP32-S3 RLCD 4.2 based **time-scale instrument** — a reflective
monochrome 400×300 display showing local time, UTC, MJD(TAI), GPS week/TOW,
ISO week date, sync state and telemetry, in a fixed-width instrument aesthetic.

Primary time source is Wi-Fi SNTP. After a successful sync, the ESP32 system
clock provides bounded holdover. The PCF85063A RTC can restore trusted time
across a reboot while the last verified SNTP sync is less than 24 hours old.
No GNSS or PPS. TAI−UTC comes from an uploaded IERS `leap-seconds.list` when
one verifies, otherwise from a built-in current-era value.

## Repository layout

```
AGENTS.md                                            env, hardware facts, bring-up gotchas
rlcd_time_scale_monitor_implementation_notes.md      active progress and agreed scope
common/                                              shared pure-C render, clock and storage code
  time_model.{h,c} render_faces.{h,c} frame_export.{h,c} display_geometry.h
  clock_health.{h,c} sensor_health.{h,c} storage_files.{h,c}
host/                                                host-first simulator (SDL3 + u8g2)
  src/    host platform code only: main, host_time glue, SDL3 backend
  tests/  eleven host tests, including sensor freshness, RTC policy, TZ/leap parsing, Wi-Fi secrets and config selection
firmware/                                            ESP-IDF v6.0.2 target firmware
  main/       app, CLI, Wi-Fi/SNTP/HTTP, storage/config, display, sensors
  components/ u8g2 (submodule) + u8g2_st7305 SPI backend
u8g2/                                                u8g2 submodule (drawing engine)
docs/                                                architecture, hardware notes and schematic
```

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
ctest --test-dir host/build --output-on-failure   # eleven host tests
host/build/rlcd_host                              # window (15 Hz cap, nearest-neighbor)
host/build/rlcd_host --scale 2
host/build/rlcd_host --pbm out.pbm                # headless single-frame dump
host/build/rlcd_host --bmp out.bmp
host/build/rlcd_host --png out.png
```

See `host/README.md` for simulator details and the design notes for the full
spec.

## Firmware (ESP32-S3 RLCD)

ESP-IDF v6.0.2 firmware — Wi-Fi/SNTP/HTTP server + serial CLI. Known Wi-Fi
networks come from a write-only `secrets/wifi.json`. Neither they nor CLI
overrides are stored in NVS (`WIFI_STORAGE_RAM`). A small NVS record stores
the last verified SNTP checkpoint for RTC trust, not user config.
Versioned settings on SD or internal flash persist across reboot;
without a usable config, TZ defaults to UTC+8 (`<+08>-8`).

Flash layout is defined in `firmware/partitions.csv`: one 4 MiB factory app
and an 8 MiB FAT data partition on the 16 MiB flash, with NVS/PHY retained.
The internal filesystem mounts at `/flash` with 4096-byte-sector wear levelling;
SD mounts at `/sdcard`. `flash init` explicitly initializes an unmountable
internal filesystem; ordinary boot mounts never auto-format.

Build/flash:

```sh
source /opt/esp-idf/export.sh
cd firmware
idf.py -p /dev/ttyACM0 build flash  # replace with the port found on this host
```

- **Display**: the full single face on the ST7305 panel via the vendor
  `u8g2_st7305` SPI backend, refreshed 1 Hz aligned to the wall-clock second
  boundary (self-heals across SNTP steps).
- **CLI** (USB-Serial/JTAG console): `wifi status | reconnect | reset`,
  `wifi connect "<ssid>" [password]` (manual until `wifi reset`),
  `wifi disconnect` (auto connection paused until `wifi reset`),
  `ntp status | ntp server <host> | ntp reset`,
  `tz [<POSIX rule>|±HH:MM|minutes|reset]`, `leap status | reload`,
  `config status | reload | cleanup [sd|flash] [confirm]`, `rtc status`, `sensor`, `sd`,
  `flash status | mount | init`, `http status`. `linenoise`
  runs in dumb mode for the USB VFS.
- **Sensors**: SHTC3 temperature/humidity (I2C, CRC-8 checked) + battery
  voltage (ADC1 CH3, ×3 divider). Device RSSI is real
  (`esp_wifi_sta_get_ap_info`); values flow into the model each frame.
  One-off SHTC3 failures retain the last sample for up to 10 seconds; after
  that, temperature/humidity show `n/a` until a new valid reading. Battery
  ADC failure shows `n/a` immediately instead of a misleading 0 V.
- **RTC holdover**: PCF85063A on the shared I²C bus is updated after SNTP
  synchronization. A boot read is accepted only when its oscillator-stop flag
  is clear, its calendar and RAM marker are valid, and its time is less than
  24 hours after the NVS last-sync checkpoint. `rtc status` shows these checks.
  A valid RTC boot is trusted holdover, not a fresh SNTP synchronization;
  invalid or stale RTC data leaves time fields masked until SNTP succeeds.
- **SD card**: FAT on 1-bit SDMMC (CLK=38 CMD=21 D0=39, 20 MHz), mounted
  at `/sdcard` without auto-formatting. CLI: `sd status`, `sd ls [dir]`,
  `sd cat <file>` (4 KiB preview), `sd test` (temporary-file round trip),
  `sd unmount` / `sd mount`, and destructive `sd format` (16 KiB clusters).
  Paths are relative to `/sdcard`. Unmount before removing the card;
  automatic hotplug is not implemented. Both storage volumes are managed at
  `http://<device>/files` and through GET/PUT/DELETE under `/fs/flash/`
  and `/fs/sd/` (versioned JSON configs, WAV sounds, the leap table and
  write-only Wi-Fi secrets).
- **HTTP server** (:80): English homepage `/`, English file manager `/files`,
  status `/status`, active config `/fs/active`, and `/snapshot.pbm` and
  `/snapshot.bmp`. Snapshots encode via the shared `frame_export`, so
  host↔target exports are byte-comparable.
- **NTP**: manual `ntp server` CLI > selected config `ntp_server` (SD first,
  then internal flash) > DHCP option 42 > pool.ntp.org, with a watchdog that reverts to the pool if
  the DHCP-provided server cannot sync within 18 s, and retries DHCP every
  5 minutes. Last-good trust survives resync; age uses monotonic time, with
  2 h freshness and 24 h maximum holdover. Stays on the lwIP SNTP
  client — full NTP/xleave is outside the agreed scope (see
  [implementation notes](rlcd_time_scale_monitor_implementation_notes.md)).

## Project documentation

- [Implementation notes](rlcd_time_scale_monitor_implementation_notes.md):
  active progress, remaining work and agreed design boundaries.
- [Architecture](docs/architecture.md): current data flow, time trust,
  rendering and storage contracts.
- [Hardware notes](docs/hardware_notes.md): board wiring, panel behavior
  and RTC power boundary; [AGENTS.md](AGENTS.md) is the local bring-up runbook.

## Status

- **Host simulator**: working — single face renders all time-scale fields
  from the system clock, sync/Wi-Fi/battery states exercisable from the
  keyboard, headless PBM/BMP/PNG export, 15 Hz frame cap, eleven CTest targets.
- **Firmware**: working — full UI ported to the panel and verified on
  hardware (time/sensors/telemetry all live), Wi-Fi + SNTP + HTTP +
  CLI bring-up complete, 1 Hz second-aligned refresh.
- **Not yet**: alarm scheduling and audio playback (including FLAC),
  leap-table status on the face, RTC drift calibration, custom icon fonts,
  low-battery visual polish —
  see [remaining work](rlcd_time_scale_monitor_implementation_notes.md#remaining-work).

## HTTP file management

`GET /files` opens the responsive file manager (volume selection, directory
listing, multi-file upload/drop, download, delete and versioned JSON editing).
The homepage at `/` uses the same style. `GET /fs/` lists mounted volumes and
free space; `GET /fs/active` reports the selected config, Wi-Fi secrets source
and leap table. File API examples:

```sh
curl http://<device>/fs/flash/
curl -T config.json http://<device>/fs/flash/config/20260923T120000000Z.json
curl -T alarm.wav http://<device>/fs/sd/sounds/alarm.wav
curl -T leap-seconds.list http://<device>/fs/sd/time/leap-seconds.list
curl -T wifi.json http://<device>/fs/flash/secrets/wifi.json   # write-only
curl http://<device>/fs/sd/cleanup                             # preview
curl -X POST http://<device>/fs/sd/cleanup                     # delete older versions
curl -o saved.json http://<device>/fs/flash/config/20260923T120000000Z.json
curl -X DELETE http://<device>/fs/sd/sounds/alarm.wav
```

Managed files are `config/<ASCII name>.json` (JSON objects, max 16 KiB,
max nesting 16), `sounds/<ASCII name>.wav` (RIFF/WAVE container with
matching length, max 16 MiB and available space permitting) and
`time/leap-seconds.list` (IERS format with a matching SHA-1 line, max 16 KiB;
replaced in place) and `secrets/wifi.json` (known networks; replaced in place,
never downloadable). Config versions
are read in descending filename order: the first valid SD version wins; if
none is usable, the first valid internal-flash version wins; otherwise the
defaults apply. SD takes priority even when a Flash filename sorts later.
Supported keys are `tz` (POSIX TZ rule; offsets west of UTC as in POSIX),
the legacy `tz_offset_minutes` (integer -840..840 east of UTC, used when `tz`
is absent) and optional `ntp_server` (hostname or IPv4 address). See
[architecture](docs/architecture.md#storage-and-configuration) for the rule
limits. For example:

```json
{"tz": "CST-8", "ntp_server": "pool.ntp.org"}
```

Start from the repository examples: copy `config.json.example` to
`config/<UTC timestamp>.json` and `wifi.json.example` to
`secrets/wifi.json` on either volume, or upload them as shown above.
`config.json`, `wifi.json`, `config/` and `secrets/` are git-ignored so real
settings stay out of the repository. Cleanup keeps each volume's selected
version and anything newer, and deletes only older versions; see
[architecture](docs/architecture.md#storage-and-configuration).

CLI timezone and NTP overrides take priority until reset. Uploading a config
requires a new filename
(409 if already present); the web editor creates a timestamped version and
retains older files. Invalid config returns 422. WAV playback and alarm
configuration are still pending. Paths are case-sensitive at the API;
percent-encode spaces, and use no query parameters. Root, `config/`,
`sounds/`, `time/` and `secrets/` listings return JSON; file listings show at most 256 entries.

Uploads use Content-Length and a 120 s deadline; downloads also have a
120 s deadline. A storage worker handles one transfer at a time, with one
additional request queued, keeping `/status` and snapshots responsive.
Serial unmount/format waits for storage ownership. A private `.rlcd-txn`
journal stages replacements and recovers interrupted rename sequences;
this is not a guarantee against FAT metadata damage on power loss. Do not
modify that directory. No HTTP formatting endpoint is exposed.
