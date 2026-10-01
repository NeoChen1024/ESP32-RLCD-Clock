#include "time_model.h"
#include <stdio.h>
#include <string.h>

static int check_iso(clock_model_t *m, int64_t unix_s,
                     int expected_year, int expected_week, int expected_weekday)
{
    int year, week, weekday;
    m->unix_ms = unix_s * 1000LL;
    iso_week_date(m, &year, &week, &weekday);
    if (year == expected_year && week == expected_week && weekday == expected_weekday)
        return 1;
    printf("FAIL ISO for unix %lld: got %04d-W%02d-%d, expected %04d-W%02d-%d\n",
           (long long)unix_s, year, week, weekday,
           expected_year, expected_week, expected_weekday);
    return 0;
}

int main(void)
{
    /* Known instant: 2026-06-21T15:17:15Z = unix 1782055035 s.
     * Expected (TAI=UTC+37, GPS=UTC+18):
     *   MJD(TAI) day = 61212, frac = 6374074
     *   GPS week = 2424, TOW = 55053  (GPS scale, includes +18)
     */
    clock_model_t m;
    memset(&m, 0, sizeof m);
    m.unix_ms = 1782055035LL * 1000LL;
    m.time_valid = true;
    m.tai_minus_utc_s = TAI_MINUS_UTC_BUILTIN_S;

    int64_t day, frac;
    mjd_tai(&m, &day, &frac);
    printf("MJD(TAI): day=%lld frac=%07lld -> %07lld.%07lld\n",
           (long long)day, (long long)frac, (long long)day, (long long)frac);

    int64_t w, tow;
    gps_week_tow(&m, &w, &tow);
    printf("GPS: week=%lld TOW=%lld\n", (long long)w, (long long)tow);

    int y,mo,d,wd,h,mi,s;
    civil_fields(&m, 0, &y,&mo,&d,&wd,&h,&mi,&s);
    const char *wdn[]={"MON","TUE","WED","THU","FRI","SAT","SUN"};
    printf("UTC civil: %04d-%02d-%02d %s %02d:%02d:%02d\n", y,mo,d,wdn[wd],h,mi,s);

    int iy, iw, iwd;
    iso_week_date(&m, &iy,&iw,&iwd);
    printf("ISO week: %04d-W%02d-%d\n", iy, iw, iwd);

    /* checks */
    int ok = 1;
    if (w != 2424) { printf("FAIL gps week\n"); ok=0; }
    if (tow != 55053) { printf("FAIL gps tow (got %lld)\n", (long long)tow); ok=0; }
    if (day != 61212) { printf("FAIL mjd day (got %lld)\n", (long long)day); ok=0; }
    if (frac != 6374074) { printf("FAIL mjd fraction (got %07lld)\n", (long long)frac); ok=0; }
    if (!(y==2026 && mo==6 && d==21 && h==15 && mi==17 && s==15)) { printf("FAIL civil\n"); ok=0; }
    if (wd != 6) { printf("FAIL weekday: 2026-06-21 is Sun (wd=6), got %d\n", wd); ok=0; }
    if (iwd != 7) { printf("FAIL iso weekday: Sun=7, got %d\n", iwd); ok=0; }
    if (iy != 2026 || iw != 25) { printf("FAIL iso year/week\n"); ok=0; }

    /* TAI−UTC comes from the model: one second less moves TAI and GPS back. */
    m.tai_minus_utc_s = TAI_MINUS_UTC_BUILTIN_S - 1;
    int64_t day2, frac2, w2, tow2;
    mjd_tai(&m, &day2, &frac2);
    gps_week_tow(&m, &w2, &tow2);
    if (w2 != 2424 || tow2 != 55052) { printf("FAIL gps with TAI-UTC 36\n"); ok=0; }
    if (day2 != 61212 || frac2 != 6373958) { printf("FAIL mjd with TAI-UTC 36 (got %07lld)\n", (long long)frac2); ok=0; }

    /* epoch anchor: 1970-01-01 = Thursday */
    memset(&m, 0, sizeof m);
    m.unix_ms = 0;
    m.time_valid = true;
    civil_fields(&m, 0, &y,&mo,&d,&wd,&h,&mi,&s);
    printf("epoch: %04d-%02d-%02d %s %02d:%02d:%02d\n", y,mo,d,wdn[wd],h,mi,s);
    if (!(y==1970 && mo==1 && d==1 && wd==3)) { printf("FAIL epoch weekday (expect THU)\n"); ok=0; }
    iso_week_date(&m, &iy,&iw,&iwd);
    printf("epoch ISO: %04d-W%02d-%d\n", iy,iw,iwd);
    if (!(iy==1970 && iw==1 && iwd==4)) { printf("FAIL epoch ISO (expect 1970-W01-4)\n"); ok=0; }

    /* ISO week-year boundaries, including both 52- and 53-week years. */
    ok &= check_iso(&m, 1451606400LL, 2015, 53, 5); /* 2016-01-01 */
    ok &= check_iso(&m, 1483228800LL, 2016, 52, 7); /* 2017-01-01 */
    ok &= check_iso(&m, 1546214400LL, 2019,  1, 1); /* 2018-12-31 */
    ok &= check_iso(&m, 1609459200LL, 2020, 53, 5); /* 2021-01-01 */
    ok &= check_iso(&m, 1640995200LL, 2021, 52, 6); /* 2022-01-01 */
    ok &= check_iso(&m, 1798761600LL, 2026, 53, 5); /* 2027-01-01 */

    printf(ok ? "ALL OK\n" : "FAILURES\n");
    return ok ? 0 : 1;
}
