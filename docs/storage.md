# Storage

This is the contract for volumes, managed files, selection rules and the
HTTP file API. The README has usage examples. The meaning of each file's
content lives with its subsystem: [time](time.md), [network](network.md),
[audio](audio.md) and [events](events.md).

## Volumes

The 16 MiB flash holds a 4 MiB factory app and an 8 MiB FAT partition with
wear levelling (`firmware/partitions.csv`). There are no OTA slots, and NVS
and PHY are retained.

| Volume | Mount | Notes |
| --- | --- | --- |
| `flash` | `/flash` | 4096-byte sectors. `flash init` explicitly initializes an unmountable partition. |
| `sd` | `/sdcard` | 1-bit SDMMC at 20 MHz. `sd format` (16 KiB clusters) erases the card. Unmount before removing; no hotplug. |

Ordinary mounts never format either volume.

## Ownership

One storage mutex covers all file access and mount changes: the HTTP
worker, CLI storage commands, config, leap and secrets reloads, and the
audio reader.

- **Audio reader:** holds the mutex only around each `fread` or `fseek`.
- **Long holders:** HTTP transfers release it after every 4 KiB chunk.
  Each mount, unmount or format increments a generation counter
  (`storage_generation_locked()`). A holder that sees it change abandons its
  open file.
- **Releasing the player's file:** before unmounting or formatting a volume,
  or deleting or replacing a file, storage owners call
  `audio_mgr_release_locked()`.

## Managed files

Only these paths are reachable through the API. Names use ASCII letters,
digits, space, `_`, `-` and `.`, and paths are case-sensitive.

| Path | Content | Size | Replacement | Selection |
| --- | --- | --- | --- | --- |
| `config/<name>.json` | JSON object, nesting ≤ 16 | 16 KiB | New file per version; no overwrite (409) | Newest valid name, SD before flash |
| `sounds/<name>.wav` / `.flac` | Decodable audio ([formats](audio.md#formats)) | 64 MiB | In place | Named by the player |
| `time/leap-seconds.list` | IERS table with a valid SHA-1 line | 16 KiB | In place | Latest `#$` update, SD wins ties |
| `secrets/wifi.json` | Known networks ([schema](network.md#known-networks)) | 16 KiB | In place; write-only over HTTP | First valid: SD, then flash |
| `events/<name>.json` | Planned ([events](events.md)) | — | In place | SD if it has `events/`, else flash |

Every upload is validated before it replaces anything. A failure returns
422 with a reason. Uploads, deletes, explicit reloads and mount changes
reload the affected selection.

## Configuration versions

Config file names are compared bytewise in descending order. The first
readable, valid SD version wins; only if SD has none is flash searched the
same way. If neither has one, defaults apply. SD takes priority even when a
flash name sorts later. The web editor saves a new UTC-timestamped version
and keeps older ones.

| Key | Meaning |
| --- | --- |
| `tz` | POSIX TZ rule ([local time](time.md#local-time)) |
| `tz_offset_minutes` | Legacy offset east of UTC, −840..840, used when `tz` is absent. If both are present, both must be valid. |
| `ntp_server` | Hostname or IPv4 address ([time sources](time.md#time-sources)) |
| `audio_volume` | 0–100, default 80 ([audio](audio.md#volume)) |

Unknown keys are ignored. Without a usable config the defaults are
`<+08>-8`, DHCP/pool NTP and volume 80. CLI overrides take priority until
they are reset. `config.json.example` is a valid starting point.

### Cleanup

Cleanup (`config cleanup [sd|flash] [confirm]`,
`GET`/`POST /fs/{sd,flash}/cleanup`, or the file-manager button) works on
one volume at a time.

- It keeps the version that volume would select on its own, and deletes
  only versions whose names sort before it.
- Newer, necessarily invalid versions remain so they can be repaired.
- A volume without a valid version is left untouched.
- The GET form and the CLI without `confirm` only list the files.

## Transactions

Replacements are staged in the private `.rlcd-txn` journal: record, staged
upload, backup. That lets an interrupted rename sequence be recovered under
the storage lock. Unknown or malformed journal state is left untouched for
inspection.

This is not a guarantee against FAT metadata damage on power loss. Do not
modify `.rlcd-txn`.

## HTTP file API

- `GET /fs/` lists volumes with their free space. `GET /fs/active` reports
  the selected config, the Wi-Fi secrets source and the leap table.
- `GET /fs/<volume>/<dir>/` lists a directory as JSON, at most 256 entries
  (the newest names). The `secrets/` listing shows names and sizes only.
- `GET`, `PUT` and `DELETE /fs/<volume>/<path>` work on files. A GET under
  `secrets/` returns 403.
- Paths are percent-encoded, with no query parameters. Encoded separators
  and NUL are rejected.
- **Uploads:** must use Content-Length; chunked uploads are rejected.
  Uploads need room for the complete staged file plus metadata.
- **Deadline:** uploads and downloads have a deadline of 120 s plus 1 s per
  128 KiB.
- **Concurrency:** one worker task handles a transfer at a time, with one
  more request queued, so `/status` and snapshots stay responsive. Serial
  unmount and format wait for storage ownership.
- There is no HTTP formatting endpoint.

The browser file manager at `/files` covers the same operations. It adds
versioned JSON editing, cleanup preview, and Play, Loop and Stop buttons for
sounds.
