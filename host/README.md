# RLCD Time Scale Monitor — Host Simulator

Host-first development simulator for the ESP32-S3 RLCD 4.2 time-scale instrument.
u8g2 is the only drawing engine; SDL3 only presents the final 1-bit framebuffer.
The same render code will later drive the ST7305 target backend.

## Layout

```
host/
  CMakeLists.txt
  src/
    main.c            SDL3 main loop, 15 Hz frame cap, keyboard input
    time_model.{h,c}  integer time-scale derivations (MJD-TAI, GPS week/SOW, civil, ISO week)
    render_faces.{h,c} u8g2 draw calls for the single all-in-one face
    sdl3_backend.{h,c} u8g2 display callback + SDL3 presenter (400x300 visible, 400x304 buffer)
  tests/
    test_time_model.c offline time-model checks (no SDL)
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
```

Headless / no-display machines:

```sh
SDL_VIDEODRIVER=dummy host/build/rlcd_host --pbm out.pbm
```

## Keyboard

| Key | Action                          |
|-----|---------------------------------|
| S   | save `rlcd_screenshot.pbm`       |
| Esc | quit                             |

All time-scale telemetry is shown on a single face; there is no page
switching. The main loop is capped at **15 Hz** (~66 ms/frame), sufficient
for a 1 s-tick instrument readout.

## Design notes

- **u8g2 is the only drawing engine.** SDL3 never draws text/glyphs itself; it
  only copies the u8g2 framebuffer to a texture. Host and target will share the
  same `render_faces.c`.
- **Framebuffer padding.** 300 is not a multiple of 8, so the u8g2 buffer is
  padded to 400x304 (38 tile rows, 15200 bytes). The visible 400x300 top portion
  is presented; layout code never hardcodes the byte stride.
- **Integer-only time math.** No float/double in time-scale derivation — all
  paths use `int64` milliseconds/seconds to avoid readout jitter.
- **Hardcoded offsets** (current/future only, no leap-second table):
  `TAI = UTC + 37`, `GPS = UTC + 18`.

## Test

```sh
ctest --test-dir host/build --output-on-failure
```
