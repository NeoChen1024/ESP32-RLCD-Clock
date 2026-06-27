#include "time_model.h"
#include <stdio.h>
#include <string.h>

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
    m.time_trusted = true;

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
    if (!(y==2026 && mo==6 && d==21 && h==15 && mi==17 && s==15)) { printf("FAIL civil\n"); ok=0; }
    printf(ok ? "ALL OK\n" : "FAILURES\n");
    return ok ? 0 : 1;
}
