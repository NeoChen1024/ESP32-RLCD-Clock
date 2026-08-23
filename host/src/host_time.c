#include "time_model.h"

#include <time.h>

/*
 * Host-only platform glue for the time model. Not compiled into the ESP32
 * target (which has its own time_model_now / tz_offset_minutes backed by
 * SNTP and a CLI-configurable offset).
 */

/* Fill model from the host system clock (always trusted / NTP_OK). */
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
}

/* Current TZ offset in minutes from the host system local time. */
int tz_offset_minutes(void)
{
    time_t t = time(NULL);
    struct tm tm_local;
    struct tm tm_utc;
    localtime_r(&t, &tm_local);
    gmtime_r(&t, &tm_utc);
    long local_off = tm_local.tm_gmtoff;   /* seconds east of UTC */
    (void)tm_utc;
    return (int)(local_off / 60);
}
