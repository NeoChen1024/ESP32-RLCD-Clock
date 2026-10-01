#include "tz_rule.h"
#include <stdio.h>
#include <string.h>

#define MAX_EAST_S (14 * 3600)

static bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool name(const char **p)
{
    const char *s = *p;
    if (*s == '<') {
        const char *q = ++s;
        while (is_alpha(*q) || is_digit(*q) || *q == '+' || *q == '-') ++q;
        if (*q != '>' || q - s < 3) return false;
        *p = q + 1;
        return true;
    }
    while (is_alpha(*s)) ++s;
    if (s - *p < 3) return false;
    *p = s;
    return true;
}

static bool decimal(const char **p, int max, int *out)
{
    if (!is_digit(**p)) return false;
    int v = 0;
    while (is_digit(**p)) {
        v = v * 10 + (*(*p)++ - '0');
        if (v > max) return false;
    }
    *out = v;
    return true;
}

/* [+-]hh[:mm[:ss]] in seconds; both offsets and rule times use this form. */
static bool hms(const char **p, int max_hours, int32_t *out)
{
    int sign = 1, h, m = 0, s = 0;
    if (**p == '+' || **p == '-') sign = *(*p)++ == '-' ? -1 : 1;
    if (!decimal(p, max_hours, &h)) return false;
    if (**p == ':') {
        ++*p;
        if (!decimal(p, 59, &m)) return false;
        if (**p == ':') { ++*p; if (!decimal(p, 59, &s)) return false; }
    }
    *out = sign * (h * 3600 + m * 60 + s);
    return true;
}

/* POSIX offsets are west of UTC; store east, whole minutes, within ±14 h. */
static bool offset(const char **p, int32_t *east)
{
    int32_t west;
    if (!hms(p, 24, &west) || west % 60 || west < -MAX_EAST_S || west > MAX_EAST_S) return false;
    *east = -west;
    return true;
}

static bool date(const char **p, tz_rule_date_t *d)
{
    int a, b, c;
    memset(d, 0, sizeof *d);
    if (**p == 'M') {
        ++*p;
        if (!decimal(p, 12, &a) || a < 1 || *(*p)++ != '.' || !decimal(p, 5, &b) || b < 1 ||
            *(*p)++ != '.' || !decimal(p, 6, &c)) return false;
        d->kind = 'M'; d->month = (uint8_t)a; d->week = (uint8_t)b; d->day = (uint16_t)c;
    } else if (**p == 'J') {
        ++*p;
        if (!decimal(p, 365, &a) || a < 1) return false;
        d->kind = 'J'; d->day = (uint16_t)a;
    } else {
        if (!decimal(p, 365, &a)) return false;
        d->kind = 'N'; d->day = (uint16_t)a;
    }
    d->time_s = 2 * 3600;
    if (**p == '/') { ++*p; return hms(p, 167, &d->time_s); }
    return true;
}

bool tz_rule_parse(const char *text, tz_rule_t *out)
{
    if (!text || !out || strlen(text) >= TZ_RULE_TEXT_MAX) return false;
    tz_rule_t r;
    memset(&r, 0, sizeof r);
    const char *p = text;
    if (!name(&p) || !offset(&p, &r.std_east_s)) return false;
    if (*p) {
        r.has_dst = true;
        if (!name(&p)) return false;
        r.dst_east_s = r.std_east_s + 3600;
        if (*p != ',' && !offset(&p, &r.dst_east_s)) return false;
        if (r.dst_east_s < -MAX_EAST_S || r.dst_east_s > MAX_EAST_S) return false;
        if (*p++ != ',' || !date(&p, &r.start) || *p++ != ',' || !date(&p, &r.end) || *p) return false;
    } else {
        r.dst_east_s = r.std_east_s;
    }
    memcpy(r.text, text, strlen(text) + 1);
    *out = r;
    return true;
}

bool tz_rule_fixed(int east_minutes, tz_rule_t *out)
{
    if (east_minutes < -MAX_EAST_S / 60 || east_minutes > MAX_EAST_S / 60) return false;
    int a = east_minutes < 0 ? -east_minutes : east_minutes;
    char text[TZ_RULE_TEXT_MAX];
    if (!east_minutes) snprintf(text, sizeof text, "UTC0");
    else if (a % 60) snprintf(text, sizeof text, "<%c%02d%02d>%s%d:%02d", east_minutes < 0 ? '-' : '+',
                              a / 60, a % 60, east_minutes < 0 ? "" : "-", a / 60, a % 60);
    else snprintf(text, sizeof text, "<%c%02d>%s%d", east_minutes < 0 ? '-' : '+',
                  a / 60, east_minutes < 0 ? "" : "-", a / 60);
    return tz_rule_parse(text, out);
}

/* Days since 1970-01-01 (Howard Hinnant's days_from_civil). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static bool leap_year(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int64_t year_of(int64_t days)
{
    int64_t y = 1970 + days / 366;
    while (days_from_civil(y + 1, 1, 1) <= days) ++y;
    while (days_from_civil(y, 1, 1) > days) --y;
    return y;
}

/* Local midnight of the rule date, in days since the epoch. */
static int64_t rule_day(const tz_rule_date_t *d, int64_t y)
{
    int64_t jan1 = days_from_civil(y, 1, 1);
    if (d->kind == 'J') return jan1 + d->day - 1 + (leap_year(y) && d->day >= 60);
    if (d->kind == 'N') return jan1 + (d->day == 365 && !leap_year(y) ? 364 : d->day);
    static const uint8_t month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int64_t first = days_from_civil(y, d->month, 1);
    int first_wd = (int)(((first + 4) % 7 + 7) % 7);           /* 0 = Sunday */
    int64_t day = first + (d->day - first_wd + 7) % 7 + (int64_t)(d->week - 1) * 7;
    int length = month_days[d->month - 1] + (d->month == 2 && leap_year(y));
    while (day >= first + length) day -= 7;
    return day;
}

int tz_rule_offset_minutes(const tz_rule_t *r, int64_t unix_s, bool *dst)
{
    bool in_dst = false;
    if (r->has_dst) {
        int64_t local = unix_s + r->std_east_s;
        int64_t days = local / 86400 - (local % 86400 < 0);
        int64_t y = year_of(days);
        int64_t start = rule_day(&r->start, y) * 86400 + r->start.time_s - r->std_east_s;
        int64_t end = rule_day(&r->end, y) * 86400 + r->end.time_s - r->dst_east_s;
        in_dst = start < end ? unix_s >= start && unix_s < end : !(unix_s >= end && unix_s < start);
    }
    if (dst) *dst = in_dst;
    return (in_dst ? r->dst_east_s : r->std_east_s) / 60;
}
