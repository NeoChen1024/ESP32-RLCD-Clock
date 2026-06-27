# RLCD Time Scale Monitor

A Waveshare ESP32-S3 RLCD 4.2 based **time-scale instrument** — a reflective
monochrome 400×300 display showing local time, UTC, MJD(TAI), GPS week/SOW,
ISO week date, sync state and telemetry, in a fixed-width instrument aesthetic.

Primary time source is Wi-Fi SNTP/NTP, with PCF85063 RTC / ESP32 system time
as holdover. No GNSS, no PPS, no leap-second historical table.

## Repository layout

```
rlcd_time_scale_monitor_implementation_notes.md   design reference
host/                                             host-first simulator (SDL3 + u8g2)
  src/    time model, render face, SDL3 backend, main loop
  tests/  offline time-model unit tests
u8g2/                                             u8g2 submodule (drawing engine)
docs/                                             schematics
```

## Host simulator

The host simulator is the active development surface. u8g2 is the only
drawing engine; SDL3 only presents the final 1-bit framebuffer, so the same
render code will later drive the ESP32/ST7305 target.

```sh
cmake -S host -B host/build -DCMAKE_BUILD_TYPE=Release
cmake --build host/build -j
ctest --test-dir host/build --output-on-failure   # time-model tests
host/build/rlcd_host                              # window (15 Hz cap, nearest-neighbor)
host/build/rlcd_host --scale 2
host/build/rlcd_host --pbm out.pbm                # headless single-frame dump
```

See `host/README.md` for simulator details and the design notes for the full
spec.

## Design reference

`rlcd_time_scale_monitor_implementation_notes.md` covers hardware
constraints, the single-face display layout, time-scale derivations
(integer-only MJD-TAI / GPS week-SOW / ISO week), the graphics stack
decision (u8g2, no LVGL), firmware architecture and milestones.

## Status

Host simulator: working — single face renders all time-scale fields from the
system clock, headless PBM export, 15 Hz frame cap, HiDPI-aware (nearest
scaling, aspect-preserving). ESP32/ST7305 target backend and SD config/alarm
storage are not yet implemented.
