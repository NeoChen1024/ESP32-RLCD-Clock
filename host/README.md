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
    host_time.c       platform glue: time_model_now() (system clock and local zone)
    sdl3_backend.{h,c} u8g2 display callback + SDL3 presenter (400x300 visible, 400x304 buffer)
  tests/            thirteen CTest targets for rendering, time, TZ/leap, audio decoding, sensors, RTC, network, storage, config and Wi-Fi secrets
  tests/data/       small WAV/FLAC fixtures (ffmpeg tone, flac --best --no-padding)
  sample.png         single-face screenshot (--png output)
../common/            shared pure-C sources, compiled by host and firmware:
  time_model.{h,c}   integer time-scale derivations (MJD-TAI, GPS week/TOW, civil, ISO week)
  render_faces.{h,c} u8g2 draw calls for the single all-in-one face
  frame_export.{h,c} PBM/BMP encoders
  tz_rule.{h,c}      POSIX TZ rule parsing and local offset
  leap_table.{h,c}   leap-seconds.list parsing, SHA-1 check and TAI−UTC lookup
  sha1.{h,c}         minimal SHA-1 for the leap table hash
  wav_format.{h,c}   RIFF/WAVE header parsing
  audio_source.{h,c} WAV/FLAC (dr_flac from ../contrib/dr_libs) to 16-bit PCM
  clock_health.{h,c} monotonic trust and clock-step policy
  sensor_health.{h,c} bounded last-good SHTC3 sample policy
  rtc_clock.{h,c}   PCF85063A calendar and boot-age validation
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
- **Model-supplied offsets.** `clock_model_t` carries the local offset and
  TAI−UTC. The host uses the system local zone and the built-in TAI−UTC of
  37 s; the firmware uses its POSIX TZ rule and verified leap table.
- **Selected host fonts.** The host build links only the three u8g2 fonts used
  by the current face rather than the complete generated font catalogue.

## Test

```sh
ctest --test-dir host/build --output-on-failure
```

The thirteen CTest targets cover time math, WAV header parsing, bit-exact
FLAC (16/24-bit) decoding against a WAV reference with rewind and damage
detection, POSIX TZ rules checked against
glibc, leap-seconds.list hash and step validation, frame encoding, monotonic trust and
clock-step scheduling, firmware Wi-Fi/SNTP managers with a fake IDF transport
(including known-network scan order, failover and backoff), `wifi.json`
validation and the repository example files,
sensor sample expiry/recovery, masking invalid telemetry and untrusted time,
and file-path/content validation with
interrupted FAT replacement recovery. They also test SD-first config version
selection and fallback to internal flash/defaults, config cleanup, plus RTC calendar decoding
and the 24-hour boot trust limit. Storage tests link the
system libcjson package through pkg-config. The fake transport exercises the
actual manager sources but does not emulate radio/RTOS/network timing.
