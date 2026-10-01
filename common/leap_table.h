#ifndef RLCD_LEAP_TABLE_H
#define RLCD_LEAP_TABLE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * TAI−UTC table parsed from the IERS/IETF leap-seconds.list format.
 *
 * A file is accepted only when its "#$" update, "#@" expiry and "#h" SHA-1
 * lines are present, the hash matches, and every step changes TAI−UTC by
 * exactly one second at a strictly later instant. The table is shared by
 * host tests and the target.
 */

#define LEAP_TABLE_MAX     64
#define LEAP_FILE_MAX      (16U * 1024U)
#define NTP_UNIX_OFFSET_S  2208988800LL   /* 1900-01-01 to 1970-01-01 */

typedef struct {
    int64_t unix_s;     /* first UTC instant with this TAI−UTC */
    int16_t tai_minus_utc;
} leap_entry_t;

typedef struct {
    unsigned count;
    int64_t updated_unix_s;
    int64_t expires_unix_s;
    leap_entry_t entries[LEAP_TABLE_MAX];
} leap_table_t;

bool leap_table_parse(const char *text, size_t length, leap_table_t *out);
/* Reads and parses at most LEAP_FILE_MAX bytes. Caller owns any storage lock. */
bool leap_table_load(const char *path, leap_table_t *out);
/* TAI−UTC in seconds at unix_s. Instants before the first entry use it. */
int leap_table_tai_minus_utc(const leap_table_t *t, int64_t unix_s);
#endif
