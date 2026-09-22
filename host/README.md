# RLCD Time Scale Monitor — Host Simulator

Host-first development simulator for the ESP32-S3 RLCD 4.2 time-scale instrument.
u8g2 is the only drawing engine; SDL3 only presents the final 1-bit framebuffer.
The same render code also drives the ST7305 target backend.

## Layout

```
host/
  CMakeLists.txt
  src/
    main.c            SDL3 main loop, 15 Hz frame cap, keyboard input
    host_time.c       platform glue: time_model_now(), tz_offset_minutes()
    sdl3_backend.{h,c} u8g2 display callback + SDL3 presenter (400x300 visible, 400x304 buffer)
  tests/            seven CTest targets for rendering, time, network, storage and config
  sample.png         single-face screenshot (--png output)
../common/            shared pure-C sources, compiled by host and firmware:
  time_model.{h,c}   integer time-scale derivations (MJD-TAI, GPS week/TOW, civil, ISO week)
  render_faces.{h,c} u8g2 draw calls for the single all-in-one face
  frame_export.{h,c} PBM/BMP encoders
  clock_health.{h,c} monotonic trust and clock-step policy
  storage_files.{h,c} managed paths, file validation and version selection
  display_geometry.h shared visible/buffer dimensions (no SDL dependency)
```

## Build

```sh
cmake -S host -B host/build -DCMAKE_BUILD_TYPE=Release
cmake --build host/build -j
```

## Run

```sh
host/build/rlcd_host                 # window, 3x scaling
host/build/rlcd_host --scale 2       # 2x scaling
host/build/rlcd_host --pbm out.pbm   # headless: render one frame, save PBM, exit
host/build/rlcd_host --png out.png   # headless: render one frame, save PNG, exit
```

The PBM/PNG paths do not initialize the SDL video subsystem, so they work on
no-display machines without setting `SDL_VIDEODRIVER`.

## Keyboard

| Key | Action                          |
|-----|---------------------------------|
| S   | save `rlcd_screenshot.pbm`       |
| N   | cycle sync/trust state (including expired trust)            |
| W   | toggle Wi-Fi-lost state           |
| B   | toggle normal/low battery         |
| Esc | quit                             |

All time-scale telemetry is shown on a single face; there is no page
switching. The main loop is capped at **15 Hz** (~66 ms/frame), sufficient
for a 1 s-tick instrument readout.

A rendered sample is in [`sample.png`](sample.png) (400x300, RGBA).

## Design notes

- **u8g2 is the only drawing engine.** SDL3 never draws text/glyphs itself; it
  only copies the u8g2 framebuffer to a texture. Host and target compile the
  same `render_faces.c`.
- **Framebuffer padding.** 300 is not a multiple of 8, so the u8g2 buffer is
  padded to 400x304 (38 tile rows, 15200 bytes). The visible 400x300 top portion
  is presented; layout code never hardcodes the byte stride.
- **Integer-only time math.** No float/double in time-scale derivation — all
  paths use `int64` milliseconds/seconds to avoid readout jitter.
- **Hardcoded current-era offsets** (no historical leap-second table and no
  automatic update after a future leap second): `TAI = UTC + 37`,
  `GPS = UTC + 18`.
- **Selected host fonts.** The host build links only the three u8g2 fonts used
  by the current face rather than the complete generated font catalogue.

## Test

```sh
ctest --test-dir host/build --output-on-failure
```

The seven CTest targets cover time math, frame encoding, monotonic trust and
clock-step scheduling, firmware Wi-Fi/SNTP managers with a fake IDF transport,
masking all time fields when untrusted, and file-path/content validation with
interrupted FAT replacement recovery. They also test SD-first config version
selection and fallback to internal flash/defaults. Storage tests link the
system libcjson package through pkg-config. The fake transport exercises the
actual manager sources but does not emulate radio/RTOS/network timing.
