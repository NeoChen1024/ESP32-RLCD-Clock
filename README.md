# RLCD Time Scale Monitor

A Waveshare ESP32-S3 RLCD 4.2 based **time-scale instrument** — a reflective
monochrome 400×300 display showing local time, UTC, MJD(TAI), GPS week/TOW,
ISO week date, sync state and telemetry, in a fixed-width instrument aesthetic.

Primary time source is Wi-Fi SNTP/NTP, with PCF85063 RTC / ESP32 system time
as holdover. No GNSS, no PPS, no leap-second historical table.

## Repository layout

```
AGENTS.md                                            env, hardware facts, bring-up gotchas
rlcd_time_scale_monitor_implementation_notes.md      design reference (current status)
common/                                              shared pure-C render path
  time_model.{h,c} render_faces.{h,c} frame_export.{h,c} display_geometry.h
host/                                                host-first simulator (SDL3 + u8g2)
  src/    host platform code only: main, host_time glue, SDL3 backend
  tests/  offline unit tests (time model, frame export)
firmware/                                            ESP-IDF v6.0.2 target firmware
  main/       app, CLI, Wi-Fi/SNTP/HTTP, display task, sensors, model glue
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
ctest --test-dir host/build --output-on-failure   # time-model + frame-export tests
host/build/rlcd_host                              # window (15 Hz cap, nearest-neighbor)
host/build/rlcd_host --scale 2
host/build/rlcd_host --pbm out.pbm                # headless single-frame dump
host/build/rlcd_host --bmp out.bmp
host/build/rlcd_host --png out.png
```

See `host/README.md` for simulator details and the design notes for the full
spec.

## Firmware (ESP32-S3 RLCD)

ESP-IDF v6.0.2 bring-up firmware — Wi-Fi/SNTP/HTTP server + serial CLI, with
**non-persistent settings** (WIFI_STORAGE_RAM; nothing written to NVS; TZ
defaults to UTC+8 at boot). Build/flash:

```sh
source /opt/esp-idf/export.sh
cd firmware
idf.py -p /dev/ttyACM1 build flash
```

- **Display**: the full single face on the ST7305 panel via the vendor
  `u8g2_st7305` SPI backend, refreshed 1 Hz aligned to the wall-clock second
  boundary (self-heals across SNTP steps).
- **CLI** (USB-Serial/JTAG console): `wifi connect "<ssid>" [password]`,
  `ntp status | ntp server <host>`, `tz [±HH:MM|minutes|reset]`,
  `sensor`, `http status`. `linenoise` runs in dumb mode for the USB VFS.
- **Sensors**: SHTC3 temperature/humidity (I2C, CRC-8 checked) + battery
  voltage (ADC1 CH3, ×3 divider). Device RSSI is real
  (`esp_wifi_sta_get_ap_info`); values flow into the model each frame.
- **HTTP debug server** (:80): `/`, `/status`, `/snapshot.pbm`,
  `/snapshot.bmp`. Snapshots encode via the shared `frame_export`, so
  host↔target exports are byte-comparable.
- **NTP**: manual `ntp server` CLI > SD config `ntp_server` (milestone 8) >
  DHCP option 42 > pool.ntp.org, with a watchdog that reverts to the pool if
  the DHCP-provided server cannot sync within ~18 s. Stays on the lwIP SNTP
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
  keyboard, headless PBM/BMP/PNG export, 15 Hz frame cap.
- **Firmware**: working — full UI ported to the panel and verified on
  hardware (time/sensors/telemetry all live), Wi-Fi + SNTP + HTTP +
  CLI bring-up complete, 1 Hz second-aligned refresh.
- **Not yet**: SD card config/alarm storage, PCF85063 RTC holdover,
  alarm feature, custom icon fonts, low-battery/Wi-Fi-lost visual polish —
  see milestone status in the design notes.
