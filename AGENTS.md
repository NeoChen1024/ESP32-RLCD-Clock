# Agent Runbook

ESP32-RLCD is a Waveshare ESP32-S3 RLCD 4.2 time-scale instrument. The same
u8g2 render sources drive the SDL3 host simulator and the ST7305 target.

## Documentation

Write repository documents, comments and user-facing product text primarily
in English; discuss work with the user in their language. Give each topic one
primary home:

- `README.md` and `host/README.md`: supported build and usage instructions.
- `docs/architecture.md`: overview (shared code, tasks, locks, data flow).
- `docs/time.md`: time scales, local time, SNTP, time state and RTC.
- `docs/storage.md`: volumes, managed files, selection, journal, file API.
- `docs/audio.md`: sound formats, playback pipeline, loops, volume, control.
- `docs/network.md`: Wi-Fi modes, known-network secrets, HTTP endpoints.
- `docs/events.md`: agreed event/alarm scheduling design until implemented.
- `docs/hardware_notes.md`: board wiring, panel behavior and power boundary.
- `rlcd_time_scale_monitor_implementation_notes.md`: remaining work and
  explicitly agreed scope.
- This file: local build/bring-up steps and invariants agents must preserve.

Link to these homes instead of duplicating them. Keep completed milestones
brief; remove superseded plans. Put validation evidence in the task response,
not dated debug transcripts or sample readings in ordinary project docs.

## Local environment

This host runs Arch Linux. Use `uv` for Python tooling. ESP-IDF v6.0.2 is
installed at `/opt/esp-idf`; **every new shell** running `idf.py`, `esptool.py`
or the Xtensa toolchain must first run:

```sh
source /opt/esp-idf/export.sh
```

This activates `~/.espressif/python_env/idf6.0_py3.14_env/`. If Component
Manager cache writes fail under sandboxing, set
`XDG_CACHE_HOME=/tmp/rlcd-idf-cache` for the build. Host storage tests need
the system `libcjson` pkg-config package.

The board uses USB-Serial/JTAG, not UART0. Discover its changing device path
through `/dev/serial/by-id` (`303a:1001`); `/dev/ttyACM0` is only an example.
The original factory firmware was silent on serial, so use esptool to probe a
silent device. Vendor ESP-IDF examples are available locally at
`~/scratchpad/ESP32-S3-RLCD-4.2/02_Example/ESP-IDF/`; `11_U8G2_Test` is the
display reference and `10_FactoryProgram` is a known-good baseline.

## Build and flash

```sh
source /opt/esp-idf/export.sh
cd firmware
idf.py -p /dev/ttyACM0 build flash  # replace with the detected port
```

`firmware/` is a standalone ESP-IDF `esp32s3` project. Its partition table
has a 4 MiB factory app and 8 MiB wear-levelled FAT storage, with no OTA
slots. Normal boot does not format storage. `flash init` may initialize an
unmountable internal volume; `sd format` erases the SD card. Unmount SD before
removing it. File access and mount changes share the storage mutex. The audio
reader holds it only per chunk. Long holders must yield it between chunks and
check `storage_generation_locked()`. Call `audio_mgr_release_locked()` before
unmounting, formatting, deleting or replacing files.

```sh
cmake -S host -B host/build && cmake --build host/build -j
ctest --test-dir host/build --output-on-failure
host/build/rlcd_host --png out.png  # headless; also --pbm / --bmp
```

## Firmware invariants

- The console uses `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` and
  `linenoiseSetDumbMode(1)`; terminal escape probing hangs this USB VFS.
- The panel renders bit 0 as black, opposite to the u8g2/host buffer. Keep
  inversion in the ST7305 DRAW_TILE callback only. Snapshots must export the
  un-inverted shared framebuffer, using `common/frame_export.{h,c}`.
- Wi-Fi credentials come only from `secrets/wifi.json` or the RAM-only
  `wifi connect`; never write them to NVS, serve `secrets/` over HTTP GET or
  log passwords. Keep examples and docs free of real credentials. CLI
  overrides stay in RAM. SNTP priority is manual
  CLI, selected config, DHCP option 42, then pool fallback. DHCP addresses
  are captured independently of the active SNTP table using the linker
  `--wrap=dhcp_set_ntp_servers`; SNTP operations run on tcpip_thread.
- Config files are selected by descending filename, SD before internal
  flash. The HTTP API does not overwrite an existing config version. The
  private `.rlcd-txn` journal is recovered under the storage lock; ordinary
  mounts must never auto-format either volume.
- Time state is INVALID, TRUSTED or RTC_HOLD (`common/clock_health`). It is
  the only input to display masking and event scheduling.
  - Wall time outside [build, build + 10 years] is INVALID, and SNTP
    results outside that window are rejected before they set the clock.
  - An RTC boot needs the oscillator-stop flag clear, a valid calendar, the
    RAM marker and the build window. It is TRUSTED with an NVS checkpoint
    under 24 hours, and RTC_HOLD otherwise.
  - RTC_HOLD holds only while the per-minute RTC cross-check passes: within
    60 s or 50 ppm of the time since the clocks were aligned.
  - The current board has no RTC backup battery; true power-loss retention
    remains unverified.
- Local time is a POSIX TZ rule (`common/tz_rule.c`), not zoneinfo; POSIX
  offsets count west, while CLI and legacy `tz_offset_minutes` offsets count
  east. A `time/leap-seconds.list` is used only after its SHA-1 line and
  one-second steps verify; an expired table holds its last TAI−UTC.
- SHTC3 last-good telemetry expires after 10 seconds without a valid read.
  An ADC failure renders battery voltage as `n/a`, never as zero volts.
