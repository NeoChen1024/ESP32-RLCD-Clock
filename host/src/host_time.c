#include "time_model.h"

#include <time.h>

/*
 * Host-only platform glue for the time model. Not compiled into the ESP32
 * target (whose time_model_now is backed by SNTP, a POSIX TZ rule and the
 * leap-seconds.list table).
 */

/* Fill model from the host system clock and local zone (always trusted /
 * NTP_OK, built-in TAI−UTC). */
void time_model_now(clock_model_t *m)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    m->unix_ms = (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
    m->time_trusted = true;
    m->sync         = SYNC_NTP_OK;
    m->ntp_age_s    = 0;
    m->wifi_rssi_dbm = -57;
    m->temp_c  = 28.4f;
    m->rh_pct  = 61.0f;
    m->batt_v  = 3.91f;
    m->temp_humi_valid = true;
    m->batt_valid = true;
    time_t t = (time_t)ts.tv_sec;
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    m->tz_offset_min = (int)(tm_local.tm_gmtoff / 60);  /* seconds east of UTC */
    m->tai_minus_utc_s = TAI_MINUS_UTC_BUILTIN_S;
}
