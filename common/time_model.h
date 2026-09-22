#ifndef RLCD_TIME_MODEL_H
#define RLCD_TIME_MODEL_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Time-scale model — shared verbatim between the host simulator and the
 * ESP32 target (compiled from this file by both builds).
 *
 * All time-scale derivations use integer arithmetic only (no float/double)
 * to avoid readout jitter. Three independent paths from unix epoch:
 *   - MJD(TAI)        from unix_ms
 *   - GPS week/TOW    from unix_s
 *   - civil/UTC/ISO   from unix_ms
 *
 * Offsets are hardcoded for the current era (no leap-second historical table
 * and no automatic update after a future leap second):
 *   TAI = UTC + 37,  GPS = UTC + 18,  TAI = GPS + 19
 */

#define TAI_MINUS_UTC_SECONDS 37
#define GPS_MINUS_UTC_SECONDS 18
#define UNIX_TO_GPS_EPOCH_S   315964800LL
#define MJD_EPOCH_UNIX_MS     (40587LL * 86400000LL)   /* MJD at the Unix epoch, in ms */

/* Sync / trust state. Host simulator always starts NTP_OK. */
typedef enum {
    SYNC_BOOT_UNS = 0,
    SYNC_SYNCING,
    SYNC_NTP_OK,
    SYNC_RTC_HOLD,
    SYNC_WIFI_LOST,
    SYNC_TIME_UNSAFE,
} sync_state_t;

typedef struct {
    int64_t  unix_ms;
    bool     time_trusted;
    sync_state_t sync;
    uint32_t ntp_age_s;
    int      wifi_rssi_dbm;
    float    temp_c;
    float    rh_pct;
    float    batt_v;
} clock_model_t;

/* Fill model from the platform's time source + state. Implemented per
 * platform: host = system clock (always trusted/NTP_OK); target = SNTP time
 * + Wi-Fi/sync state. */
void time_model_now(clock_model_t *m);

/* ---- Time-scale field extractors (from m->unix_ms) ---- */

/* MJD(TAI): integer day + 7 fractional digits. */
void mjd_tai(const clock_model_t *m, int64_t *day, int64_t *frac_1e7);

/* GPS week + time-of-week. */
void gps_week_tow(const clock_model_t *m, int64_t *week, int64_t *tow);

/* Civil fields. tz_offset_min is the local UTC offset in minutes (e.g. +480). */
void civil_fields(const clock_model_t *m, int tz_offset_min,
                  int *year, int *month, int *day, int *weekday,
                  int *hour, int *minute, int *second);

/* ISO week date: year, week (1..53), weekday (1..7, Mon=1). */
void iso_week_date(const clock_model_t *m, int *iso_year, int *iso_week, int *iso_weekday);

/* Current TZ offset in minutes east of UTC. Implemented per platform:
 * host = system local time; target = selected SD/flash config, then a
 * RAM-only CLI override (default UTC+8 when no usable config exists). */
int tz_offset_minutes(void);

#endif
