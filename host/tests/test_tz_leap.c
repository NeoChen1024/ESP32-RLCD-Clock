#define _DEFAULT_SOURCE
#include "leap_table.h"
#include "tz_rule.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* glibc implements POSIX TZ strings; use it as the reference oracle. */
static void compare_with_libc(const char *text)
{
    tz_rule_t rule;
    assert(tz_rule_parse(text, &rule));
    assert(!setenv("TZ", text, 1));
    tzset();
    /* Hourly samples plus each instant around every hour boundary cover
     * both transitions in each year from 1999 through 2041. */
    for (int64_t t = 915148800LL; t < 2240611200LL; t += 3599) {
        struct tm tm;
        time_t tt = (time_t)t;
        assert(localtime_r(&tt, &tm));
        bool dst;
        int ours = tz_rule_offset_minutes(&rule, t, &dst);
        if (ours != tm.tm_gmtoff / 60 || dst != (tm.tm_isdst > 0)) {
            printf("FAIL %s at %lld: ours %+d dst=%d, libc %+ld dst=%d\n", text, (long long)t,
                   ours, dst, tm.tm_gmtoff / 60, tm.tm_isdst);
            exit(1);
        }
    }
}

static char *read_text(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(LEAP_FILE_MAX + 1);
    *n = fread(buf, 1, LEAP_FILE_MAX, f);
    fclose(f);
    return buf;
}

static void replace(char *text, size_t *n, const char *from, const char *to)
{
    char *p = strstr(text, from);
    assert(p);
    size_t a = strlen(from), b = strlen(to);
    memmove(p + b, p + a, *n - (size_t)(p - text) - a);
    memcpy(p, to, b);
    *n = *n - a + b;
}

/* Minimal valid file. The hash is SHA-1 of "100" "4000000000" and the row
 * fields "227206080010" "228778560011", derived independently with sha1sum. */
static const char sample[] =
    "#$\t100\n#@\t4000000000\n"
    "2272060800\t10\t# 1 Jan 1972\n"
    "2287785600\t11\t# 1 Jul 1972\n";
static const char sample_hash[] = "#h\t5367a011 c30a5f81 9340d986 82856c05 33c8c286\n";

int main(void)
{
    /* ---- TZ rules ---- */
    const char *rules[] = {
        "CST-8", "<+0530>-5:30", "<-03>3", "UTC0", "<+1345>-13:45",
        "CET-1CEST,M3.5.0,M10.5.0/3", "EST5EDT,M3.2.0,M11.1.0",
        "AEST-10AEDT,M10.1.0,M4.1.0/3", "<-03>3<-02>,M3.5.0/-2,M10.5.0/-1",
        "IST-1GMT0,M10.5.0,M3.5.0/1", "<+0330>-3:30<+0430>,J79/24,J263/24",
        "NZST-12NZDT,M9.5.0,M4.1.0/3",
    };
    for (unsigned i = 0; i < sizeof rules / sizeof rules[0]; ++i) compare_with_libc(rules[i]);

    /* tzcode's permanent-DST idiom. glibc briefly reports standard time at
     * each new year here; the rule's intent is daylight time all year. */
    tz_rule_t r;
    assert(tz_rule_parse("EST5EDT,0/0,J365/25", &r));
    for (int64_t t = 915148800LL - 86400; t < 1230768000LL; t += 1799) {
        bool dst;
        assert(tz_rule_offset_minutes(&r, t, &dst) == -240 && dst);
    }

    const char *bad[] = {
        "", "8", "UT0", "CST", "CST-8:00:30", "CST-15", "<+08-8", "CST-8CDT",
        "EST5EDT,M3.2.0", "EST5EDT,M13.2.0,M11.1.0", "EST5EDT,M3.6.0,M11.1.0",
        "EST5EDT,M3.2.7,M11.1.0", "EST5EDT,J0,J100", "EST5EDT,M3.2.0/168,M11.1.0",
        "CST-8 ", "CET-1CEST,M3.5.0,M10.5.0/3x",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA0",
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        if (tz_rule_parse(bad[i], &r)) { printf("FAIL accepted \"%s\"\n", bad[i]); return 1; }
    }
    assert(tz_rule_fixed(480, &r) && !strcmp(r.text, "<+08>-8") && tz_rule_offset_minutes(&r, 0, NULL) == 480);
    assert(tz_rule_fixed(-210, &r) && !strcmp(r.text, "<-0330>3:30") && tz_rule_offset_minutes(&r, 0, NULL) == -210);
    assert(tz_rule_fixed(0, &r) && !strcmp(r.text, "UTC0"));
    assert(!tz_rule_fixed(841, &r) && !tz_rule_fixed(-841, &r));

    /* ---- leap-seconds.list ---- */
    char text[512];
    leap_table_t t;
    snprintf(text, sizeof text, "%s%s", sample, sample_hash);
    assert(leap_table_parse(text, strlen(text), &t));
    assert(t.count == 2 && t.updated_unix_s == 100 - NTP_UNIX_OFFSET_S);
    assert(t.expires_unix_s == 4000000000LL - NTP_UNIX_OFFSET_S);
    assert(leap_table_tai_minus_utc(&t, 0) == 10);
    assert(leap_table_tai_minus_utc(&t, 78796799) == 10);
    assert(leap_table_tai_minus_utc(&t, 78796800) == 11);

    char broken[600];
    size_t n = strlen(text);
    memcpy(broken, text, n + 1);
    replace(broken, &n, "\t11\t", "\t12\t");            /* step of two seconds */
    assert(!leap_table_parse(broken, n, &t));
    n = strlen(text); memcpy(broken, text, n + 1);
    replace(broken, &n, "#@\t4000000000\n", "");        /* missing expiry */
    assert(!leap_table_parse(broken, n, &t));
    n = strlen(text); memcpy(broken, text, n + 1);
    replace(broken, &n, "2272060800\t10", "02272060800\t10"); /* non-canonical digits */
    assert(!leap_table_parse(broken, n, &t));
    n = strlen(text); memcpy(broken, text, n + 1);
    replace(broken, &n, "# 1 Jul 1972", "# 1 Jul 1972 (comment only)"); /* comments are unhashed */
    assert(leap_table_parse(broken, n, &t));
    n = strlen(text); memcpy(broken, text, n + 1);
    replace(broken, &n, "#$\t100\n", "#$\t101\n");      /* hash mismatch */
    assert(!leap_table_parse(broken, n, &t));
    assert(!leap_table_parse(sample, strlen(sample), &t)); /* no hash line */

    /* The distributed IERS file, when the host has one. */
    char *real = read_text("/usr/share/zoneinfo/leap-seconds.list", &n);
    if (real) {
        assert(leap_table_parse(real, n, &t));
        assert(t.count >= 28 && leap_table_tai_minus_utc(&t, 1782055035LL) == 37);
        assert(leap_table_tai_minus_utc(&t, 915148799LL) == 31);  /* 1998-12-31T23:59:59Z */
        assert(leap_table_tai_minus_utc(&t, 915148800LL) == 32);
        char *updated = strstr(real, "#$");
        assert(updated && (updated = strchr(updated, '\n')));
        updated[-1] = updated[-1] == '0' ? '1' : '0';   /* tamper with the hashed update time */
        assert(!leap_table_parse(real, n, &t));
        free(real);
        puts("system leap-seconds.list verified");
    }
    puts("POSIX TZ rules match libc; leap-seconds.list parsing and hash checks OK");
    return 0;
}
