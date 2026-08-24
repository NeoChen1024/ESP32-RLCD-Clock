# RLCD Time Scale Monitor — Implementation Notes

Design reference for a Waveshare ESP32-S3 RLCD 4.2 board acting as an NTP/RTC-based time-scale instrument. 400×300 reflective monochrome display showing ISO-8601 civil time, UTC, TAI-based MJD, GPS Week/TOW, sync state, temperature/humidity, battery and Wi-Fi status.

---

## 1. Hardware

- ESP32-S3 (Xtensa LX7 dual-core, 240 MHz), 512 KB SRAM, 16 MB Flash, 8 MB PSRAM
- 2.4 GHz Wi-Fi, BT 5 LE
- 4.2" fully reflective RLCD, landscape **400×300 px**
- PCF85063 RTC, SHTC3 temp/humidity, 18650 battery holder, MicroSD
- KEY/BOOT side buttons (limited usable UI keys), 2×8 2.54 mm expansion header

**Ink polarity (verified on hardware)**: this RLCD panel renders bit 0 as ink
(black) and bit 1 as paper (white) — the opposite of u8g2's convention
(1 = ink). The u8g2 buffer is kept host-convention (1 = black) everywhere; the
ST7305 backend inverts (XOR 0xFF) only in the DRAW_TILE callback right before
bytes reach the panel. Snapshots export the un-inverted buffer, so host and
target exports stay byte-comparable. **Pin map (verified, from the vendor
example)**: RLCD MOSI=12 SCK=11 DC=5 CS=40 RST=41 TE=6; I2C SDA=13 SCL=14;
battery ADC = ADC1_CH3 (GPIO4, divider ×3).

### RTC battery strategy and unsafe time state

No RTC backup battery is installed. This is acceptable: main power comes from the 18650, and Wi-Fi/SNTP can re-acquire trusted time after boot. However, when the 18650 is removed / low-voltage cutoff / long-term depleted, the PCF85063 may lose time. Firmware must have an explicit unsafe time state to avoid showing seemingly precise MJDTAI / GPS values before sync.

Sync state codes:

```text
BOOT UNS   booted, trusted time not yet acquired
SYNC...    syncing
NTP OK     SNTP/NTP synced, time trusted
RTC HOLD   relying on RTC or system clock holdover
WIFI LOST  Wi-Fi down
```

When unsynced, show placeholders — never fake-precise values:

```text
TIME   UNSYNC
MJDTAI --------.-------
GPS    W---- TOW------
```

---

## 2. Display design direction

### 2.1 Style

Carry over the fixed-grid instrument aesthetic from the old GPS clock (14×6 Nokia 5110): fixed-width tabular numeric, short labels, telemetry layout, high information density, status-code vocabulary, small icons with short text. Conceptually shift from **GPS telemetry clock** to **time-scale telemetry clock**.

### 2.2 400×300 landscape layout

```text
Top bar       24 px   date, status, sync age
Main time     88 px   local time, big numeric font
UTC/scales    96 px   UTC, MJDTAI, GPS week/TOW
Telemetry     64 px   temp, humidity, battery, Wi-Fi
Margin        28 px   spacing / separators
```

At 8×16 monospace this is roughly 50 columns × 18 rows — far more room than the old 14×6, so no need to over-compress.

---

## 3. Time scales and formats

### 3.1 Core time scales

Local civil time (ISO-8601), UTC (ISO-8601), ISO week date, MJD on TAI scale, GPS Week Number, GPS TOW, NTP sync age / trust state.

### 3.2 Leap second / TAI−UTC strategy

Hardcode current-era offsets. There is no historical leap-second table and no
automatic update after a future leap second, so these constants remain valid
only while TAI−UTC is 37 seconds:

```c
#define TAI_MINUS_UTC_SECONDS 37
#define GPS_MINUS_UTC_SECONDS 18
// TAI = UTC + 37, GPS = UTC + 18, TAI = GPS + 19
```

### 3.3 MJD(TAI)

