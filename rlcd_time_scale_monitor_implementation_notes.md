# RLCD Time Scale Monitor — Implementation Notes

Design reference for a Waveshare ESP32-S3 RLCD 4.2 time-scale instrument. It
currently uses Wi-Fi SNTP with bounded ESP32 system-clock holdover; PCF85063
RTC integration is planned. The 400×300 reflective monochrome display shows
ISO-8601 civil time, UTC, TAI-based MJD, GPS Week/TOW, sync state,
temperature/humidity, battery and Wi-Fi status.

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

During the original bring-up no RTC backup battery was installed; its current
physical state has not been rechecked. Main power comes from the 18650, and
Wi-Fi/SNTP can re-acquire trusted time after boot. Without backup power, a
removed or depleted 18650 would also let the PCF85063 lose time. Firmware
therefore masks precise MJDTAI/GPS values until a trusted sync; reading the
PCF85063 itself remains future work.

Sync state codes:

```text
BOOT UNS   booted, trusted time not yet acquired
SYNC...    syncing
NTP OK     SNTP/NTP synced, time trusted
RTC HOLD   relying on RTC or system clock holdover
WIFI LOST  Wi-Fi down, still within trusted system-clock holdover
TIME UNS   last successful sync is too old (24 h)
```

When unsynced or trust has expired, mask local date/time, UTC, ISO week,
MJDTAI and GPS — never fake-precise values:

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
everything at once. Debug/config uses the Serial console and the HTTP file
manager (§8.6).

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

When `time_trusted` is false, all date/time fields are masked: `TIME UNSYNC`,
`--:--:--`, UTC/ISO placeholders, `--------.-------`, and `W---- TOW------`.
The SNTP callback records the last successful sync using monotonic time;
reading status does not consume a sync event. Current policy constants live
in `common/clock_health.h`: recent sync <2 h, trusted holdover <24 h. These
limits are product policy, not an accuracy guarantee.

`NTP OK` requires the current source to have synced recently and the network
to be up. Otherwise trusted system time shows `RTC HOLD` (network up) or
`WIFI LOST` (network down). At 24 h it becomes `TIME UNS` and fields are
masked. `RTC HOLD` currently means ESP32 system-clock holdover; PCF85063 is
still unimplemented. Manual resync/server changes preserve last-good trust
while waiting for the new source. Cold boot remains unsafe until first sync.
ISO week uses the UTC date; the top-bar civil date uses the local offset.

### 4.2 Button behavior

The following button behavior is planned, not implemented. Configuration
versions can already be managed over HTTP; diagnostics also use the Serial
console. With the single-face design, the on-device keys are reserved for:

- **short press**: dismiss a currently ringing alarm (snooze/disable)
- **long press**: force NTP resync

Alarm scheduling and button handling have not been implemented.

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

- **No LVGL**: no touch, no complex on-device UI, no widget/theme/animation needs; settings/debug use Serial and HTTP, while the screen is a fixed instrument panel. LVGL's object tree, style system and event routing would be mostly overhead.
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

- The board's only host-facing serial port is the **USB-Serial/JTAG** controller, not UART0. The path was `/dev/ttyACM0` during 2026-09-23 testing, but can change; discover it via `/dev/serial/by-id`. Console must use `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, and the CLI must call `linenoiseSetDumbMode(1)` — linenoise's escape-sequence probe hangs the USB-serial VFS otherwise.
- picolibc's `struct tm` has no `tm_gmtoff`, so the host's TZ readout cannot be reused on target; TZ comes from the selected config or a RAM-only CLI override (see §8.4).

### 8.2 Firmware subsystems and tasks

```text
display_task    1 Hz: render shared face → ST7305 flush (ink inversion in backend) [implemented]
wifi_mgr        event-driven STA connect/disconnect; auto-reconnect  [implemented]
sntp_mgr        lwIP SNTP; CLI > selected SD/flash config > DHCP > pool [implemented]
storage/config  SD + internal FAT, version selection and TZ/NTP application [implemented]
http_srv        esp_http_server: homepage, files API, status, snapshots [implemented]
sensor frontend SHTC3 + battery ADC, read on each display frame     [implemented]
CLI             linenoise REPL over USB-Serial/JTAG console          [implemented]
alarm_task      compare local time against SD-loaded alarm schedule; trigger audio on match  [future]
button_task     debounce; short press = dismiss ringing alarm, long press = force resync     [future]
```

