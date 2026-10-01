# Audio

This is the contract for sound playback. Pins and the codec's I²C address
are in [hardware notes](hardware_notes.md#board-and-connections). File placement and
validation rules are in [storage](storage.md#managed-files).

## Formats

Sound files live in `sounds/`, at most 64 MiB each. They must be mono or
stereo at 8–48 kHz, the ES8311/I²S range used here.

- `.wav`: 16-bit linear PCM (WAVE_FORMAT_PCM, or EXTENSIBLE with the PCM
  subformat). Unknown chunks are skipped.
- `.flac`: native FLAC (not Ogg) at any bit depth. dr_flac converts samples
  to 16 bits by keeping the top 16, without dither.

`common/audio_source.c` presents both formats as interleaved 16-bit frames.
The same code validates uploads by opening the file and decoding its first
block, decodes during playback, and runs in host tests. FLAC decoding uses
dr_flac from the `contrib/dr_libs` submodule. It is pinned to an upstream
master commit that includes the parsing and seek fixes made after the
`flac-0.13.3` tag. dr_flac's allocations exceed the 16 KiB internal-RAM
threshold, so they land in PSRAM.

## Pipeline

Playback uses the ES8311 on I²S0 through `esp_codec_dev`: TX only, Philips
standard format, MCLK = 256 × fs. `esp_codec_dev` turns the speaker PA on at
open and off at close, and reconfigures the I²S clock for each file's sample
rate.

The player has two tasks:

- **Reader** (`audio_in`, priority 5): decodes into 8 KiB chunks and fills a
  512 KiB PSRAM stream buffer, about 3 s of 44.1 kHz stereo. Its I/O
  callbacks hold the storage mutex only around each `fread` or `fseek`,
  never while decoding.
- **Writer** (`audio_out`, priority 7): drains the stream buffer to the
  codec. An empty buffer counts as an underrun, and the auto-cleared DMA
  plays silence.

Storage changes cannot strand the player. Long HTTP transfers yield the
storage mutex every chunk. Before an unmount, format, delete or replace
touches the playing file, storage calls `audio_mgr_release_locked()`. The
reader then closes the file, and the audio already buffered still plays.
See [storage ownership](storage.md#ownership).

## Single and looped playback

- A **single** play is never cut.
- A **loop** rewinds to the first frame with no gap: a WAV seeks back to its
  data chunk, a FLAC calls `drflac_seek_to_pcm_frame(0)`. A loop stops after
  exactly 10 minutes of audio (`AUDIO_LOOP_LIMIT_MS`), even mid-pass.
- A loop request for a file of 10 minutes or longer plays it once, uncut.

## Volume

Volume 0 mutes. Volumes 1–100 map linearly to −40…0 dB, replacing the
library's −50…0 dB curve, which made mid-range settings very quiet.
`esp_codec_dev` then subtracts about 2.4 dB of PA-gain compensation (6 dB PA
gain, 3.3 V DAC into a 5 V PA), so 100 sets the DAC to about −2.4 dB.

The effective volume is the RAM-only CLI or HTTP override if one is set,
otherwise the config's `audio_volume` (default 80). `audio volume reset`
returns to the config value.

## Control

| Interface | Operations |
| --- | --- |
| CLI | `audio play [sd\|flash] "<file>" [loop]`, `audio stop`, `audio volume [0-100\|reset]`, `audio status` |
| HTTP | `GET /audio`; `POST /audio/play` `{"file", "storage", "loop"}`; `POST /audio/stop`; `POST /audio/volume` `{"level"}` or `{"reset": true}` |
| Web | Play, Loop and Stop audio in the `/files` file manager |

These requests only queue work. A play is answered with 202, and open or
decode errors then appear in `GET /audio` (`error`). The status reports the
file, format, bit depth, sample rate, channels, loop state, position,
elapsed time, duration, volume and its source, and the underrun count. Like
the rest of the LAN API, these endpoints are unauthenticated.

Scheduled playback is specified in [events](events.md); it is not
implemented yet.
