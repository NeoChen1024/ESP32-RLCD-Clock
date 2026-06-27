#include "time_model.h"

#include <time.h>
#include <string.h>

/* ---- model from system clock ---- */

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

/* ---- MJD(TAI) ---- */

void mjd_tai(const clock_model_t *m, int64_t *day, int64_t *frac_1e7)
{
    int64_t tai_ms  = m->unix_ms + (int64_t)TAI_MINUS_UTC_SECONDS * 1000LL;
    int64_t mjd_ms  = MJD_EPOCH_UNIX_MS + tai_ms;
    *day            = mjd_ms / 86400000LL;
    int64_t rem_ms  = mjd_ms % 86400000LL;
    if (rem_ms < 0) { rem_ms += 86400000LL; (*day)--; }
    /* 7 fractional decimal digits of a day */
    *frac_1e7 = rem_ms * 10000000LL / 86400000LL;
}

/* ---- GPS week / TOW ---- */

void gps_week_tow(const clock_model_t *m, int64_t *week, int64_t *tow)
{
    int64_t unix_s = m->unix_ms / 1000LL;
    int64_t gps_s  = unix_s - UNIX_TO_GPS_EPOCH_S + GPS_MINUS_UTC_SECONDS;
    if (gps_s < 0) gps_s += 604800LL;  /* safety; not expected for current era */
    *week = gps_s / 604800LL;
    *tow  = gps_s % 604800LL;
}

/* ---- civil fields ---- */

/* Convert unix seconds to Y/M/D/h/m/s for a given UTC offset (minutes).
 * Algorithm: Howard Hinnant, days_from_civil inverse. */
static void civil_from_days(int64_t z, int *year, int *month, int *day, int *weekday)
{
    z += 719468LL;                          /* days since 1970-01-01 to epoch of algorithm */
    int64_t era = (z >= 0 ? z : z - 146096LL) / 146097LL;
    unsigned doe = (unsigned)(z - era * 146097LL);       /* [0, 146096] */
    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;  /* [0, 399] */
    int64_t y = (int64_t)yoe + era * 400LL;
    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);    /* [0, 365] */
    unsigned mp  = (5*doy + 2)/153;                      /* [0, 11] */
    unsigned d   = doy - (153*mp + 2)/5 + 1;             /* [1, 31] */
    unsigned mon = mp < 10 ? mp + 3 : mp - 9;            /* [1, 12] */
    *year  = (int)(y + (mon <= 2));
    *month = (int)mon;
    *day   = (int)d;
    /* weekday: 1970-01-01 was Thursday = 4 (Mon=0). z is days since 1970-01-01. */
    int wd = (int)((z - 719468LL) % 7);
    if (wd < 0) wd += 7;
    *weekday = wd;  /* 0=Mon .. 6=Sun */
}

void civil_fields(const clock_model_t *m, int tz_offset_min,
                  int *year, int *month, int *day, int *weekday,
                  int *hour, int *minute, int *second)
{
    int64_t local_s = m->unix_ms / 1000LL + tz_offset_min * 60LL;
    int64_t days = local_s / 86400LL;
    int64_t rem = local_s % 86400LL;
    if (rem < 0) { rem += 86400LL; days--; }
    civil_from_days(days, year, month, day, weekday);
    *hour   = (int)(rem / 3600LL);
    *minute = (int)((rem % 3600LL) / 60LL);
    *second = (int)(rem % 60LL);
}

/* ---- ISO week date ---- */

/* weekday: 0=Mon..6=Sun (from civil_fields). ISO weekday: 1=Mon..7=Sun. */
static int day_of_year(int year, int month, int day)
{
    static const int doy[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int d = doy[month-1] + day;
    int leap = (year%4==0 && year%100!=0) || (year%400==0);
    if (leap && month > 2) d++;
    return d;
}

static int iso_weekday_mon1(int weekday0) { return weekday0 + 1; }

void iso_week_date(const clock_model_t *m, int *iso_year, int *iso_week, int *iso_weekday)
{
    /* Use UTC date for ISO week to stay scale-consistent. */
    int tz = 0;
    int y, mo, d, wd, h, mi, s;
    civil_fields(m, tz, &y, &mo, &d, &wd, &h, &mi, &s);

    int doy  = day_of_year(y, mo, d);
    int iwd  = iso_weekday_mon1(wd);          /* 1..7, Mon=1 */
    int week = (doy - iwd + 10) / 7;

    if (week < 1) {
        /* belongs to previous year's last week */
        *iso_year = y - 1;
        /* last week of prev year: Dec 28 always in last week */
        int py = y - 1;
        int leap = (py%4==0 && py%100!=0) || (py%400==0);
        int days_in_year = leap ? 366 : 365;
        /* weekday of Dec 31 of prev year */
        int py_doy_dec31 = days_in_year;
        int py_wd_dec31 = (iwd + (days_in_year - doy)) % 7;
        if (py_wd_dec31 == 0) py_wd_dec31 = 7;
        *iso_week = (py_doy_dec31 - py_wd_dec31 + 10) / 7;
    } else if (week > 52) {
        /* might spill into next year's week 1 */
        int leap = (y%4==0 && y%100!=0) || (y%400==0);
        int days_in_year = leap ? 366 : 365;
        if (days_in_year - doy < 4 - iwd) {
            *iso_year = y + 1;
            *iso_week = 1;
        } else {
            *iso_year = y;
            *iso_week = week;
        }
    } else {
        *iso_year = y;
        *iso_week = week;
    }
    *iso_weekday = iwd;
}

/* ---- host TZ offset ---- */

int host_tz_offset_minutes(void)
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