`app_main` initializes subsystems, mounts storage and selects a config, then
the CLI REPL blocks (commands run in the console task). The display task is
the only periodic renderer; SNTP, Wi-Fi and HTTP are event/task-driven. The
HTTP file API uses a separate storage worker. SNTP policy, DHCP capture, sync
callbacks and status snapshots are serialized on lwIP tcpip_thread. A linker
wrapper captures `dhcp_set_ntp_servers` (including option-42 removal) without
allowing DHCP to overwrite the active manual/fallback table. Wi-Fi retries
use event-loop commands and a periodic timer, with 1–30 s capped backoff;
manual disconnect cancels retries. `wifi reconnect` forces a link drop through
the same recovery path without re-entering RAM credentials.

### 8.3 ClockModel snapshot

The model is the shared `clock_model_t` (common/time_model.h, verbatim on both platforms):

```c
typedef struct {
    int64_t  unix_ms;
    bool     time_trusted;
    sync_state_t sync;        /* SYNC_BOOT_UNS / SYNC_SYNCING / SYNC_NTP_OK / SYNC_RTC_HOLD / SYNC_WIFI_LOST / SYNC_TIME_UNSAFE */
    uint32_t ntp_age_s;
    int      wifi_rssi_dbm;
    float    temp_c, rh_pct, batt_v;
} clock_model_t;
```

The display framebuffer is protected by a mutex; both the periodic display task
and the HTTP snapshot handler render through it:

`xSemaphoreTake` → `time_model_now(&m)` (device glue fills from SNTP + Wi-Fi + sensors) → `render_face(g, &m)` → `xSemaphoreGive` (display task additionally flushes). The renderer switches all date/time fields to placeholders based on `time_trusted` (see §1). `alarm_ringing` from the original design is not yet in the struct — the alarm feature is unbuilt.

### 8.4 Serial console

Primary setup/debug goes over the CLI (USB-Serial/JTAG console, `rlcd>`
prompt). Implemented commands:

```text
wifi connect "<ssid>" [password]   connect STA (quoted args; not persisted)
wifi status | wifi disconnect | wifi reconnect
ntp status | ntp server <host|ip> | ntp reset | ntp resync
tz [±HH:MM | ±HHMM | minutes | reset]      show/set local offset (default UTC+8)
config status | config reload               inspect/reselect SD/flash config
sensor                                   read SHTC3 + battery ADC live
sd status | mount | unmount | ls [dir] | cat <file> | test | format
flash status | mount | init
http status                              list snapshot/debug endpoints
help
```

**NTP server selection** (priority order): manual `ntp server` CLI override >
selected config JSON `ntp_server` key (SD first, then internal flash) > DHCP option 42
(`CONFIG_LWIP_DHCP_GET_NTP_SRV`) > pool.ntp.org fallback.
`ntp status` shows the selected source/address, `synced` (ever synced this
boot), `trusted`, `fresh`, and monotonic `age`. `/status` exposes these fields
and a `display_frames` counter incremented only after actual panel flushes.
DHCP servers are cached independently of the active SNTP table, so `ntp
reset` immediately restores config, DHCP or the pool, including after a manual
server change. An 18 s unsuccessful DHCP trial falls back to the pool; every
5 minutes the cached DHCP source is retried. A DHCP source that stops syncing
for 2 h also falls back. DHCP changes/removal on lease renewal are applied
without requiring a new GOT_IP event. Current sdkconfig has one SNTP slot;
a second fallback name is only installed when the configured capacity allows.
Only a selected config with an `ntp_server` key takes precedence over DHCP.

**TZ**: the selected config supplies `tz_offset_minutes` (default +480), not
the host OS — see §8.1 (picolibc has no `tm_gmtoff`). A CLI override is
RAM-only; `tz reset` returns to the selected config value.

Future commands from the original design (RTC read/write, alarm
schedule) are not yet implemented.

### 8.5 HTTP debug snapshot

Read-only HTTP endpoint on the same Wi-Fi the instrument uses for SNTP; renders a fresh frame on demand and serves it as a 1-bit bitmap so the device's actual rendering can be inspected from a browser or compared bit-for-bit against the host simulator.

