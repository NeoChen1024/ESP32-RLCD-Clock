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
- USB port: `/dev/ttyACM1` (device visible as `303a:1001` Espressif USB
  JTAG/serial). The factory firmware does NOT emit serial output, so reads on
  the port hang — do not treat a silent port as a dead device; probe with
  esptool instead.

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

ST7305 display: 400×300 landscape, SPI mode 0, up to ~24 MHz clock.
u8g2 setup uses the vendor's `u8g2_st7305` component (tile buffer + SPI
flush); the host backend (400×304 vertical_top_lsb) matches it 1:1.

## Firmware (bring-up, ESP-IDF)

`firmware/` is a standalone ESP-IDF project (target esp32s3). Build/flash:

```sh
source /opt/esp-idf/export.sh
cd firmware
idf.py -p /dev/ttyACM1 build flash
```

- **Console**: the board's only host-facing serial port is the USB-Serial/JTAG
  controller (`/dev/ttyACM1`), NOT UART0 (GPIO 43/44 are unused). sdkconfig
  must set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, and the CLI calls
  `linenoiseSetDumbMode(1)` — linenoise's escape-sequence probe hangs the
  USB-serial VFS otherwise.
- **CLI commands**: `wifi connect "<ssid>" [password] | wifi status |
  wifi disconnect`, `ntp status | ntp server <host|ip> | ntp reset |
  ntp resync`, `tz [±HH:MM|±HHMM|minutes|reset]`, `http status`. Settings are
  NOT persisted (WIFI_STORAGE_RAM; nothing written to NVS; TZ defaults to
  UTC+8 at boot).
- **UI**: the full time-scale face is ported. Shared sources compiled verbatim
  from `host/src/`: `time_model.c` (pure int64 derivations; platform hooks
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
- **NTP server selection**: manual `ntp server` override > DHCP option 42
  (CONFIG_LWIP_DHCP_GET_NTP_SRV) > pool.ntp.org fallback. Re-applied on every
  `IP_EVENT_STA_GOT_IP`. `ntp status` shows `server_name` (what we configured)
  vs `server_ip` (the address actually in use).
  **Verified behavior**: the office DHCP (10.127.16.1) advertises option 42
  pointing at itself (10.127.16.1), but that host has NO NTP daemon. lwIP's
  DHCP mode is authoritative (option-42 IPs replace the pool, no automatic
  fallback), so sntp_mgr runs a watchdog: if the DHCP-provided server hasn't
  synced ~18 s after GOT_IP, it reverts to pool.ntp.org (`source: fallback`).
  Manual override always wins. Do NOT re-apply fallback names on GOT_IP —
  that clobbers the DHCP-written addresses (bug fixed 2026-08-22).
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
- **HTTP debug server** (:80): `/`, `/status`, `/snapshot.pbm`,
  `/snapshot.bmp`. Snapshot renders the current face and encodes via
  `fopencookie` streaming into the shared `frame_export` (same bytes as host).
- u8g2 is compiled from the repo submodule by `firmware/components/u8g2/`;
  `frame_export.{h,c}` is compiled straight from `host/src/` — both stay
  verbatim-shared (notes §8.5, §9).

## Host simulator

```sh
cmake -S host -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
./build/rlcd_host --png out.png   # headless render; also --pbm / --bmp
```

Shared pure-C frame exporters (`host/src/frame_export.{h,c}`) must stay
verbatim-shared with the target so host↔target screenshot diffs are
byte-exact (see notes §8.5).
