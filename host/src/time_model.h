#ifndef RLCD_HOST_TIME_MODEL_H
#define RLCD_HOST_TIME_MODEL_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Time-scale model.
 *
 * All time-scale derivations use integer arithmetic only (no float/double)
 * to avoid readout jitter. Three independent paths from unix epoch:
 *   - MJD(TAI)        from unix_ms
 *   - GPS week/SOW    from unix_s
 *   - civil/UTC/ISO   from unix_ms
 *
 * Offsets are hardcoded (current and future display only; no leap-second
 * historical table):
 *   TAI = UTC + 37,  GPS = UTC + 18,  TAI = GPS + 19
 */

#define TAI_MINUS_UTC_SECONDS 37
#define GPS_MINUS_UTC_SECONDS 18
#define UNIX_TO_GPS_EPOCH_S   315964800LL
#define MJD_EPOCH_UNIX_MS     (40587LL * 86400000LL)   /* 1858-11-17 in unix ms (TAI) */

/* Sync / trust state. Host simulator always starts NTP_OK. */
typedef enum {
    SYNC_BOOT_UNS = 0,
    SYNC_SYNCING,
    SYNC_NTP_OK,
    SYNC_RTC_HOLD,
    SYNC_WIFI_LOST,
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

/* Fill model from the system clock (host: always trusted / NTP_OK). */
void time_model_now(clock_model_t *m);

/* ---- Time-scale field extractors (from m->unix_ms) ---- */

/* MJD(TAI): integer day + 7 fractional digits. */
void mjd_tai(const clock_model_t *m, int64_t *day, int64_t *frac_1e7);

/* GPS week + second-of-week. */
void gps_week_sow(const clock_model_t *m, int64_t *week, int64_t *sow);

/* Civil fields. tz_offset_min is the local UTC offset in minutes (e.g. +480). */
void civil_fields(const clock_model_t *m, int tz_offset_min,
                  int *year, int *month, int *day, int *weekday,
                  int *hour, int *minute, int *second);

/* ISO week date: year, week (1..53), weekday (1..7, Mon=1). */
void iso_week_date(const clock_model_t *m, int *iso_year, int *iso_week, int *iso_weekday);

/* Current TZ offset in minutes from the host system local time. */
int host_tz_offset_minutes(void);

#endif