| Endpoint              | Format                           | Purpose                                                                              |
| --------------------- | -------------------------------- | ------------------------------------------------------------------------------------ |
| `GET /snapshot.pbm` | `image/x-portable-bitmap` (P4) | Canonical raw framebuffer; diff against host PBM for pixel-exact host↔target parity |
| `GET /snapshot.bmp` | 1-bit BMP (top-down)             | Browser-friendly view (`<img>` / double-click)                                     |
| `GET /`             | English HTML page                | Styled live display preview, refreshed from `/snapshot.bmp`, with a link to `/files` |

Design rules:

- **Shared encoder**: `frame_export.{h,c}` (pure C, no SDL/hardware deps) converts the u8g2 vertical_top_lsb buffer to P4 PBM and 1-bit BMP, byte-for-byte identical on host and target. PBM: P4, `400 300`, MSB-first, row-major, 1 = ink. BMP: 1-bit, top-down (`biHeight` negative), palette {white, black}, rows padded to 4 bytes.
- **Fresh frame**: the handler locks the display framebuffer, renders the face, encodes, unlocks — never exposes a half-drawn frame.
- **Layout translation on target**: the ST7305's u8g2 buffer lives in the panel's native orientation (304-wide × 400-tall, U8G2_R1 rotation), while the face is drawn in logical 400×300 landscape. `snapshot.c` re-maps the physical buffer back to the host's 400×300 layout before encoding, so target exports stay byte-comparable with the host.
- **Read-only snapshot by construction**: no query params or config mutation. The separate, volume-aware file API (§8.6) handles writes; snapshot routes remain read-only.
- Serves only when Wi-Fi is up (STA); not required for instrument function.

### 8.6 Storage and HTTP file access (milestone 8)

Versioned config files and sound samples live on SD or internal flash and are
managed through the same HTTP file API. TZ and NTP keys are applied today;
TAI/GPS offset overrides and alarms are planned.

**Implemented SD foundation (2026-09-23)**: `storage_mgr.c` mounts FAT at
`/sdcard` using 1-bit SDMMC, CLK=38 CMD=21 D0=39, 20 MHz (vendor
`06_SD_Card` wiring). Mount failure is nonfatal and never formats the card.
Long filenames and media status checks are enabled. `sd status` reports
card/filesystem/cluster/space information; `sd ls [dir]` lists a directory;
`sd cat <file>` previews at most 4096 bytes. Paths are relative to `/sdcard`.
`sd test` exclusively creates a temporary file and verifies a 4096-byte
write/fsync/close/reopen/read round trip, then removes it. `sd unmount` and
`sd mount` support deliberate removal/reinsertion; automatic hotplug is not
implemented. All diagnostic operations share one storage mutex.

Hardware verification: SK32G SDHC, FAT32, 16 KiB clusters, 31,898,320,896
data bytes with 31,898,304,512 bytes free. Two 4096-byte self-tests passed;
temporary files were removed and free space was unchanged after unmount/remount.
Unmounted access and `../` paths were rejected. The originally empty test card
was later explicitly reformatted with `sd format` as authorized by the user;
ordinary mounts still never format a card.

**Transport selected: HTTP web page + API (2026-09-23).**

`GET /files` serves an English, responsive file manager: Flash/SD selection,
space readout, root/config/sounds browsing, multi-file selection/drop uploads,
download/delete, and versioned JSON editing. The clock preview at `/` links
to it and shares its stylesheet. TZ and NTP settings apply from the selected
config; WAV files are stored but not yet played.
FTP is not implemented.

Internal storage is an 8 MiB `data,fat` partition mounted at `/flash`, using
ESP-IDF wear levelling with 4096-byte sectors. The factory app is now 4 MiB.
Both normal SD and flash mounts disable auto-formatting. Explicit `flash init`
initializes an unmountable internal volume; explicit `sd format` erases and
reformats the SD volume with 16 KiB clusters. Neither operation is exposed
through HTTP. Unmount/format and HTTP file IO share the same storage mutex.

