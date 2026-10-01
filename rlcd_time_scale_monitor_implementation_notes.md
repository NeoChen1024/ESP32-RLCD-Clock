# RLCD Time Scale Monitor — Implementation Notes

This is the active scope and progress tracker for the Waveshare ESP32-S3
RLCD 4.2 time-scale instrument. The current product behavior and data flow
are described in [architecture](docs/architecture.md); board facts and the
RTC power boundary live in [hardware notes](docs/hardware_notes.md). Use the
[README](README.md) for build and usage instructions.

## Current state

The single 400×300 face and SDL3 host simulator are working. The firmware
renders that face on the ST7305, acquires trusted time through Wi-Fi SNTP,
supports bounded PCF85063A boot holdover, and shows SHTC3, battery and Wi-Fi
telemetry. Storage is available on SD and internal wear-levelled FAT; the
English HTTP file manager and API manage versioned JSON config and WAV files.
Config selection is SD-first with fallback to older valid versions and then
internal flash. POSIX TZ rules and NTP-server keys are applied, and a
verified `time/leap-seconds.list` supplies TAI−UTC.

## Remaining work

| Area | Next work | Status |
| --- | --- | --- |
| Leap table presentation | Decide how the face marks an expired or missing leap table, and adjust the layout around the unused band between the GPS row and telemetry. | Open |
| Events | Implement the agreed [events design](docs/events.md): per-file `events/` rules, ringing and display-only events, upcoming list on the face, button dismissal. | Next |
| RTC power and drift | Verify backup operation across a true power loss and measure RTC drift before considering calibration. The current test board has no RTC backup battery available. | Deferred until hardware is available |
| Visual assets | Replace stock u8g2 fonts/text placeholders with a small shared font/icon asset set if the current face needs it. | Planned |
| Validation and power | Add representative host↔target screenshot parity cases, injected sensor-failure coverage on hardware, low-battery presentation and power-behavior measurements. | Planned |

## Agreed design boundaries

- Keep one fixed u8g2 face shared by host and target; SDL3 presents the host
  framebuffer. LVGL and a separate SDL text renderer are outside scope.
- Keep lwIP SNTP rather than implementing full RFC 5905 NTP or xleave. The
  face updates at 1 Hz. Time is TRUSTED for 24 hours after a sync, then
  RTC_HOLD while the RTC cross-check passes; only INVALID time masks the
  face or stops events.
- Use HTTP web page + file API for SD/internal-flash file management. FTP is
  not implemented. New config versions preserve older files; selection is
  based on descending filename order, with SD preferred over flash.
- Wi-Fi credentials live only in the write-only `secrets/wifi.json`. They are
  never stored in NVS or served over HTTP. CLI overrides stay in RAM. Other
  user config belongs in versioned files, while NVS stores only the RTC
  last-sync checkpoint.
- Use a 4 MiB factory app and 8 MiB internal FAT partition on the 16 MiB
  flash. There are no OTA slots in the current layout.
- Time zones use POSIX TZ rule strings, not zoneinfo/TZif files. Leap
  seconds use the IERS `leap-seconds.list` format, uploaded through the file
  API.
- There is no GNSS/PPS source, Bluetooth UI, voice recognition or complex
  on-device menu in the current scope.
