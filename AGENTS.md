# AGENTS.md

Work notes for agents operating in this repository.

## Project

ESP32-RLCD: Waveshare ESP32-S3 RLCD 4.2 time-scale instrument. Host-first
development — the same u8g2 render code drives both the SDL3 host simulator
(`host/`) and, on the target, the ST7305 reflective panel via SPI. See
`rlcd_time_scale_monitor_implementation_notes.md` for the full design.

## ESP-IDF environment

ESP-IDF is installed system-wide at `/opt/esp-idf` (v6.0.2). Every new shell
MUST activate it before `idf.py` / `esptool.py` / toolchain commands:

```sh
source /opt/esp-idf/export.sh
```

This puts `idf.py` on `PATH` and activates the Python venv at
`~/.espressif/python_env/idf6.0_py3.14_env/` (esptool.py lives there).
Toolchain: `xtensa-esp32s3-elf-gcc` from `~/.espressif/tools/xtensa-esp-elf/`.

## Target hardware

- Board: Waveshare ESP32-S3 RLCD 4.2, chip revision v0.2, 8 MB embedded PSRAM,
  Wi-Fi/BT, USB-Serial/JTAG (no external UART bridge).
- Flash: 16 MB, quad (QIO), 3.3 V, set by eFuse.
- USB-Serial/JTAG port: `/dev/ttyACM0` in the 2026-09-23 session, but the
  number can change (previously `/dev/ttyACM1`); discover via
  `/dev/serial/by-id`. The USB device identifies as `303a:1001`.
  The factory firmware does not emit serial output, so a silent port is not
  evidence of dead hardware; probe with esptool instead.

### Reference examples (pins + init)

Vendor ESP-IDF examples live at
`~/scratchpad/ESP32-S3-RLCD-4.2/02_Example/ESP-IDF/`. Most relevant:

- `11_U8G2_Test/` — u8g2 on the ST7305 panel; includes a complete
  `u8g2_st7305` ESP-IDF component (SPI backend + init sequence) and the
  `port_bsp` DisplayPort wrapper.
- `10_FactoryProgram/` — vendor factory firmware; useful as a known-good
  baseline for `sdkconfig` (SPIRAM, partition table, flash mode).

### Pin map (from `11_U8G2_Test/main/user_config.h`)

| Function | GPIO |
|---|---|
| RLCD MOSI | 12 |
| RLCD SCK | 11 |
| RLCD DC | 5 |
| RLCD CS | 40 |
| RLCD RST | 41 |
| RLCD TE | 6 |
| I2C SDA (PCF85063/SHTC3) | 13 |
| I2C SCL | 14 |
| SDMMC CLK (1-bit) | 38 |
| SDMMC CMD | 21 |
| SDMMC D0 | 39 |

ST7305 display: 400×300 landscape, SPI mode 0, up to ~24 MHz clock.
u8g2 setup uses the vendor's `u8g2_st7305` component (tile buffer + SPI
flush); the host backend (400×304 vertical_top_lsb) matches it 1:1.

## Firmware (bring-up, ESP-IDF)

`firmware/` is a standalone ESP-IDF project (target esp32s3).

Custom flash layout: `firmware/partitions.csv`, 4 MiB factory app at
`0x10000`, NVS at `0x9000` (24 KiB), PHY at `0xf000` (4 KiB). The rest of
the 16 MiB flash includes an 8 MiB `storage` FAT partition at `0x410000`.
It mounts at `/flash` with 4096-byte-sector wear levelling; no OTA slots.
`flash init` explicitly formats if unmountable; normal boot never auto-formats.

Build/flash:

```sh
source /opt/esp-idf/export.sh
cd firmware
idf.py -p /dev/ttyACM0 build flash  # replace with the currently detected port
```

- **Console**: the board's only host-facing serial port is the USB-Serial/JTAG
  controller, not UART0 (GPIO 43/44 are unused). sdkconfig
  must set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, and the CLI calls
  `linenoiseSetDumbMode(1)` — linenoise's escape-sequence probe hangs the
  USB-serial VFS otherwise.
- **CLI commands**: `wifi connect "<ssid>" [password] | wifi status |
  wifi disconnect | wifi reconnect`, `ntp status | ntp server <host|ip> | ntp reset |
  ntp resync`, `tz [±HH:MM|±HHMM|minutes|reset]`, `config status |
  config reload`, `sensor`, `sd status | mount | unmount | ls | cat | test |
  format`, `flash status | mount | init`, `http status`. Wi-Fi credentials
  and CLI overrides are RAM-only
  (WIFI_STORAGE_RAM; nothing written to NVS); boot loads the selected config
  version from SD, then flash, or defaults to UTC+8.
- **UI**: the full time-scale face is ported. Shared sources compiled verbatim
  from `common/`: `time_model.c` (pure int64 derivations; platform hooks
  `time_model_now()` + `tz_offset_minutes()` live in the platform glue),
  `render_faces.c` (the 400x300 single face), `frame_export.c`. Host glue is
  `host/src/host_time.c`; device glue is `firmware/main/model.c` (fills
  clock_model_t from SNTP + Wi-Fi state, trust-gated).
- **Sensors** (`firmware/main/sensors.c`): SHTC3 temp/humidity over I2C
  (SDA=13 SCL=14, addr 0x70, cmd 0x7866, CRC-8 poly 0x31, T=175·raw/65536−49,
  RH=100·raw/65536) + battery voltage via ADC1 CH3 (GPIO4, atten 12dB,
  divider ×3). `sensor` CLI command reads both. Values flow into the model
  each frame; failed SHTC3 reads keep the last good value.
  Device RSSI is real (`esp_wifi_sta_get_ap_info`).
