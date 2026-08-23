#include "time_model.h"

#include <string.h>

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
static void civil_from_days(int64_t days, int *year, int *month, int *day, int *weekday)
{
    int64_t z = days + 719468LL;            /* days since 1970-01-01 to epoch of algorithm */
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
    /* weekday: 1970-01-01 was Thursday = 3 if Mon=0 */
    int wd = (int)((days + 3) % 7);
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

static int is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/* An ISO year has 53 weeks iff 1 January is Thursday, or Wednesday in a
 * leap year. jan1_iwd uses the ISO weekday convention (Mon=1..Sun=7). */
static int iso_weeks_in_year(int year, int jan1_iwd)
{
    return jan1_iwd == 4 || (jan1_iwd == 3 && is_leap_year(year)) ? 53 : 52;
}

void iso_week_date(const clock_model_t *m, int *iso_year, int *iso_week, int *iso_weekday)
{
    /* Use UTC date for ISO week to stay scale-consistent. */
    int tz = 0;
    int y, mo, d, wd, h, mi, s;
    civil_fields(m, tz, &y, &mo, &d, &wd, &h, &mi, &s);

    int doy  = day_of_year(y, mo, d);
    int iwd  = iso_weekday_mon1(wd);          /* 1..7, Mon=1 */
    int week = (doy - iwd + 10) / 7;
    int jan1_iwd = ((iwd - 1 - ((doy - 1) % 7) + 7) % 7) + 1;

    if (week < 1) {
        /* belongs to previous year's last week */
        int py = y - 1;
        int py_days = is_leap_year(py) ? 366 : 365;
        int py_jan1_iwd = ((jan1_iwd - 1 - (py_days % 7) + 7) % 7) + 1;
        *iso_year = py;
        *iso_week = iso_weeks_in_year(py, py_jan1_iwd);
    } else if (week > iso_weeks_in_year(y, jan1_iwd)) {
        *iso_year = y + 1;
        *iso_week = 1;
    } else {
        *iso_year = y;
        *iso_week = week;
    }
    *iso_weekday = iwd;
}
