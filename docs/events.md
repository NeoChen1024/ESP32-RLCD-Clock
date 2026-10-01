# Events

This is the agreed design for scheduled events. Events can ring an alarm,
or only appear in the upcoming list on the face. They are **not implemented
yet**; progress is tracked in the
[roadmap](roadmap.md).
Playback is already in place (see [audio](audio.md)).

## Files

- Each event is one file, `events/<name>.json`, using the managed ASCII
  file-name rules. There are at most 32 events.
- Only the [active volume](storage.md#active-volume)'s `events/` is used;
  the volumes are never merged.
- Event files are not versioned. HTTP uploads and device updates replace
  them in place through the `.rlcd-txn` journal, like `wifi.json` and the
  leap table.
- Uploads, deletes, explicit reloads and volume mount changes reload the
  set.

```json
{
  "title": "New Year",
  "armed": true,
  "once": true,
  "rule": { "years": "2027", "months": "1", "days": "1" },
  "times": ["00:00:00"],
  "duration": "00:30",
  "sound": { "file": "fanfare.flac", "loop": true, "volume": 90 }
}
```

| Field | Meaning |
| --- | --- |
| `title` | Printable ASCII, at most 40 characters. Other characters are rejected; the face font is ASCII-only and CJK titles are out of scope. |
| `armed` | `false` disables the event without deleting it. |
| `once` | Fire at most once, then disarm; see below. |
| `rule` | The date pattern; see below. |
| `times` | When the event fires; see [times of day](#times-of-day). Empty or absent makes an all-day event, which is displayed but never fires. |
| `duration` | `HH:MM`, timed events only; the event shows as NOW while it lasts. |
| `list` | `false` keeps the event out of the upcoming list, e.g. an hourly chime. Default `true`. |
| `sound` | Optional. Without it the event is display-only. `file` is a `sounds/` WAV or FLAC on the same volume. `loop` applies the 10-minute loop limit. `volume` (0–100) affects only this event's playback. |

Unknown keys are kept, so that device rewrites do not drop them.

## Date rule

Each field compiles to a bitmap. A day matches when **every** given field
matches; an omitted field matches everything. Unlike cron, day-of-month and
weekday are combined with AND, not OR.

Numeric fields here and in [times of day](#times-of-day) share one cron-like
syntax: comma-separated items, each a value, a range `a-b`, `*`, or a range
or `*` followed by a step `/N`. A step counts from the start of its range,
so `*/3` months is `1,4,7,10` and `10-59/20` minutes is `10,30,50`.

| Field | Values | Bitmap |
| --- | --- | --- |
| `years` | Optional list or ranges, e.g. `"2027"`. Mainly for `once` events. | Range check |
| `months` | `1`–`12`, lists, ranges and steps: `"1,4,7,10"`, `"*/3"`, `"3-5,9"` | 12 bits |
| `days` | `1`–`31`, `L` (last day), `L-1` … `L-6` (days before the last) | 31 + 7 bits; the union of both parts |
| `weekdays` | `mon`–`sun`, lists and ranges | 7 bits (ISO, Monday first) |
| `nth` | Which occurrence of the weekday in the month: `1`–`5`, `L` (last) | 6 bits |

Without `years` the longest cycle is one year. Examples:

| Intent | Rule and times |
| --- | --- |
| Every Saturday and Sunday at 08:00 | `{"weekdays": "sat,sun"}`, `["08:00"]` |
| Last day of every month at 18:00 | `{"days": "L"}`, `["18:00"]` |
| Wednesdays in January, April, July and October at 09:00 | `{"months": "*/3", "weekdays": "wed"}`, `["09:00"]` |
| New Year, 1 January 00:00:00 | `{"months": "1", "days": "1"}`, `["00:00:00"]` |
| Second Tuesday of each month | `{"weekdays": "tue", "nth": "2"}` |
| Last Friday of each month | `{"weekdays": "fri", "nth": "L"}` |

Day-of-month OR weekday, as in cron, takes two event files. Intervals that
do not divide the hour or day ("every 90 minutes"), alternate weeks and
holiday exclusion are out of scope for now.

## Times of day

`times` takes one of two forms:

- **List:** up to 8 `HH:MM` or `HH:MM:SS` local times, for irregular times
  such as `["08:15", "17:40"]`.
- **Pattern:** an object with `hours` (0–23), `minutes` (0–59) and
  `seconds` (0–59) in the syntax above. The event fires at every
  combination of the three, as in cron. At least one field must be given;
  omitted `hours` means every hour, and omitted `minutes` or `seconds`
  means 0.

| Intent | `times` |
| --- | --- |
| Hourly chime on the hour | `{"minutes": "0"}` |
| Every 15 minutes | `{"minutes": "*/15"}` |
| Every half hour, 09:00–17:30 | `{"hours": "9-17", "minutes": "0,30"}` |
| Every 10 seconds within one minute | `{"hours": "12", "minutes": "0", "seconds": "*/10"}` |

The pattern compiles to 24 + 60 + 60 bits. If an event fires again while its
own sound is still playing, playback continues rather than restarting.

## Once events

- After a `once` event fires, the device rewrites its file with
  `"armed": false`.
  - Before rewriting, it compares a hash of the file with the hash taken
    when the event was loaded. If the file has changed, it reloads instead
    of overwriting the newer upload.
  - All other fields are preserved.
  - If the write fails, RAM remembers that this occurrence fired, so it
    cannot repeat before a reboot. The failure is logged and shown in
    status.
- A missed occurrence (device off, or time INVALID) must not wait a year.
  - Without `years`, the device writes the computed next occurrence as
    `"due": "YYYY-MM-DDTHH:MM:SS"` the first time it loads the event.
  - Once `due` (or the last matching time in `years`) has passed, the event
    is disarmed even if it never rang.
- An event suppressed by a higher-priority event still counts as fired.

## When events run

- **Time state:** Events run unless the time state is INVALID; RTC_HOLD
  time runs them normally (see
  [time](time.md#time-state)).
- **Local time:** Matching uses local time from the active POSIX TZ rule.
- **Clock steps:**
  - A forward step of up to 60 s fires the occurrences it skipped; a larger
    forward step does not catch up.
  - A backward step never fires the same occurrence twice.
- **Daylight time:** handled like cron.
  - Spring forward: occurrences in the skipped local interval fire
    immediately after the transition, once per event, however many the
    gap held.
  - Fall back: a repeated local time fires only on its first pass.

## Priority

The **period** of an event is measured by how many times it occurs in a
fixed four-year reference window that includes one leap year: matching days
times firing times per day. Fewer occurrences means a longer period.

| Event | Occurrences in the window |
| --- | --- |
| Yearly | about 4 |
| Last day of every month | about 48 |
| Every Saturday and Sunday | about 417 |
| 29 February only | 1 |
| Hourly chime | about 35 064 |

A frequent pattern therefore never displaces a rarer event: a daily 08:00
alarm wins over an hourly chime at the same second.

`once` events rank above all others.

- **Simultaneous:** when events fire in the same second, the longest period
  rings. A tie goes to the first file name.
- **Overlapping:** when an event fires while another is still playing, it
  interrupts only if its period is at least as long. Otherwise it is
  suppressed and recorded in status. For example, a daily alarm cannot cut
  off the New Year sound, but New Year interrupts a daily alarm.

## Dismissal

Ringing stops with:

- either board button: BOOT (GPIO0) or GPIO18, both active low;
- `audio stop` on the CLI;
- `POST /audio/stop` over HTTP.

Snooze is out of scope for the first version.

## Display

The band between the GPS row and the telemetry lines shows up to three
upcoming events within the next seven days, in time order. All-day events
sort before timed events on the same day. Each event appears at most once,
at its next occurrence. Events with `"list": false` are left out.

```
TODAY  18:00  Dinner
SAT    ALL DAY Weekend market
01-01  00:00  New Year
```

- A running event with a `duration` shows `NOW`.
- An all-day event today shows `TODAY`.
- The list is hidden while time is INVALID and shown normally under
  RTC_HOLD.
- Layout is tuned in the host simulator: `rlcd_host --events <dir>` reads
  event files from a local directory. No firmware flashing is needed.

## Shared code and tests

These parts live in `common/` and are covered by host tests:

- rule and time-pattern parsing (including steps) and bitmap compilation;
- next-occurrence search (bounded at eight years, for 29 February rules);
- period counting and priority;
- the upcoming list.

Test cases include last days, nth weekdays, leap years, the year boundary
and daylight-time transitions.