| Method | Path | Effect |
| --- | --- | --- |
| GET | `/fs/` | Volumes (`flash`, `sd`), mount status, total/free data bytes |
| GET | `/fs/active` | Selected config volume/path and configured TZ/NTP values (CLI overrides are separate) |
| GET | `/fs/<volume>/` | Virtual config and sounds directories |
| GET | `/fs/<volume>/config/` | JSON config versions (latest 256 names) |
| GET | `/fs/<volume>/sounds/` | WAV listing (at most 256 entries) |
| GET | `/fs/<volume>/<file>` | Stream a download |
| PUT | `/fs/<volume>/<file>` | Stream and validate a file; config versions require a new name |
| DELETE | `/fs/<volume>/<file>` | Delete one managed file; no recursive deletion |

Managed files: `config/<ASCII name>.json` (JSON object, <=16 KiB and nesting
<=16) and `sounds/<ASCII name>.wav` (RIFF/WAVE header and matching container
length, <=16 MiB). WAV validation does not imply codec/playback support;
FLAC uploads and decoding are not implemented.
File names may contain letters, digits, spaces, underscore, hyphen and dot;
sound names cannot start with dot/space. URI decoding rejects encoded NUL,
separators, traversal, drive prefixes, aliases and private transaction paths.
The API uses canonical case, accepts percent-encoded spaces and no query params.

Requests exceeding limits return 413, invalid JSON/WAVE 422, existing config
version 409, missing file 404,
unmounted/busy storage 503, insufficient free space 507. Uploads require
Content-Length; no chunked request uploads, multipart, Range or resume API.
A 4 KiB buffer bounds transfer memory. The async worker serializes file IO
with one additional request slot; uploads/downloads have a 120 s deadline.
The main HTTP task continues serving clock status and snapshots during transfers.

Recoverable replacement uses a reserved `.rlcd-txn` directory per volume:
write/fsync the target journal; stream/fsync/close the upload; validate;
rename old target to backup; rename upload to target; remove backup and
journal. Recovery restores a backup if the target is absent, or retains the
new target if already published; partial uploads are discarded. Journal
removal is last. Invalid or foreign journal state is not deleted automatically.
This is recovery from interrupted operations, not a guarantee against FAT
metadata corruption during power loss. The SD card is never formatted by
ordinary mount failure.

At boot and after a config upload/delete or volume mount change, filenames in
`config/` are sorted descending. The first readable, valid SD file is selected;
if none exists, internal flash is searched the same way. This preserves older
versions and falls back on corrupt or semantically invalid newer files. If
both volumes have no usable version, TZ defaults to UTC+8 and NTP uses
DHCP/pool. The currently supported JSON keys are integer
`tz_offset_minutes` (-840..840, default 480) and optional `ntp_server`
(hostname/IP). Unknown keys are ignored for forward compatibility; alarm and
time-scale offset application remain future work. The web editor creates a
new sortable UTC timestamp filename on save and retains the source version.

Hardware and browser validation (2026-09-23): explicit `sd format` produced
FAT32 with 16 KiB clusters; `flash init` mounted a wear-levelled FAT volume
with 8,278,016 usable bytes. Exact JSON round trips and 1 MiB WAV SHA-256
comparisons passed on both volumes. Invalid JSON/WAV and interrupted uploads
left the previous config intact; malformed paths, 16 KiB JSON limit, 507
capacity guard, 503 transfer queue guard, and SD-unmount isolation were
verified over HTTP. A slow WAV upload left `/snapshot.bmp` responsive in
0.296 s and the actual panel frame counter increased. Chrome rendering,
volume switching, directory browsing and JSON editing were exercised. After
reflashing/rebooting, test JSON and WAV bytes persisted on both volumes;
those transfer-test files were then deleted. Later config-version selection
and source precedence were verified on hardware, including HTTP 409/422, CLI overrides and SD
unmount/remount fallback. The browser editor created a new version and left
older files in place. A subsequent cold boot selected that latest SD version
before Wi-Fi was connected. Deleting only the test versions stepped through
older SD files, then internal flash, then UTC+8 defaults; the test versions
were removed afterward. Host CTest passed 7/7.

---

## 9. Project layout

