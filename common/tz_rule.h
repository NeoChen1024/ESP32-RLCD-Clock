#ifndef RLCD_TZ_RULE_H
#define RLCD_TZ_RULE_H
#include <stdbool.h>
#include <stdint.h>

/*
 * POSIX TZ rule strings (the same format as a TZif v2+ footer), shared by
 * host tests and the target. Examples: "CST-8", "<+0530>-5:30",
 * "CET-1CEST,M3.5.0,M10.5.0/3".
 *
 * POSIX offsets are hours WEST of UTC; tz_rule_offset_minutes() returns
 * minutes EAST of UTC like the rest of the firmware. Offsets must be whole
 * minutes within ±14 hours. A rule with daylight time must name its start
 * and end dates; the implementation-defined default is not accepted.
 */

#define TZ_RULE_TEXT_MAX 64

typedef struct {
    char kind;            /* 'J' (1..365, no Feb 29), 'N' (0..365), 'M' */
    uint16_t day;         /* J/N day number; M weekday 0=Sun..6 */
    uint8_t month, week;  /* M only: 1..12, 1..5 (5 = last) */
    int32_t time_s;       /* local wall time of transition, -167h..+167h */
} tz_rule_date_t;

typedef struct {
    char text[TZ_RULE_TEXT_MAX];
    int32_t std_east_s, dst_east_s;
    bool has_dst;
    tz_rule_date_t start, end;
} tz_rule_t;

bool tz_rule_parse(const char *text, tz_rule_t *out);
/* Fixed rule for a whole-minute offset east of UTC, e.g. 480 -> "<+0800>-8". */
bool tz_rule_fixed(int east_minutes, tz_rule_t *out);
/* Local offset in minutes east of UTC at unix_s; *dst may be NULL. */
int tz_rule_offset_minutes(const tz_rule_t *rule, int64_t unix_s, bool *dst);
#endif