Signature field, instrument-readout aesthetic (7 fractional digits = 8.64 ms resolution — slightly above actual SNTP sync precision, intentionally over-spec'd):

```text
MJDTAI 0060842.6373148    format DDDDDDD.xxxxxxx, leading zeros are part of the display format
```

Integer arithmetic (avoid float/double; all intermediate terms must be int64):

```c
int64_t tai_ms = unix_ms + 37000;
int64_t mjd_ms = 40587LL * 86400000LL + tai_ms;
int64_t mjd_day = mjd_ms / 86400000LL;
int64_t rem_ms  = mjd_ms % 86400000LL;
int64_t frac_1e7 = rem_ms * 10000000LL / 86400000LL;  // 7 decimal places of day
// MJDTAI %07lld.%07lld
```

### 3.4 GPS Week / TOW

GPS epoch 1980-01-06T00:00:00Z; use full week number (no 10-bit rollover):

```c
#define UNIX_TO_GPS_EPOCH 315964800LL
#define GPS_MINUS_UTC    18LL
int64_t gps_s = unix_s - UNIX_TO_GPS_EPOCH + GPS_MINUS_UTC;
int64_t gps_week = gps_s / 604800;
int64_t gps_tow  = gps_s % 604800;
```

Display `GPS W=2424 TOW=055035` or compact `G2424/055035`.

### 3.5 ISO-8601 / ISO week

Local civil time, UTC, and ISO week date are all rendered on the single face
(see §4). Compact display forms used on-screen:

```text
23:17:15          local HH:MM:SS (big)
+0800  UTC 15:17:15Z
ISO 2026-W25-7
```

---

## 4. Display layout (single face)

All time-scale telemetry lives on a single 400×300 face. There is no page
switching and no dense/system page — the screen is roomy enough to hold
everything at once. Debug/config goes over the Serial console.

```text
┌────────────────────────────────────────┐
│ 2026-06-21 SUN   [bell] [wifi] NTP OK  │   top bar ([bell] only while ringing)
│                                        │
│             23:17:15                    │   local time (big)
│                                        │
│         +0800   UTC 15:17:15Z           │   tz offset + UTC
│ ─────────────────────────────────────  │   separator
│ ISO     2026-W25-7                     │   ISO week date
│ MJDTAI  0061212.6374074                │   MJD on TAI scale
│ GPS     W=2424 TOW=055053              │   GPS week / TOW
│ ─────────────────────────────────────  │   separator
│ [tmp] +28.4C  [rh] 61%  [bat] 3.91V   │   telemetry (real values since m7)
│ ntp +0s   wifi -57 dBm                 │   sync age / RSSI
└────────────────────────────────────────┘
```

Design reference only — the working face matches this layout. Differences in
the current implementation: no alarm/bell icon (alarm unbuilt), icons are
ASCII text placeholders (`[wifi]`, `[tmp]`, ...) rather than an icon font, and
telemetry shows real sensor values. Verified on hardware: `[tmp] +31.7C  [rh] 53%  [bat] 4.06V  ntp +Xs  wifi -43 dBm`.

### 4.1 Trust gating

When `time_trusted` is false (boot unsynced / holdover lost), MJDTAI and GPS
fields show placeholders (`--------.-------`, `W---- TOW------`) rather than
fake-precise values. The top-bar sync state code (`BOOT UNS` / `SYNC...` /
`NTP OK` / `RTC HOLD` / `WIFI LOST`) is the single source of trust; there is
no separate RTC-holdover indicator, `RTC HOLD` in the top bar already conveys it.

### 4.2 Button behavior

Buttons are limited; complex operations go through the Serial console. With
the single-face design the on-device keys cover only the two functions that
need to be reachable without a serial terminal:

- **short press**: dismiss a currently ringing alarm (snooze/disable)
- **long press**: force NTP resync

Everything else (config, alarm schedule editing, debug) is Serial-only.

---

## 5. Fonts and icons

### 5.1 u8g2 font format

Text, numbers and icons all go through the u8g2 font/glyph path. Do not maintain a separate bitmap icon blitter — only rare large logos/splashes use XBM or custom blits.

### 5.2 Font set

```text
font_time_big   main-time large digits (0-9 : + - Z space only); 7-seg / OCR-B / HP calc / avionics style
font_mono_18    MJD / GPS / UTC / ISO fields; must use tabular digits to avoid readout jitter
font_mono_12    status bar / system page
font_icon_16/24 small / large icons
```

### 5.3 Icon font

Icons: Wi-Fi bars, NTP/sync, RTC/crystal, battery, thermometer, droplet, SD, warning/unsafe, lock/trusted, serial/debug. First version uses ASCII codepoints for simplicity (`'0'..'3'` Wi-Fi, `'A'..'D'` battery, etc.); can switch to Unicode PUA later.

### 5.4 Asset pipeline

`assets/fonts/*.bdf` → bdfconv → `generated/u8g2_font_*.c`. Firmware and host simulator share the same generated font data.

---

## 6. Graphics stack decisions

- **No LVGL**: no touch, no complex on-device UI, no widget/theme/animation needs, settings/debug go over Serial, the screen is a fixed/semi-fixed instrument panel. LVGL's object tree, style system and event routing would be mostly overhead.
- **u8g2 is the only drawing engine**: monochrome, fixed-coordinate layout, multi-font, glyph/icon font, full-buffer rendering. Only `sendBuffer()` differs: host = SDL3, target = ST7305 SPI flush.
- **No parallel SDL renderer**: do not write separate `CanvasSDL3` / `CanvasU8g2` drawText/drawGlyph implementations — they diverge on font metrics, baseline, glyph advance, icon alignment and host-vs-target screenshots. Host and target run the same u8g2 draw calls.

---

## 7. Host simulator

### 7.1 Development order (host-first)

1. Host simulator (iterate layout / font / icon / time model) → 2. ESP32 display backend → 3. Sensor/RTC/Wi-Fi/NTP integration. The fastest-iterating parts are kerning, baseline and information density, not SPI or peripherals.

### 7.2 u8g2 + SDL3 backend

The host does not emulate the full ST7305 command set — only the final 400×300 1-bit framebuffer: u8g2 draw → internal framebuffer → `sendBuffer()` → SDL texture → 400×300 window (optional 2x/3x scaling). Target: `sendBuffer()` → ST7305 SPI flush.

### 7.3 First-version simulator goals

400×300 landscape window, integer scaling, 1-bit framebuffer preview, host
ClockModel state overrides, keyboard state switching, and PNG/PBM export.

Keyboard: `N` cycles sync/trust state, `W` toggles Wi-Fi loss, `B` toggles low
battery, `S` saves a PBM screenshot, and `Esc` quits. There is no page-switching
binding because the current design uses one face.

### 7.4 Test states

Interactive controls currently cover normal / holdover / unsync / Wi-Fi lost /
low battery. Offline time-model tests cover MJD fractional output, GPS week/TOW,
civil time, weekdays, and ISO week across both 52- and 53-week year boundaries.
Future deterministic screenshot scenarios should add old NTP age, MJD/GPS
rollovers, UTC-local date crossing, status overflow, and sensor edge values.

### 7.5 Framebuffer padding

400×300/8 = 15000 bytes, but 300 is not a multiple of 8, so the internal buffer may round up to 400×304/8 = 15200 bytes. Both host and target support `visible 400×300 / buffer 400×304`; layout/render code must not hardcode the framebuffer byte layout.

**Target buffer orientation differs from host**: the ST7305 panel's native memory is 300 wide × 400 tall (tile 38×50, pitch 304 bytes/row-group). With `U8G2_R1` rotation the u8g2 *logical* coordinate space is 400×300 landscape (matching the host), but the physical buffer bytes are in panel orientation. `snapshot.c` re-maps `(px,py) = (299−y, x)` back to the host's 400×300 layout before encoding so exports are comparable (see §8.5). The display flush itself needs no remap — the backend's DRAW_TILE callback handles it.

---

## 8. Firmware architecture

### 8.1 ESP-IDF

Target firmware is built with **ESP-IDF v6.0.2** (FreeRTOS underneath, picolibc libc). The u8g2 core (`csrc/`, pure C) compiles directly as an IDF component — no Arduino core, no Arduino-only wrapper. Direct access to FreeRTOS task/queue/mutex via the native IDF APIs.

Two IDF-specific gotchas discovered during bring-up (see AGENTS.md):

- The board's only host-facing serial port is the **USB-Serial/JTAG** controller (`/dev/ttyACM1`), not UART0. Console must use `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, and the CLI must call `linenoiseSetDumbMode(1)` — linenoise's escape-sequence probe hangs the USB-serial VFS otherwise.
- picolibc's `struct tm` has no `tm_gmtoff`, so the host's TZ readout cannot be reused on target; TZ is a CLI-configurable value instead (see §8.4).

### 8.2 FreeRTOS tasks

```text
display_task    1 Hz: render shared face → invert → ST7305 flush   [implemented]
wifi_mgr        event-driven STA connect/disconnect; auto-reconnect  [implemented]
sntp_mgr        esp_sntp with DHCP option-42 + manual + fallback     [implemented]
http_srv        esp_http_server: / /status /snapshot.pbm /snapshot.bmp [implemented]
sensor frontend SHTC3 + battery ADC, read on each display frame     [implemented]
CLI             linenoise REPL over USB-Serial/JTAG console          [implemented]
alarm_task      compare local time against SD-loaded alarm schedule; trigger audio on match  [future]
button_task     debounce; short press = dismiss ringing alarm, long press = force resync     [future]
```

`app_main` initializes subsystems, then the CLI REPL blocks (commands run in
the console task). The display task is the only periodic renderer; SNTP,
Wi-Fi and HTTP are event/task-driven.

### 8.3 ClockModel snapshot

The model is the shared `clock_model_t` (common/time_model.h, verbatim on both platforms):

```c
typedef struct {
    int64_t  unix_ms;
    bool     time_trusted;
    sync_state_t sync;        /* SYNC_BOOT_UNS / SYNC_SYNCING / SYNC_NTP_OK / SYNC_RTC_HOLD / SYNC_WIFI_LOST */
    uint32_t ntp_age_s;
    int      wifi_rssi_dbm;
    float    temp_c, rh_pct, batt_v;
} clock_model_t;
```

The display framebuffer is protected by a mutex; both the periodic display task
and the HTTP snapshot handler render through it:

`xSemaphoreTake` → `time_model_now(&m)` (device glue fills from SNTP + Wi-Fi + sensors) → `render_face(g, &m)` → `xSemaphoreGive` (display task additionally flushes). The renderer switches MJDTAI/GPS to placeholders based on `time_trusted` (see §1). `alarm_ringing` from the original design is not yet in the struct — the alarm feature is unbuilt.

### 8.4 Serial console

Primary setup/debug goes over the CLI (USB-Serial/JTAG console, `rlcd>`
prompt). Implemented commands:

```text
wifi connect "<ssid>" [password]   connect STA (quoted args; not persisted)
wifi status | wifi disconnect
ntp status | ntp server <host|ip> | ntp reset | ntp resync
tz [±HH:MM | ±HHMM | minutes | reset]      show/set local offset (default UTC+8)
sensor                                   read SHTC3 + battery ADC live
http status                              list debug endpoints
help
```

**NTP server selection** (priority order): manual `ntp server` override >
DHCP option 42 (`CONFIG_LWIP_DHCP_GET_NTP_SRV`) > pool.ntp.org fallback.
`ntp status` shows `server_name` (configured) vs `server_ip` (address actually
in use — lwIP stores DHCP servers as IPs). A watchdog reverts to the public
pool if a DHCP-provided server fails to sync within ~18 s (a DHCP option-42
address pointing at a host without an NTP daemon must not strand the clock).

**TZ**: offset is a CLI-set value (default UTC+8), not read from the host OS —
see §8.1 (picolibc has no `tm_gmtoff`). Not persisted yet; SD config comes
with milestone 8.

Future commands from the original design (RTC read/write, SD config, alarm
schedule) are not yet implemented.

### 8.5 HTTP debug snapshot

Read-only HTTP endpoint on the same Wi-Fi the instrument uses for SNTP; renders a fresh frame on demand and serves it as a 1-bit bitmap so the device's actual rendering can be inspected from a browser or compared bit-for-bit against the host simulator.

| Endpoint              | Format                           | Purpose                                                                              |
| --------------------- | -------------------------------- | ------------------------------------------------------------------------------------ |
| `GET /snapshot.pbm` | `image/x-portable-bitmap` (P4) | Canonical raw framebuffer; diff against host PBM for pixel-exact host↔target parity |
| `GET /snapshot.bmp` | 1-bit BMP (top-down)             | Browser-friendly view (`<img>` / double-click)                                     |
| `GET /`             | HTML page                        | Inline`<img src=/snapshot.bmp>` + auto-refresh for a live-ish preview from a phone |

Design rules:

- **Shared encoder**: `frame_export.{h,c}` (pure C, no SDL/hardware deps) converts the u8g2 vertical_top_lsb buffer to P4 PBM and 1-bit BMP, byte-for-byte identical on host and target. PBM: P4, `400 300`, MSB-first, row-major, 1 = ink. BMP: 1-bit, top-down (`biHeight` negative), palette {white, black}, rows padded to 4 bytes.
- **Fresh frame**: the handler locks the display framebuffer, renders the face, encodes, unlocks — never exposes a half-drawn frame.
- **Layout translation on target**: the ST7305's u8g2 buffer lives in the panel's native orientation (304-wide × 400-tall, U8G2_R1 rotation), while the face is drawn in logical 400×300 landscape. `snapshot.c` re-maps the physical buffer back to the host's 400×300 layout before encoding, so target exports stay byte-comparable with the host.
- **Read-only by construction**: no query params, no config mutation, no auth. It is a debug surface on the user's LAN, nothing more — do not extend it into a control/API endpoint. Config upload is a separate, SD-backed endpoint (§8.6), not a query-param on this one.
- Serves only when Wi-Fi is up (STA); not required for instrument function.

### 8.6 SD config upload (milestone 8)

How config files (TZ offset, TAI/GPS offset overrides, alarm times) and alarm
sound samples get onto the SD card, and how the JSON config format is
iterated during alarm development.

**Transport decision: REST `/fs/` API, not WebDAV.**

- WebDAV's only real value is OS-native mounting (Windows Explorer "Map
  network drive", macOS Finder, davfs2). The actual workflow here is
  curl / Python: `curl -T file URL` is a plain PUT — WebDAV adds nothing
  for it, only PROPFIND XML, LOCK semantics and RFC 4918 compliance cost.
- ESP-IDF would support WebDAV if we ever wanted it: `http_parser` (bundled
  component) already parses PROPFIND/LOCK/MKCOL/COPY/MOVE/UNLOCK/etc.,
  `esp_http_server` has an `HTTP_ANY` wildcard method for routing them, and
  `esp_vfs_fat_sdmmc_mount()` gives POSIX file I/O on the card. Platform is
  not the obstacle — the protocol's own verbosity is. So a future WebDAV
  layer can be added on top without changing the architecture.

API (all under `/fs/`):

| Method   | Path               | Effect                                            |
| -------- | ------------------ | ------------------------------------------------- |
| `PUT`    | `/fs/config.json`  | Upload/overwrite a config file (raw body → FATFS) |
| `GET`    | `/fs/config.json`  | Download a file (verify writes, backup)           |
| `DELETE` | `/fs/config.json`  | Remove a file                                     |
| `GET`    | `/fs/`             | List files (JSON)                                 |

Workflow: edit locally → `curl -T config.json http://<ip>/fs/config.json`
(or a Python helper) → device re-reads → observe alarm behavior → GET back to
verify. Fixed whitelist of known filenames (`config.json`, `alarms.json`,
`sounds/*.wav`), no arbitrary URL path parsing → no path traversal, and no
need for an auth scheme beyond the existing LAN-debug trust boundary (§8.5).

Implementation notes:

- Upload is the inverse of the §8.5 fopencookie pattern: `httpd_req_recv()`
  loop → write to the mounted FATFS file. No fopencookie needed for input.
- Write to a temp file then rename for atomic replace, so a half-written
  config can never be parsed as valid.
- Milestone 8 also brings the SD mount (`esp_vfs_fat_sdmmc_mount`) and the
  config parser; the endpoint itself is deliberately dumb (file in / file
  out), all semantics live in the parser.

---

## 9. Project layout

```text
rlcd-time-scale-monitor/
  AGENTS.md             environment + bring-up notes
  common/               shared pure-C render path (compiled verbatim by both)
    time_model.{h,c} render_faces.{h,c} frame_export.{h,c} display_geometry.h
  host/                 SDL3 simulator (build: cmake -S host -B build)
    src/                host-only platform code: main.c, host_time.c,
                        sdl3_backend.{h,c}, u8g2_selected_fonts.c
    tests/              test_time_model.c, test_frame_export.c
  firmware/             ESP-IDF bring-up project (idf.py -p /dev/ttyACM1 flash)
    main/               app_main.c, cli.c, wifi_mgr.{h,c}, sntp_mgr.{h,c},
                        model.{h,c}, sensors.{h,c}, display.{h,c}, render.{h,c},
                        snapshot.{h,c}, http_srv.{h,c}
    components/
      u8g2/             compiles the repo's u8g2 submodule csrc + selected fonts
      u8g2_st7305/      vendor ST7305 SPI backend (with ink-polarity inversion)
  u8g2/                 submodule (csrc is the only drawing engine)
  docs/                 schematics
  tools/                build_fonts.sh, dump_screenshot.py  [future]
```

**Shared verbatim sources** — compiled from `common/` by BOTH builds so host
and target can never drift (the "no parallel renderer" rule of §6):

- `time_model.c` — pure int64 time-scale derivations; platform hooks
  `time_model_now()` / `tz_offset_minutes()` live in `host/src/host_time.c`
  (host) and `firmware/main/model.c` (target)
- `render_faces.c` — the single 400×300 face
- `frame_export.c` — PBM/BMP encoding (§8.5)

---

## 10. Rendering API

Render code uses the u8g2 C API directly; no Canvas abstraction and no page
switching — the design settled on a single face (§4):

```c
void render_frame(u8g2_t *g)
{
    clock_model_t m;
    time_model_now(&m);          /* platform glue: SNTP + Wi-Fi + sensors */
    render_face(g, &m);          /* shared, from common/render_faces.c */
}
```

`render_face` draws the full 400×300 face (top bar, big time, UTC, ISO,
MJDTAI, GPS, telemetry) with trust-gated placeholders. Helpers isolate layout
details: `draw_str_right`, `draw_label_value`, `draw_scale_placeholder`.
`sendBuffer` is NOT called by the renderer — the display task flushes, the
HTTP handler encodes (§8.5).

---

## 11. Milestones

Status: ✅ done, ⚠️ partial, ⬜ not started. **Verified on hardware** means
flashed to the RLCD 4.2 board and confirmed over CLI/HTTP/panel.

1. ✅ **Host simulator skeleton** — SDL3 400×300 window + scaling, fake ClockModel, u8g2 render path, screenshot export
2. ✅ **Time model** — Unix ms → local/UTC/MJD(TAI)/GPS week/TOW/ISO week, offset constants, edge-case tests (2/2 tests pass)
3. ⚠️ **Fonts and icons** — uses stock u8g2 fonts (inr42/VCR_OSD/7x13) on both platforms; no custom BDF pipeline or icon font yet
4. ✅ **Main face layout** — single-face layout, screenshot export (host PNG/PBM/BMP; golden screenshot review not automated)
5. ✅ **ESP32 bring-up** — ESP-IDF v6.0.2 app, u8g2 as IDF component, ST7305 SPI init/flush (vendor backend), 1 Hz display task, CLI, HTTP debug snapshot. **Verified on hardware.**
6. ⚠️ **Time sync and RTC** — Wi-Fi/SNTP sync, NTP age, DHCP option-42 + manual + fallback servers all verified; PCF85063 RTC read/write, boot-unsafe/RTC-holdover states NOT built (host simulates via keyboard only)
7. ✅ **Sensors and telemetry** — SHTC3 (I2C, CRC-8 verified) + battery ADC (curve-fitted, divider ×3) + real Wi-Fi RSSI. **Verified on hardware** (31.7 °C / 53 %RH / 4.06 V / −43 dBm).
8. ⬜ **SD storage** — mount (`esp_vfs_fat_sdmmc_mount`), read config file (TZ / offsets / alarm times), load alarm sound effects; REST `/fs/` upload API (§8.6): PUT/GET/DELETE + list for JSON config iteration via curl/Python. Transport decided: plain HTTP PUT, not WebDAV (see §8.6).
9. ⬜ **Polish** — low battery / Wi-Fi lost state, screenshot regression, power behavior tuning

Also done ahead of plan: **HTTP debug snapshot** (§8.5) shipped with milestone 5,
and the full host UI is ported to the target (§10) with TZ configurable via CLI
(§8.4).

---

## 12. Non-goals and SD role

Not building: LVGL GUI, voice recognition, Bluetooth UI, precision RTC calibration, leap-second historical table, complex on-device menu.

**SD card role**: store a config file (TZ offset, TAI/GPS offset overrides, alarm times) and alarm sound-effect samples. Not a general log sink in the first version. Config files are pushed over the LAN via the REST `/fs/` upload endpoint (§8.6) — plain HTTP PUT from curl/Python, deliberately not WebDAV.

Future extensions: RTC drift measurement, Wi-Fi captive config portal, web UI (the §8.5 snapshot endpoint is a debug tool and the §8.6 `/fs/` endpoint is a config-transfer API — neither is the start of a web UI), SD-based logging, ham radio page (poll solar conditions / HF propagation info).

Built ahead of the original plan: HTTP debug snapshot (§8.5), CLI-configurable
TZ, DHCP option-42 NTP, and real SHTC3/battery telemetry — see milestone
status in §11.

---

## 13. Design summary

```text
Product         RLCD Time Scale Monitor
Display         400×300 landscape monochrome (RLCD: bit 0 = ink/black)
Primary source  Wi-Fi SNTP/NTP (DHCP option 42, manual override, pool fallback)
Holdover        PCF85063 RTC / ESP32 system time  [RTC not yet wired]
Main fields     local time, UTC, MJD(TAI), GPS week/TOW, ISO week
Aesthetic       fixed-width time-scale telemetry instrument
GUI framework   no LVGL
Drawing engine  u8g2 (csrc submodule, both platforms)
Host preview    SDL3 backend presenting u8g2 framebuffer
Target          ESP-IDF v6.0.2 + ST7305 SPI flush; HTTP debug snapshot (PBM/BMP)
UI              Serial CLI (USB-Serial/JTAG): wifi / ntp / tz / sensor / http
Asset format    stock u8g2 fonts (custom BDF pipeline not yet built)
SD role         config file + alarm time / sound-effect storage; REST /fs/ upload (§8.6)  [not built]
```

Core architectural principle: first write it as a portable 400×300 monochrome
instrument renderer, make SDL3 the first display backend, and only then wire
ESP32/ST7305 in as the hardware backend. The renderer and time model are now
compiled verbatim from `common/` into the firmware (§9).
