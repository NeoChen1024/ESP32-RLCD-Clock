# Network

This is the contract for Wi-Fi station behavior, known-network secrets and
the HTTP endpoints. SNTP is covered in [time](time.md#time-sources).

## Wi-Fi modes

`firmware/main/wifi_mgr.c` runs the station on the default event loop. It
has three modes:

| Mode | Entered by | Behavior |
| --- | --- | --- |
| AUTO | Boot default; `wifi reset` | Whenever there is no active connection, scans and joins the strongest visible known network, trying the next one on failure. |
| MANUAL | `wifi connect "<ssid>" [password]` | Joins one network and retries it with a 1–30 s backoff. Known networks are ignored. |
| OFF | `wifi disconnect` | No automatic connection. |

MANUAL and OFF last until `wifi reset` or a reboot.

**AUTO details:**

- **Rescan timing:**
  - When no known network joins, it rescans after 10 s, doubling up to
    5 minutes.
  - A dropped connection rescans on the next tick.
- **Candidates:**
  - A live link is not switched to a stronger network.
  - Among APs sharing an SSID, the strongest one is joined.
  - Hidden SSIDs are not supported.
- **Attempt timeout:** an attempt is given 30 s for association and DHCP.
  A stalled attempt is aborted, and the next candidate is tried.
- **List changes:** a changed known-network list keeps the current AUTO link
  only if that link's SSID and password are still listed. Otherwise the
  manager rescans. An identical list, as reloaded on a remount, changes
  nothing.

Credentials are never written to NVS (`WIFI_STORAGE_RAM`), and passwords
are never logged.

### Known networks

`secrets/wifi.json` holds up to 8 networks:

```json
{"networks": [{"ssid": "Example Home", "password": "replace-with-passphrase"},
              {"ssid": "Example Open Network"}]}
```

- SSIDs are 1–32 bytes and must be unique.
- A password is omitted or empty for an open network, 8–63 printable ASCII
  characters, or 64 hex digits.
- Unknown keys are ignored.

The first valid file on SD, then flash, is used; the lists are not merged.
Over HTTP the file is write-only (see [storage](storage.md#http-file-api)).
It is still plaintext on FAT, so anyone with serial access or the SD card
can read it. `wifi.json.example` is a valid starting point, and real
`wifi.json` files are git-ignored.

## HTTP endpoints

The server runs on port 80. It is unauthenticated and intended for a trusted
LAN.

| Endpoint | Purpose |
| --- | --- |
| `GET /` | Homepage with live state |
| `GET /status` | JSON: Wi-Fi, SNTP and time state, TZ, leap table, audio, display frames |
| `GET /snapshot.pbm`, `/snapshot.bmp` | Current framebuffer through the shared `frame_export` encoder (byte-comparable with the host) |
| `GET /files` | File manager page |
| `/fs/...` | File API ([storage](storage.md#http-file-api)) |
| `/audio...` | Playback control ([audio](audio.md#control)) |
