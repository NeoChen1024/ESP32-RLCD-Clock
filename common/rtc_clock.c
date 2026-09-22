#include "rtc_clock.h"
#include "clock_health.h"
#include <stddef.h>

enum {
    RTC_YEAR_BASE = 2000,
    RTC_YEAR_LAST = 2099,
    SECONDS_PER_MINUTE = 60,
    MINUTES_PER_HOUR = 60,
    HOURS_PER_DAY = 24,
    SECONDS_PER_HOUR = SECONDS_PER_MINUTE * MINUTES_PER_HOUR,
    SECONDS_PER_DAY = SECONDS_PER_HOUR * HOURS_PER_DAY,
    WEEKDAY_2000_01_01 = 6, /* Saturday; PCF85063A uses Sunday = 0. */
    RTC_SECONDS_MASK = 0x7f,
    RTC_MINUTES_MASK = 0x7f,
    RTC_HOURS_24_MASK = 0x3f,
    RTC_DAY_MASK = 0x3f,
    RTC_WEEKDAY_MASK = 0x07,
    RTC_MONTH_MASK = 0x1f,
};
static const int64_t RTC_UNIX_2000_01_01_S = 946684800LL;
/* Policy floor: reject a corrupted or uninitialized NVS checkpoint. */
static const int64_t MIN_PLAUSIBLE_SYNC_UNIX_S = 1577836800LL; /* 2020-01-01 */

static bool leap(int year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }
static int month_days(int year, int month)
{
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return days[month - 1] + (month == 2 && leap(year));
}
static bool bcd(uint8_t value, uint8_t allowed_bits, int limit, int *out)
{
    if (value & ~allowed_bits) return false;
    int high = (value >> 4) & 0x0f, low = value & 0x0f;
    if (high > 9 || low > 9 || high * 10 + low > limit) return false;
    *out = high * 10 + low;
    return true;
}
static uint8_t to_bcd(int value) { return (uint8_t)((value / 10) << 4 | value % 10); }

bool rtc_clock_decode(const uint8_t r[RTC_CLOCK_REGISTER_COUNT], int64_t *utc_sec)
{
    if (!r || !utc_sec || (r[RTC_CLOCK_SECONDS] & RTC_CLOCK_OS_FLAG)) return false;
    int second, minute, hour, day, weekday, month, yy;
    if (!bcd(r[RTC_CLOCK_SECONDS], RTC_SECONDS_MASK, 59, &second) ||
        !bcd(r[RTC_CLOCK_MINUTES], RTC_MINUTES_MASK, 59, &minute) ||
        !bcd(r[RTC_CLOCK_HOURS], RTC_HOURS_24_MASK, 23, &hour) ||
        !bcd(r[RTC_CLOCK_DAY], RTC_DAY_MASK, 31, &day) ||
        !bcd(r[RTC_CLOCK_MONTH], RTC_MONTH_MASK, 12, &month) ||
        !bcd(r[RTC_CLOCK_YEAR], UINT8_MAX, 99, &yy) ||
        (r[RTC_CLOCK_WEEKDAY] & ~RTC_WEEKDAY_MASK) ||
        (weekday = r[RTC_CLOCK_WEEKDAY]) > 6 || month < 1) return false;
    int year = RTC_YEAR_BASE + yy;
    if (day < 1 || day > month_days(year, month)) return false;
    int64_t days = 0;
    for (int y = RTC_YEAR_BASE; y < year; ++y) days += 365 + leap(y);
    for (int m = 1; m < month; ++m) days += month_days(year, m);
    days += day - 1;
    if (weekday != (days + WEEKDAY_2000_01_01) % 7) return false;
    *utc_sec = RTC_UNIX_2000_01_01_S + days * SECONDS_PER_DAY +
               hour * SECONDS_PER_HOUR + minute * SECONDS_PER_MINUTE + second;
    return true;
}

bool rtc_clock_encode(int64_t utc_sec, uint8_t r[RTC_CLOCK_REGISTER_COUNT])
{
    if (!r || utc_sec < RTC_UNIX_2000_01_01_S) return false;
    int64_t days = (utc_sec - RTC_UNIX_2000_01_01_S) / SECONDS_PER_DAY;
    int64_t seconds = (utc_sec - RTC_UNIX_2000_01_01_S) % SECONDS_PER_DAY;
    int year = RTC_YEAR_BASE;
    while (year <= RTC_YEAR_LAST && days >= 365 + leap(year)) days -= 365 + leap(year++);
    if (year > RTC_YEAR_LAST) return false;
    int month = 1;
    while (month <= 12 && days >= month_days(year, month)) days -= month_days(year, month++);
    int day = (int)days + 1;
    int weekday = (int)((utc_sec - RTC_UNIX_2000_01_01_S) / SECONDS_PER_DAY +
                        WEEKDAY_2000_01_01) % 7;
    r[RTC_CLOCK_SECONDS] = to_bcd((int)(seconds % SECONDS_PER_MINUTE));
    r[RTC_CLOCK_MINUTES] = to_bcd((int)(seconds / SECONDS_PER_MINUTE % MINUTES_PER_HOUR));
    r[RTC_CLOCK_HOURS] = to_bcd((int)(seconds / SECONDS_PER_HOUR));
    r[RTC_CLOCK_DAY] = to_bcd(day);
    r[RTC_CLOCK_WEEKDAY] = (uint8_t)weekday;
    r[RTC_CLOCK_MONTH] = to_bcd(month);
    r[RTC_CLOCK_YEAR] = to_bcd(year - RTC_YEAR_BASE);
    return true;
}

bool rtc_clock_eligible(int64_t rtc_sec, int64_t last_sync_sec)
{
    return last_sync_sec >= MIN_PLAUSIBLE_SYNC_UNIX_S && rtc_sec >= last_sync_sec &&
           rtc_sec - last_sync_sec < CLOCK_HOLDOVER_S;
}
