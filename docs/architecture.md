# RLCD Time Scale Monitor — Architecture

This document describes the current firmware and rendering contract. Use
[README](../README.md) for build commands and the HTTP/CLI file interface,
[hardware notes](hardware_notes.md) for board wiring, and the
[implementation notes](../rlcd_time_scale_monitor_implementation_notes.md)
for remaining work and agreed scope.

## Shared model and display

The host simulator and ESP32 firmware compile `common/time_model.c`,
`common/render_faces.c`, and `common/frame_export.c` from the same sources.
Platform glue fills `clock_model_t`; u8g2 draws the single 400×300 face.
SDL3 presents the host buffer, while the target flushes to the ST7305 over
SPI. The target snapshot handler maps the panel-oriented buffer back to the
host layout before calling the shared PBM/BMP encoder. The framebuffer mutex
serializes display refresh and HTTP snapshot rendering.

The face shows local and UTC civil time, UTC ISO week, MJD on the TAI scale,
GPS week/time-of-week, sync state, and telemetry. Time-scale arithmetic uses
integer milliseconds. TAI−UTC comes from a verified leap-seconds.list table
when one is installed, otherwise from the built-in current-era value of 37
seconds; GPS−UTC is always TAI−UTC − 19 seconds. Local time comes from a
POSIX TZ rule evaluated at each frame's instant, so daylight-time rules apply
without zoneinfo files. Both values are carried in `clock_model_t`. The date/time
fields are masked whenever `time_trusted` is false. `RTC HOLD` on the face
means trusted system-clock holdover, which may have been seeded from the
external RTC at boot; it does not imply continuous reads from the RTC.

SHTC3 temperature/humidity and the battery ADC have separate validity flags
from time trust. A failed SHTC3 read may reuse its last good sample for up to
10 seconds of monotonic time, then renders `n/a`. A failed ADC or calibration
read renders battery voltage as `n/a` immediately. No invalid raw value is
presented as a measured zero.

## Time acquisition and trust

Wi-Fi has three modes. AUTO, the boot default, scans whenever there is no
active connection and joins the strongest visible network listed in
`secrets/wifi.json`, trying the next one on failure. If no known network
joins, it rescans after 10 seconds, doubling up to 5 minutes. A connection
drop rescans on the next tick. A live link is not switched to a stronger
network. Hidden SSIDs are not supported. `wifi connect` selects MANUAL mode
(one network, retried with a 1–30 s backoff), and `wifi disconnect` selects
OFF. Both last until `wifi reset` or reboot. A changed `secrets/wifi.json`
keeps the current AUTO link only if its SSID and password are still listed;
otherwise the manager rescans. Credentials are never written to NVS
(`WIFI_STORAGE_RAM`), and passwords are not logged. CLI overrides are
RAM-only.

SNTP server
selection is manual CLI override, then the selected config's `ntp_server`,
then DHCP option 42, then `pool.ntp.org`. A DHCP server that fails to sync
within about 18 seconds yields to the pool and is retried every five minutes.
SNTP callback history uses monotonic time: the active source is fresh for
two hours after a successful sync; system time remains trusted as holdover for
less than 24 hours. Changing server or reconnecting does not erase a still
valid last-good sync.