- **NTP server selection**: manual `ntp server` CLI override > selected
  config JSON `ntp_server` key (SD before flash) > DHCP option 42
  (CONFIG_LWIP_DHCP_GET_NTP_SRV) > pool.ntp.org fallback. Re-applied on every
  `IP_EVENT_STA_GOT_IP`. `ntp status` shows `server_name` (what we configured)
  vs `server_ip` (the address actually in use).
  DHCP addresses are captured independently using the linker's
  `--wrap=dhcp_set_ntp_servers`; all SNTP operations run on tcpip_thread.
  Failed DHCP trials fall back after 18 s and retry every 5 minutes;
  `ntp reset` restores config/DHCP/fallback priority immediately. Sync age uses
  monotonic time: fresh <2 h, trusted system-clock holdover <24 h.
  Status reads never consume sync events. Manual changes/resync preserve
  last-good trust. PCF85063 itself is still not implemented.
  Historical 2026-08 bring-up found no NTP service at DHCP source 10.127.16.1;
  current 2026-09-23 hardware testing on the isolated Wi-Fi confirms it now
  responds successfully. Do not assume the old outage persists.
  Wi-Fi retries indefinitely with 1–30 s capped backoff; explicit disconnect
  cancels retries. `wifi reconnect` exercises a real driver disconnect/recovery.

- **Display**: real ST7305 panel via `firmware/components/u8g2_st7305/`
  (vendor-provided backend: SPI mode 0 @ 24 MHz, MOSI=12 SCK=11 DC=5 CS=40
  RST=41, U8G2_R1 rotation, full buffer). `display.c` owns the u8g2 instance
  + mutex; `display_task` renders `render.c` at 1 Hz and flushes. The refresh
  is **aligned to the wall-clock second boundary** (delay recomputed from
  `gettimeofday()` each loop, so SNTP steps self-heal). The u8g2
  physical buffer is 304x400 (panel native orientation); `snapshot.c` re-maps
  it to the host's 400x300 layout before encoding so exports stay
  byte-comparable with the host.
  **Ink polarity (verified on hardware)**: this RLCD panel renders bit 0 =
  black (ink), bit 1 = white (paper) — opposite of u8g2/host convention
  (1 = ink). The u8g2 buffer stays host-convention (1 = black); the backend's
  DRAW_TILE callback XORs 0xFF before the bytes reach the panel. Snapshots
  export the un-inverted buffer, matching the host. If you change the backend,
  keep this inversion and the host-buffer convention intact.
- **SD**: `storage_mgr.c` owns mount/lifecycle and a mutex for diagnostic file
  operations. Vendor `06_SD_Card` wiring: 1-bit SDMMC CLK=38 CMD=21 D0=39,
  20 MHz; mount point `/sdcard`, `format_if_mount_failed=false` and status
  checks and long filenames enabled.
  CLI: `sd status | mount | unmount | ls [dir] | cat <file> | test | format`.
  `sd format` explicitly erases the SD FAT volume (16 KiB clusters); it never
  formats internal flash. This was authorized for the empty test card.
  Paths are relative; cat is a bounded 4 KiB preview. `sd test` exclusively
  creates `.rlcd-sd-selftest.tmp`, writes/fsyncs/reads/verifies 4096 bytes,
  then deletes only the file it created. It never overwrites an existing
  test file. Unmount before removal; no automatic hotplug detection.
  `storage_mgr.c` also owns internal FAT and the shared volume mutex.
  `/files` is the English web file manager; `/` shares its CSS. `/fs/` lists
  volumes, `/fs/active` reports the selected config, and `/fs/flash/` and
  `/fs/sd/` route GET/PUT/DELETE for config/*.json and sounds/*.wav.
  `http_files.c` uses an async storage worker (one active + one queued);
  4 KiB streaming buffers, 120 s transfer deadline, JSON max 16 KiB/depth 16,
  WAV max 16 MiB. Config versions sort by filename descending; the first
  semantically valid SD file wins, otherwise flash is tried. JSON supports
  `tz_offset_minutes` (-840..840) and optional `ntp_server`; new uploads never
  overwrite an existing config version. CLI overrides remain until reset.
  Alarm config, time-scale offsets and audio playback remain pending. FLAC
  files are not accepted by the current API.
  `common/storage_files.c` validates paths/content and stages replacement via
  `.rlcd-txn/{record,upload,backup}`. Recovery is under the storage mutex.
  The journal is removed last; invalid/foreign journal state is left untouched.
  This handles interrupted operations, not arbitrary FAT power-loss corruption.
  cJSON 1.7.19 is pinned via IDF Component Manager; if sandbox cache writes
  fail, build with `XDG_CACHE_HOME=/tmp/rlcd-idf-cache` after IDF activation.
  Host storage tests require the system `libcjson` pkg-config package.
- **HTTP server** (:80): `/`, `/files`, `/fs/`, `/fs/active`, `/status`,
  `/snapshot.pbm`, `/snapshot.bmp`; `http status` currently prints only the
  debug endpoints. Snapshot renders the current face and encodes via
  `fopencookie` streaming into the shared `frame_export` (same bytes as host).
- u8g2 is compiled from the repo submodule by `firmware/components/u8g2/`;
  `frame_export.{h,c}` is compiled straight from `common/` — both stay
  verbatim-shared (notes §8.5, §9).

## Host simulator

```sh
cmake -S host -B host/build && cmake --build host/build -j
ctest --test-dir host/build --output-on-failure
host/build/rlcd_host --png out.png   # headless render; also --pbm / --bmp
```

Shared pure-C frame exporters (`common/frame_export.{h,c}`) must stay
verbatim-shared with the target so host↔target screenshot diffs are
byte-exact (see notes §8.5).
