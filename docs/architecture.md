# RLCD Time Scale Monitor — Architecture

This is an overview of how the pieces fit together. Each subsystem has one
detailed contract document:

| Document | Covers |
| --- | --- |
| [time.md](time.md) | Time scales, local time, SNTP sources, the INVALID/TRUSTED/RTC_HOLD time state, RTC, display labels |
| [storage.md](storage.md) | Volumes, managed files, selection rules, config versions and cleanup, journal, HTTP file API |
| [audio.md](audio.md) | WAV/FLAC formats, playback pipeline, loops, volume, control |
| [network.md](network.md) | Wi-Fi modes, known-network secrets, HTTP endpoints |
| [events.md](events.md) | Scheduled events and alarms (agreed design, not implemented) |
| [hardware_notes.md](hardware_notes.md) | Wiring, panel behavior, RTC power boundary |

Build and usage instructions are in the [README](../README.md). Remaining
work and agreed scope are in the
[roadmap](roadmap.md).

## Shared code

`common/` holds plain C that the host simulator and the firmware compile
from the same sources:

- **Display:** `time_model` (time-scale arithmetic), `render_faces` (the
  single 400×300 u8g2 face), `frame_export` (PBM/BMP).
- **Time:** `tz_rule`, `leap_table` with `sha1`, `clock_health` (time
  state and SNTP policy), `rtc_clock`.
- **Storage and audio:** `storage_files` (managed paths, validation,
  journal), `wav_format` and `audio_source` (WAV/FLAC decoding).
- **Telemetry:** `sensor_health`.

Platform glue fills `clock_model_t` through `time_model_now()`: the host
uses its system clock and local zone, the target its SNTP/RTC time, TZ rule
and leap table. Everything in `common/` is covered by host tests (`ctest`).
Firmware managers that need ESP-IDF run against a fake IDF in the host
tests where practical.

SDL3 presents the host framebuffer. The target flushes the same u8g2 buffer
to the ST7305 over SPI; the panel's inverted polarity is handled only in the
ST7305 tile callback. The target snapshot handler maps the panel-oriented
buffer back to the host layout before the shared encoder runs, so host and
target exports are byte-comparable.

## Firmware tasks

| Task | Priority | Role |
| --- | --- | --- |
| `audio_out` | 7 | Feeds decoded PCM to the codec ([audio](audio.md#pipeline)) |
| `display` | 5 | Builds the model and renders once per second, aligned to the wall-clock second, bounded by monotonic time across clock steps |
| `audio_in` | 5 | Reads and decodes sound files into the PSRAM stream buffer |
| `httpd` | 5 | HTTP server; file transfers are handed to `http_files` |
| `http_files` | 4 | Serialized file transfers, one in flight and one queued |
| `rtc_writer` | 4 | RTC writes after SNTP, and the per-minute RTC cross-check |
| `tcpip_thread` | IDF | lwIP, SNTP and the DHCP option-42 capture; all `sntp_mgr` state changes run here |
| default event loop | IDF | Wi-Fi state machine, scans and retries |
| console | main | USB-Serial/JTAG REPL (`linenoise` in dumb mode) |

Boot order (`app_main`):

1. NVS, Wi-Fi, SNTP, sensors (the shared I²C bus), RTC.
2. The display task.
3. Storage mount, then config, leap table and known networks.
4. Audio, then HTTP.
5. The CLI.

## Shared state and locks

- **Storage mutex:** all file access and mount changes. Long holders yield
  it between chunks and check the mount generation; see
  [storage](storage.md#ownership).
- **Framebuffer mutex:** serializes display refresh and HTTP snapshot
  rendering.
- **Model spinlock:** guards the TZ rule and leap table. Short copies only;
  writers are config reloads and the CLI, readers are the display, HTTP and
  CLI.
- **Status snapshots:** Wi-Fi status is mutex-guarded. SNTP status is taken
  on `tcpip_thread`. Audio status is spinlock-guarded.

## Per-second data flow

1. The display task wakes at the next wall-clock second.
2. `time_model_now()` collects the following into `clock_model_t`:
   - the SNTP/RTC time snapshot and its time state;
   - the Wi-Fi state, and the local offset from the TZ rule at that
     instant;
   - TAI−UTC from the leap table;
   - SHTC3 and battery readings, each with its own validity flag.
3. `render_faces` draws the face, masking time fields while the time state
   is INVALID.
4. The ST7305 backend flushes the frame.

SHTC3 readings are CRC-8 checked. Battery voltage is read on ADC1 channel 3
through a 3× divider, and RSSI comes from `esp_wifi_sta_get_ap_info`.
Telemetry validity is independent of time validity:

- A failed SHTC3 read may reuse the last good sample for up to 10 s of
  monotonic time, then shows `n/a`.
- A failed ADC read shows `n/a` immediately.
- No invalid raw value is presented as a measured zero.
