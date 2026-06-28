# RLCD Time Scale Monitor — Implementation Notes

Design reference for a Waveshare ESP32-S3 RLCD 4.2 board acting as an NTP/RTC-based time-scale instrument. 400×300 reflective monochrome display showing ISO-8601 civil time, UTC, TAI-based MJD, GPS Week/TOW, sync state, temperature/humidity, battery and Wi-Fi status.

---

## 1. Hardware

- ESP32-S3 (Xtensa LX7 dual-core, 240 MHz), 512 KB SRAM, 16 MB Flash, 8 MB PSRAM
- 2.4 GHz Wi-Fi, BT 5 LE
- 4.2" fully reflective RLCD, landscape **400×300 px**
- PCF85063 RTC, SHTC3 temp/humidity, 18650 battery holder, MicroSD
- KEY/BOOT side buttons (limited usable UI keys), 2×8 2.54 mm expansion header

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

Hardcode current offsets; no leap-second table (current and future display only):

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
│ [tmp] +28.4C  [rh] 61%  [bat] 3.91V   │   telemetry
│ ntp +0s   wifi -57 dBm                 │   sync age / RSSI
└────────────────────────────────────────┘
```

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

400×300 landscape window, integer scaling, 1-bit framebuffer preview, fake ClockModel injection, keyboard page/state switching, screenshot/PBM export.

Keyboard: `1/2/3` page, `D` dense/large toggle, `N` cycle sync state, `W` Wi-Fi, `B` battery, `S` screenshot, `Q` quit.

### 7.4 Test states

normal / holdover / unsync / lowbat / old NTP age / MJD fractional rollover / GPS TOW rollover / GPS week rollover / UTC-local date crossing / ISO week across year boundary / status text overflow / temperature and humidity edge values.

### 7.5 Framebuffer padding

400×300/8 = 15000 bytes, but 300 is not a multiple of 8, so the internal buffer may round up to 400×304/8 = 15200 bytes. Both host and target support `visible 400×300 / buffer 400×304`; layout/render code must not hardcode the framebuffer byte layout.

---

## 8. Firmware architecture

### 8.1 Arduino ESP32 core

Start with the Arduino ESP32 core (rich ecosystem, FreeRTOS underneath, direct access to task/queue/mutex); can migrate to an ESP-IDF component later.

### 8.2 FreeRTOS tasks

```text
display_task    1-2 Hz: read ClockModel snapshot → render → ST7305 flush
time_sync_task  Wi-Fi/SNTP/sync age/trust state; update system time / RTC on success
sensor_task     low-rate SHTC3 / battery ADC / Wi-Fi RSSI (1~10s)
alarm_task      compare local time against SD-loaded alarm schedule; trigger audio on match
serial_task     CLI/debug/config; may block on Serial
button_task     debounce; short press = dismiss ringing alarm, long press = force resync
```

`loop()` stays empty or just yields.

### 8.3 ClockModel snapshot

Tasks do not mutate renderer state directly. Use a global model + mutex; the display task takes a snapshot each frame:

```cpp
struct ClockModel {
    int64_t unix_ms;
    bool time_trusted, ntp_ok, rtc_holdover, wifi_ok;
    uint32_t ntp_age_s;
    int wifi_rssi_dbm;
    float temp_c, rh_pct, batt_v;
    bool alarm_ringing;
};
```

Display task: `xSemaphoreTake` → `snapshot = g_model` → `xSemaphoreGive` → `clearBuffer` → `render_current_face(u8g2, snapshot)` → `sendBuffer`. The renderer must switch to placeholders based on `time_trusted` (see §1).

### 8.4 Serial console

With only one usable on-device key, primary setup/debug goes over a Serial CLI: `status / time / sync / wifi status / rtc read|write-system-time / sensor read / set tz|tai-utc|gps-utc|refresh / sd read|write-config / alarm set|list|disable / reboot / help`.

---

## 9. Project layout

```text
rlcd-time-scale-monitor/
  docs/                 design.md, time-scale-notes.md, display-backend.md
  assets/fonts/         *.bdf
  generated/            u8g2_font_*.c
  src/app/              clock_model.h, time_model.{h,cpp}, render_faces.{h,cpp},
                        face_main.cpp, face_scales.cpp, face_system.cpp,
                        serial_console.{h,cpp}
  src/backend/sdl3/     main_sdl3.cpp, u8g2_sdl3_backend.{h,cpp}
  src/backend/esp32/    main_arduino.cpp, st7305_backend.{h,cpp},
                        wifi_ntp.{h,cpp}, pcf85063.{h,cpp}, shtc3.{h,cpp}, battery.{h,cpp}
  tools/                build_fonts.sh, dump_screenshot.py
  tests/                test_time_model.cpp, golden_screenshots/
```

---

## 10. Rendering API

Render code uses the u8g2 API directly; no large Canvas abstraction:

```cpp
void render_frame(U8G2& g, const ClockModel& m) {
    g.clearBuffer();
    switch (m.page) {
        case 0: render_face_main(g, m); break;
        case 1: render_face_scales(g, m); break;
        case 2: render_face_system(g, m); break;
    }
    g.sendBuffer();
}
```

Wrap a few helpers to isolate platform details: `draw_text_right`, `draw_label_value`, `draw_status_icon_text`.

---

## 11. Milestones

1. **Host simulator skeleton** — SDL3 400×300 window + scaling, fake ClockModel, u8g2 render path, screenshot export
2. **Time model** — Unix ms → local/UTC/MJD(TAI)/GPS week/TOW/ISO week, offset constants, edge-case tests
3. **Fonts and icons** — BDF fonts + icon font, bdfconv build script, shared generated arrays for host/target
4. **Main face layout** — single-face layout, screenshot export, golden screenshot review
5. **ESP32 bring-up** — Arduino skeleton, u8g2 target backend, ST7305 init/flush, display_task, Serial CLI
6. **Time sync and RTC** — Wi-Fi/SNTP sync, NTP age, PCF85063 read/write, boot unsafe / RTC holdover state
7. **Sensors and telemetry** — SHTC3, battery ADC, Wi-Fi RSSI
8. **SD storage** — mount, read config file (TZ / offsets / alarm times), load alarm sound effects
9. **Polish** — low battery / Wi-Fi lost state, screenshot regression, power behavior tuning

---

## 12. Non-goals and SD role

Not building: LVGL GUI, voice recognition, Bluetooth UI, precision RTC calibration, leap-second historical table, complex on-device menu.

**SD card role**: store a config file (TZ offset, TAI/GPS offset overrides, alarm times) and alarm sound-effect samples. Not a general log sink in the first version.

Future extensions: RTC drift measurement, Wi-Fi captive config portal, web UI, SD-based logging, ham radio page (poll solar conditions / HF propagation info).

---

## 13. Design summary

```text
Product         RLCD Time Scale Monitor
Display         400×300 landscape monochrome
Primary source  Wi-Fi SNTP/NTP
Holdover        PCF85063 RTC / ESP32 system time
Main fields     local time, UTC, MJD(TAI), GPS week/TOW
Aesthetic       fixed-width time-scale telemetry instrument
GUI framework   no LVGL
Drawing engine  u8g2
Host preview    SDL3 backend presenting u8g2 framebuffer
Target          Arduino ESP32 core + FreeRTOS + ST7305 flush
UI              Serial CLI + one-button (resync / dismiss alarm)
Asset format    u8g2 font format for text and icons
SD role         config file + alarm time / sound-effect storage
```

Core architectural principle: first write it as a portable 400×300 monochrome instrument renderer, make SDL3 the first display backend, and only then wire ESP32/ST7305 in as the hardware backend.