```text
ESP32-RLCD/
  AGENTS.md             environment + bring-up notes
  common/               shared pure-C render, clock and storage code
    time_model.{h,c} render_faces.{h,c} frame_export.{h,c}
    clock_health.{h,c} storage_files.{h,c} display_geometry.h
  host/                 SDL3 simulator (build: cmake -S host -B host/build)
    src/                host-only platform code: main.c, host_time.c,
                        sdl3_backend.{h,c}, u8g2_selected_fonts.c
    tests/              seven host tests, including fake-IDF network and config cases
  firmware/             ESP-IDF target (idf.py -p <detected-port> build flash)
    main/               app_main.c, cli.c, wifi_mgr.{h,c}, sntp_mgr.{h,c},
                        model.{h,c}, sensors.{h,c}, display.{h,c}, render.{h,c},
                        snapshot.{h,c}, http_srv.{h,c}, http_files.{h,c},
                        storage_mgr.{h,c}, config_mgr.{h,c}, home.html,
                        files.html, web_style.css
    partitions.csv      4 MiB app + 8 MiB wear-levelled FAT
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
2. ✅ **Time model** — Unix ms → local/UTC/MJD(TAI)/GPS week/TOW/ISO week, offset constants, time-model tests; see reliability regression coverage below
3. ⚠️ **Fonts and icons** — uses stock u8g2 fonts (inr42/VCR_OSD/7x13) on both platforms; no custom BDF pipeline or icon font yet
4. ✅ **Main face layout** — single-face layout, screenshot export (host PNG/PBM/BMP; golden screenshot review not automated)
5. ✅ **ESP32 bring-up** — ESP-IDF v6.0.2 app, u8g2 as IDF component, ST7305 SPI init/flush (vendor backend), 1 Hz display task, CLI, HTTP debug snapshot. **Verified on hardware.**
6. ⚠️ **Time sync and RTC** — Wi-Fi/SNTP sync, NTP age, DHCP option-42 + manual + fallback servers all verified; boot-unsafe gating, system-clock holdover/expiry, reconnect and DHCP fallback/retry implemented; PCF85063 RTC read/write and RTC-backed boot holdover NOT built
7. ✅ **Sensors and telemetry** — SHTC3 (I2C, CRC-8 verified) + battery ADC (curve-fitted, divider ×3) + real Wi-Fi RSSI. **Verified on hardware** (31.7 °C / 53 %RH / 4.06 V / −43 dBm).
8. ⚠️ **Storage** — SD FAT, internal wear-levelled FAT, CLI mount/status/format diagnostics, English HTTP file page + GET/PUT/DELETE API and recoverable replacements implemented. Versioned SD-first config selection and TZ/`ntp_server` application are verified; time-scale offsets, alarms and sound loading remain pending (§8.6).
9. ⚠️ **Polish** — basic Wi-Fi-lost/unsafe visuals and trust-mask rendering regression implemented; low-battery warnings, full golden screenshots and power behavior tuning remain

Also done ahead of plan: **HTTP debug snapshot** (§8.5) shipped with milestone 5,
and the full host UI is ported to the target (§10). TZ is selectable from
versioned config or the CLI (§8.4).

**Roadmap decisions** (recorded so they are not re-litigated later):

- **Keep the SNTP client; do NOT move to a full NTP implementation or
  xleave (RFC 7822).** The instrument only displays 1 Hz seconds and
  minute/second alarms — SNTP's ±ms accuracy already exceeds the need by
  3-4 orders of magnitude. Full RFC 5905 (clock filter, Marzullo
  selection, clustering/combining, poll adaptation) has no portable
  embedded implementation to reuse — lwIP's sntp.c is SNTP-only and not
  extensible, so it would mean writing ~500-1000 lines of hard-to-verify
  precision code. xleave additionally requires server-side support
  (public pools and typical DHCP option-42 servers do not provide it —
  it would mean running our own chrony), and even then its sub-ms
  accuracy cannot materialize over Wi-Fi (802.11 contention/retry jitter
  is ms-scale; no MAC-layer timestamping). If sub-ms time is ever
  genuinely needed, the prerequisite is Ethernet + PTP, which is a
  different project. Cheap resilience instead: multi-server SNTP
  (esp_sntp already indexes servers + reachability) and RTC holdover
  (milestone 6).
- **NTP server priority**: manual CLI override > selected config JSON
  (SD first, then internal flash) > DHCP option 42 > pool.ntp.org fallback (§8.4).

**Reliability implementation (2026-09-23)**:

- Persistent Wi-Fi retry policy after initial connection, including explicit
  cancellation and serialized replacement credentials. No sleep in event handlers.
- Callback-owned sync history; status reads cannot trigger a false DHCP failure.
- Immediate manual/reset selection and periodic DHCP recovery trials.
- Monotonic sync age, explicit system holdover/expiry and complete unsafe masking.
- Second-boundary refresh waits bounded by a 1 s monotonic deadline, including
  forward/backward SNTP steps. Tick delays use `pdMS_TO_TICKS`.
- Host CTest: time model, frame export, clock-health/clock-step policy, actual
  firmware network managers against a fake IDF transport, and unsafe rendering.
  Simulated tests cover 24 h expiry and DHCP failure/recovery; they do not
  substitute for hardware/network timing verification.

Hardware validation on 2026-09-23 (`/dev/ttyACM0`, isolated Wi-Fi): DHCP
10.127.16.1 now synchronizes successfully; repeated status reads beyond 18 s
stay on DHCP. Manual unreachable server -> reset restores DHCP immediately;
resync and disconnect retain trusted holdover. `wifi reconnect` traversed the
real driver disconnect path, regained IP in about 3.6 s and synced again.
Backward-step hardware injection was inconclusive because JTAG reset/halt
interfered with execution; only the host regression verifies that case.
The debugger was stopped and the board restored to normal DHCP synchronization.

Remaining: PCF85063, sensor validity/staleness, time-scale offset config, alarm/button/audio,
full screenshot parity, power tuning. `firmware/partitions.csv` now allocates
a 4 MiB factory app at 0x10000 and an 8 MiB wear-levelled FAT partition at
0x410000 on the 16 MiB flash, preserving NVS/PHY offsets. Remaining flash is
unallocated; there are no OTA slots. Both internal flash and SD can hold the
managed configuration and sound files.

---

## 12. Non-goals and storage roles

Not building: LVGL GUI, voice recognition, Bluetooth UI, precision RTC calibration, leap-second historical table, complex on-device menu, full RFC 5905 NTP client or xleave (SNTP suffices — see Roadmap decisions in §11).

**Storage role**: both SD and internal flash hold config JSON and alarm sound
files. Neither is a general log sink in the first version. HTTP web file
management and the volume-aware API are implemented (§8.6).

Future extensions: RTC drift measurement, Wi-Fi captive config portal, richer
settings UI (the current `/files` page edits JSON versions but has no form for
individual settings), SD-based logging, ham radio page (poll solar conditions
/ HF propagation info).

Built ahead of the original plan: HTTP debug snapshot (§8.5), versioned
SD-first config selection, CLI-configurable TZ, DHCP option-42 NTP, and real SHTC3/battery telemetry — see milestone
status in §11.

---

## 13. Design summary

```text
Product         RLCD Time Scale Monitor
Display         400×300 landscape monochrome (RLCD: bit 0 = ink/black)
Primary source  Wi-Fi SNTP (CLI > SD/flash config > DHCP option 42 > pool fallback)
Holdover        ESP32 system time after sync (PCF85063 RTC not yet wired)
Main fields     local time, UTC, MJD(TAI), GPS week/TOW, ISO week
Aesthetic       fixed-width time-scale telemetry instrument
GUI framework   no LVGL
Drawing engine  u8g2 (csrc submodule, both platforms)
Host preview    SDL3 backend presenting u8g2 framebuffer
Target          ESP-IDF v6.0.2 + ST7305 SPI flush; HTTP debug snapshot (PBM/BMP)
UI              Serial CLI (USB-Serial/JTAG) + English HTTP homepage/file manager
Asset format    stock u8g2 fonts (custom BDF pipeline not yet built)
Storage         SD FAT + 8 MiB internal wear-levelled FAT; SD-first versioned TZ/NTP config and web file manager/API built; alarm/audio pending (§8.6)
```

Core architectural principle: first write it as a portable 400×300 monochrome
instrument renderer, make SDL3 the first display backend, and only then wire
ESP32/ST7305 in as the hardware backend. The renderer and time model are now
compiled verbatim from `common/` into the firmware (§9).