The PCF85063A is written after SNTP synchronization by a separate task, so
the lwIP callback does not perform I²C or NVS writes. NVS holds a bounded
last-sync checkpoint; no user settings are stored there. At boot, the RTC may
seed system time only when its oscillator-stop flag, calendar and application
marker are valid and its time is less than 24 hours after that checkpoint.
This is trusted holdover but is not counted as an SNTP sync or a fresh source.
If the RTC is unavailable or invalid, boot time remains untrusted until SNTP
succeeds. Full power-loss behavior without an RTC backup cell is documented in
[hardware notes](hardware_notes.md#rtc-power-and-verification-boundary).

## Storage and configuration

The 16 MiB flash has a 4 MiB factory app and an 8 MiB FAT partition with
wear levelling. SD and internal flash mount at `/sdcard` and `/flash`; normal
mounts never format either volume. Both volumes contain managed `config/`
JSON versions, `sounds/` WAV files, an optional `time/leap-seconds.list`
and an optional write-only `secrets/wifi.json`. The HTTP file API and CLI storage
operations share storage ownership. The HTTP worker serializes transfers and
stages replacement through the private `.rlcd-txn` journal so an interrupted
upload can be recovered; this is not a guarantee against FAT metadata damage.

Config filenames are compared bytewise in descending order. The first
readable, valid SD version wins; only if SD has no usable version is flash
searched the same way. If neither has one, defaults apply. Uploading a config
requires a new filename and preserves older versions. The applied keys are:

- `tz`: a POSIX TZ rule such as `CST-8` or `CET-1CEST,M3.5.0,M10.5.0/3`.
  Offsets count west of UTC, as POSIX defines them, and must be whole minutes
  within ±14 hours. A daylight-time rule must name both transition dates.
- `tz_offset_minutes`: the legacy integer offset east of UTC (−840..840),
  used only when `tz` is absent. If both keys are present, both must be valid.
- `ntp_server`: optional hostname or IPv4 address.
- `audio_volume`: integer 0–100 (default 80) used for playback unless the
  RAM-only `audio volume` CLI override is set; `audio volume reset` returns
  to it.

Without a usable config the rule is `<+08>-8` (UTC+8). Unknown keys are
ignored. CLI timezone and NTP overrides take priority until reset. The current file manager edits JSON by creating a new
timestamped version; it does not provide per-setting forms.

Config cleanup (`config cleanup`, `GET`/`POST /fs/{sd,flash}/cleanup`, or
the file manager button) works on one volume at a time. It keeps the version
that volume would select on its own and deletes only versions whose names
sort before it. Newer, necessarily invalid versions remain for repair. A
volume without a valid version is left untouched. The GET form and the CLI
without `confirm` only list the files.

`secrets/wifi.json` is a single unversioned file listing up to 8 networks:
`{"networks": [{"ssid": "...", "password": "..."}]}`. SSIDs are 1–32 bytes
and unique. A password is omitted or empty for an open network, 8–63
printable ASCII characters, or 64 hex digits. The first valid file on SD,
then flash, is used; the lists are not merged. Over HTTP the `secrets/`
directory is write-only: PUT replaces the file through the journal after
validation, DELETE removes it, listings show only the name and size, and GET
on the file returns 403. Uploads, deletes, config reloads and volume mount
changes reload it. The file is plaintext on FAT, so anyone with serial
access or the SD card can read it.

`time/leap-seconds.list` uses the IERS/IETF format. A table is used only
when its `#$` update, `#@` expiry and `#h` SHA-1 lines are present, the hash
matches, and every entry changes TAI−UTC by one second at a later instant.
The hash detects file damage; it does not authenticate the source. Among the
verified SD and flash tables, the latest `#$` update wins, with SD winning a
tie; with none, the built-in value applies. Unlike config versions, an upload
replaces the file through the same journal after verification. Uploads,
deletes, `leap reload`, config reloads and volume mount changes reselect the
table. An expired table keeps its last TAI−UTC value. `/status` and
`leap status` judge expiry only against trusted time; the face does not yet
mark an expired or missing table.

## Audio playback

Control: CLI `audio play [sd|flash] "<file>" [loop] | stop | volume | status`,
or HTTP `GET /audio`, `POST /audio/play` with
`{"file": "...", "storage": "sd"|"flash", "loop": bool}`, `POST /audio/stop`
and `POST /audio/volume` with `{"level": 0..100}` or `{"reset": true}`. The
file manager offers Play, Loop and Stop buttons. These requests only queue
work. A play is answered with 202, and open or format errors then appear in
`GET /audio`. Like the rest of the LAN API, these endpoints are
unauthenticated.

Sound files live in `sounds/`, at most 64 MiB each, and must be mono or
stereo at 8–48 kHz.

- `.wav`: 16-bit linear PCM (WAVE_FORMAT_PCM, or EXTENSIBLE with the PCM
  subformat).
- `.flac`: native FLAC (not Ogg) at any bit depth. dr_flac converts samples
  to 16 bits by keeping the top 16, without dither. dr_flac is tracked as
  the `contrib/dr_libs` submodule, pinned to an upstream master commit that
  includes the parsing and seek fixes made after the `flac-0.13.3` tag.

`common/audio_source.c` presents both formats as interleaved 16-bit frames.
The same code validates uploads by opening the file and decoding its first
block, and decodes during playback. dr_flac's allocations exceed the 16 KiB
internal-RAM threshold, so they land in PSRAM. Playback uses ES8311 on I²S0 (TX only, Philips standard format,
MCLK = 256 × fs) through `esp_codec_dev`. `esp_codec_dev` turns the speaker
PA on at open and off at close, and reconfigures the I²S clock for each
file's sample rate.

The player has two tasks. The reader decodes into 8 KiB chunks. Its I/O
callbacks hold the storage mutex only around each `fread` or `fseek`, never
while decoding. It fills a 512 KiB PSRAM stream buffer (about 3 s of 44.1 kHz
stereo). The higher-priority writer drains that buffer to the codec; an
empty buffer counts as an underrun, and the auto-cleared DMA plays silence.
Long HTTP transfers release the mutex after every 4 KiB chunk so the reader
can refill. Each storage mount, unmount or format increments a generation
counter, and a transfer that sees it change abandons its open file. Before
unmounting or formatting a volume, or deleting or replacing a file, storage
owners call `audio_mgr_release_locked()`. The reader closes that file, and
the audio already buffered still plays.

Playback is either single or looped. A loop rewinds to the first frame with
no gap: a WAV seeks to its data chunk, a FLAC calls
`drflac_seek_to_pcm_frame(0)`. It and stops after exactly 10 minutes of audio
(`AUDIO_LOOP_LIMIT_MS`), even mid-pass. A single play is never cut. A loop
request for a file of 10 minutes or longer plays it once, uncut. Volume 0
mutes. Volumes 1–100 map
linearly to −40…0 dB, replacing the library's −50…0 dB curve, which made
mid-range settings very quiet. `esp_codec_dev` then subtracts about 2.4 dB
of PA-gain compensation (6 dB PA gain, 3.3 V DAC into a 5 V PA), so 100 sets
the DAC to about −2.4 dB. Alarm scheduling and buttons are not implemented; see
[remaining work](../rlcd_time_scale_monitor_implementation_notes.md).
