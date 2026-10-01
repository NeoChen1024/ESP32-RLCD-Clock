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
 * TAI−UTC is supplied per frame by the platform (a verified
 * leap-seconds.list table, else the built-in current-era value), and the
 * local UTC offset comes from a POSIX TZ rule evaluated at unix_ms:
 *   TAI = UTC + tai_minus_utc_s,  GPS = TAI − 19
 */

#define TAI_MINUS_UTC_BUILTIN_S 37   /* valid since 2017-01-01 */
#define TAI_MINUS_GPS_SECONDS   19
#define UNIX_TO_GPS_EPOCH_S   315964800LL
#define MJD_EPOCH_UNIX_MS     (40587LL * 86400000LL)   /* MJD at the Unix epoch, in ms */

/* Top-bar sync label. Host simulator always starts NTP_OK. The underlying
 * time state (clock_health.h) decides time_valid; this adds the detail. */
typedef enum {
    SYNC_BOOT_UNS = 0,  /* INVALID: no time yet, Wi-Fi idle */
    SYNC_SYNCING,       /* INVALID: waiting for the first SNTP sync */
    SYNC_NTP_OK,        /* TRUSTED: current source synced within 2 h */
    SYNC_HOLDOVER,      /* TRUSTED: last sync 2..24 h ago, Wi-Fi up */
    SYNC_WIFI_LOST,     /* TRUSTED: Wi-Fi down */
    SYNC_RTC_HOLD,      /* RTC_HOLD: over 24 h, RTC cross-check passing */
    SYNC_TIME_UNSAFE,   /* INVALID: time lost or failed its sanity checks */
} sync_state_t;

typedef struct {
    int64_t  unix_ms;
    bool     time_valid;      /* TRUSTED or RTC_HOLD: show time, run events */
    sync_state_t sync;
    uint32_t ntp_age_s;
    int      wifi_rssi_dbm;
    float    temp_c;
    float    rh_pct;
    float    batt_v;
    bool     temp_humi_valid; /* last good SHTC3 sample is within grace period */
    bool     batt_valid;      /* this frame's ADC read succeeded */
    int      tz_offset_min;   /* local offset east of UTC at unix_ms */
    int      tai_minus_utc_s; /* TAI−UTC at unix_ms */
} clock_model_t;

/* Fill model from the platform's time source + state. Implemented per
 * platform: host = system clock and local zone (always trusted/NTP_OK);
 * target = SNTP time, Wi-Fi/sync state, TZ rule and leap table. */
void time_model_now(clock_model_t *m);

/* ---- Time-scale field extractors (from m->unix_ms) ---- */

/* MJD(TAI): integer day + 7 fractional digits. */
void mjd_tai(const clock_model_t *m, int64_t *day, int64_t *frac_1e7);

/* GPS week + time-of-week. */
void gps_week_tow(const clock_model_t *m, int64_t *week, int64_t *tow);

/* Civil fields. tz_offset_min is the local UTC offset in minutes (e.g. +480);
 * pass m->tz_offset_min for local time or 0 for UTC. */
void civil_fields(const clock_model_t *m, int tz_offset_min,
                  int *year, int *month, int *day, int *weekday,
                  int *hour, int *minute, int *second);

/* ISO week date: year, week (1..53), weekday (1..7, Mon=1). */
void iso_week_date(const clock_model_t *m, int *iso_year, int *iso_week, int *iso_weekday);

#endif
