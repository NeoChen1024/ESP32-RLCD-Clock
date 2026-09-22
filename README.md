# RLCD Time Scale Monitor

A Waveshare ESP32-S3 RLCD 4.2 based **time-scale instrument** — a reflective
monochrome 400×300 display showing local time, UTC, MJD(TAI), GPS week/TOW,
ISO week date, sync state and telemetry, in a fixed-width instrument aesthetic.

Primary time source is Wi-Fi SNTP. After a successful sync, the ESP32 system
clock provides bounded holdover. The PCF85063A RTC can restore trusted time
across a reboot while the last verified SNTP sync is less than 24 hours old.
No GNSS, no PPS, no leap-second historical table.

## Repository layout

```
AGENTS.md                                            env, hardware facts, bring-up gotchas
rlcd_time_scale_monitor_implementation_notes.md      design reference (current status)
common/                                              shared pure-C render, clock and storage code
  time_model.{h,c} render_faces.{h,c} frame_export.{h,c} display_geometry.h
  clock_health.{h,c} storage_files.{h,c}
host/                                                host-first simulator (SDL3 + u8g2)
  src/    host platform code only: main, host_time glue, SDL3 backend
  tests/  eight host tests, including RTC policy and config selection
firmware/                                            ESP-IDF v6.0.2 target firmware
  main/       app, CLI, Wi-Fi/SNTP/HTTP, storage/config, display, sensors
  components/ u8g2 (submodule) + u8g2_st7305 SPI backend
u8g2/                                                u8g2 submodule (drawing engine)
docs/                                                schematics
```

The render path is shared: `common/time_model.c`, `render_faces.c` and
`frame_export.c` are compiled **verbatim** into both the host simulator and
the firmware (platform hooks `time_model_now()` / `tz_offset_minutes()` live
in per-platform glue). u8g2 is the only drawing engine; SDL3 only presents
the final 1-bit framebuffer.

## Host simulator

The host simulator is the active development surface for layout and
time-scale math.

```sh
cmake -S host -B host/build -DCMAKE_BUILD_TYPE=Release
cmake --build host/build -j
ctest --test-dir host/build --output-on-failure   # eight host tests
host/build/rlcd_host                              # window (15 Hz cap, nearest-neighbor)
host/build/rlcd_host --scale 2
host/build/rlcd_host --pbm out.pbm                # headless single-frame dump
host/build/rlcd_host --bmp out.bmp
host/build/rlcd_host --png out.png
```

See `host/README.md` for simulator details and the design notes for the full
spec.

## Firmware (ESP32-S3 RLCD)

ESP-IDF v6.0.2 firmware — Wi-Fi/SNTP/HTTP server + serial CLI. Wi-Fi
credentials and CLI overrides are RAM-only (WIFI_STORAGE_RAM). A small NVS
record stores the last verified SNTP checkpoint for RTC trust, not user config.
Versioned settings on SD or internal flash persist across reboot;
without a usable config, TZ defaults to UTC+8.

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
- **CLI** (USB-Serial/JTAG console): `wifi connect "<ssid>" [password]`,
  `wifi reconnect`, `ntp status | ntp server <host> | ntp reset`,
  `tz [±HH:MM|minutes|reset]`, `config status | reload`, `rtc status`, `sensor`, `sd`,
  `flash status | mount | init`, `http status`. `linenoise`
  runs in dumb mode for the USB VFS.
- **Sensors**: SHTC3 temperature/humidity (I2C, CRC-8 checked) + battery
  voltage (ADC1 CH3, ×3 divider). Device RSSI is real
  (`esp_wifi_sta_get_ap_info`); values flow into the model each frame.
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
  and `/fs/sd/` (versioned JSON configs and WAV sounds).
- **HTTP server** (:80): English homepage `/`, English file manager `/files`,
  status `/status`, active config `/fs/active`, and `/snapshot.pbm` and
  `/snapshot.bmp`. Snapshots encode via the shared `frame_export`, so
  host↔target exports are byte-comparable.
- **NTP**: manual `ntp server` CLI > selected config `ntp_server` (SD first,
  then internal flash) > DHCP option 42 > pool.ntp.org, with a watchdog that reverts to the pool if
  the DHCP-provided server cannot sync within 18 s, and retries DHCP every
  5 minutes. Last-good trust survives resync; age uses monotonic time, with
  2 h freshness and 24 h maximum holdover. Stays on the lwIP SNTP
  client — full NTP/xleave is a recorded non-goal (see design notes §11).

## Design reference

`rlcd_time_scale_monitor_implementation_notes.md` covers hardware
constraints, the single-face display layout, time-scale derivations
(integer-only MJD-TAI / GPS week-TOW / ISO week), the graphics stack
decision (u8g2, no LVGL), firmware architecture and milestones. `AGENTS.md`
is the operational reference (environment, pin map, hardware gotchas).

## Status

- **Host simulator**: working — single face renders all time-scale fields
  from the system clock, sync/Wi-Fi/battery states exercisable from the
  keyboard, headless PBM/BMP/PNG export, 15 Hz frame cap, eight CTest targets.
- **Firmware**: working — full UI ported to the panel and verified on
  hardware (time/sensors/telemetry all live), Wi-Fi + SNTP + HTTP +
  CLI bring-up complete, 1 Hz second-aligned refresh.
- **Not yet**: alarm scheduling and audio playback (including FLAC),
  time-scale offset config, RTC drift calibration, custom icon fonts,
  low-battery visual polish —
  see milestone status in the design notes.

## HTTP file management

`GET /files` opens the responsive file manager (volume selection, directory
listing, multi-file upload/drop, download, delete and versioned JSON editing).
The homepage at `/` uses the same style. `GET /fs/` lists mounted volumes and
free space; `GET /fs/active` reports the selected config. File API examples:

```sh
curl http://<device>/fs/flash/
curl -T config.json http://<device>/fs/flash/config/20260923T120000000Z.json
curl -T alarm.wav http://<device>/fs/sd/sounds/alarm.wav
curl -o saved.json http://<device>/fs/flash/config/20260923T120000000Z.json
curl -X DELETE http://<device>/fs/sd/sounds/alarm.wav
```

Managed files are `config/<ASCII name>.json` (JSON objects, max 16 KiB,
max nesting 16) and `sounds/<ASCII name>.wav` (RIFF/WAVE container with
matching length, max 16 MiB and available space permitting). Config versions
are read in descending filename order: the first valid SD version wins; if
none is usable, the first valid internal-flash version wins; otherwise the
defaults apply. SD takes priority even when a Flash filename sorts later.
Supported keys are `tz_offset_minutes` (integer -840..840; default +480) and
optional `ntp_server` (hostname or IPv4 address). For example:

```json
{"tz_offset_minutes": 480, "ntp_server": "pool.ntp.org"}
```

CLI timezone and NTP overrides take priority until reset. Uploading a config
requires a new filename
(409 if already present); the web editor creates a timestamped version and
retains older files. Invalid config returns 422. WAV playback and alarm
configuration are still pending. Paths are case-sensitive at the API;
percent-encode spaces, and use no query parameters. Root, `config/` and
`sounds/` listings return JSON; file listings show at most 256 entries.

Uploads use Content-Length and a 120 s deadline; downloads also have a
120 s deadline. A storage worker handles one transfer at a time, with one
additional request queued, keeping `/status` and snapshots responsive.
Serial unmount/format waits for storage ownership. A private `.rlcd-txn`
journal stages replacements and recovers interrupted rename sequences;
this is not a guarantee against FAT metadata damage on power loss. Do not
modify that directory. No HTTP formatting endpoint is exposed.
