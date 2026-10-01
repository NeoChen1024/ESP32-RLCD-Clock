#include "leap_table.h"
#include "sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static bool number(const char **p, const char *end, int64_t *value)
{
    while (*p < end && is_space(**p)) ++*p;
    const char *start = *p;
    int64_t v = 0;
    while (*p < end && **p >= '0' && **p <= '9') {
        if (v > 999999999999LL) return false;
        v = v * 10 + (**p - '0');
        ++*p;
    }
    /* The hash covers canonical decimal text, so leading zeros cannot match. */
    if (*p == start || (*start == '0' && *p - start > 1)) return false;
    *value = v;
    return true;
}

static bool rest_blank(const char *p, const char *end, bool comment_ok)
{
    while (p < end && is_space(*p)) ++p;
    return p == end || (comment_ok && *p == '#');
}

static bool hash_words(const char *p, const char *end, uint32_t words[5])
{
    for (unsigned i = 0; i < 5; ++i) {
        while (p < end && is_space(*p)) ++p;
        uint32_t v = 0;
        unsigned n = 0;
        for (; p < end && !is_space(*p); ++p, ++n) {
            char c = *p;
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                    c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0 || n >= 8) return false;
            v = v << 4 | (uint32_t)d;
        }
        /* IERS files may drop leading zeros, so compare word values. */
        if (!n) return false;
        words[i] = v;
    }
    return rest_blank(p, end, false);
}

bool leap_table_parse(const char *text, size_t length, leap_table_t *out)
{
    if (!text || !out || length > LEAP_FILE_MAX || memchr(text, 0, length)) return false;
    memset(out, 0, sizeof *out);
    int64_t updated = -1, expires = -1;
    uint32_t expected[5];
    bool have_hash = false;
    const char *end = text + length;
    for (const char *line = text; line < end;) {
        const char *eol = memchr(line, '\n', (size_t)(end - line));
        if (!eol) eol = end;
        const char *p = line;
        int64_t value;
        if (eol - line >= 2 && line[0] == '#' && (line[1] == '$' || line[1] == '@')) {
            int64_t *slot = line[1] == '$' ? &updated : &expires;
            p += 2;
            if (*slot >= 0 || !number(&p, eol, &value) || !rest_blank(p, eol, false)) return false;
            *slot = value;
        } else if (eol - line >= 2 && line[0] == '#' && line[1] == 'h') {
            if (have_hash || !hash_words(line + 2, eol, expected)) return false;
            have_hash = true;
        } else if (line[0] != '#' && !rest_blank(line, eol, false)) {
            int64_t ntp, offset;
            if (out->count >= LEAP_TABLE_MAX || !number(&p, eol, &ntp) ||
                !number(&p, eol, &offset) || !rest_blank(p, eol, true) ||
                ntp < NTP_UNIX_OFFSET_S || offset < 10 || offset > 1000) return false;
            leap_entry_t *e = &out->entries[out->count];
            e->unix_s = ntp - NTP_UNIX_OFFSET_S;
            e->tai_minus_utc = (int16_t)offset;
            if (out->count) {
                const leap_entry_t *prev = e - 1;
                int step = e->tai_minus_utc - prev->tai_minus_utc;
                if (e->unix_s <= prev->unix_s || (step != 1 && step != -1)) return false;
            }
            out->count++;
        }
        line = eol < end ? eol + 1 : end;
    }
    if (!out->count || updated < 0 || expires < 0 || !have_hash || expires <= updated ||
        expires - NTP_UNIX_OFFSET_S <= out->entries[out->count - 1].unix_s) return false;
    /* The hash covers update, expiry, then each data line's two fields. */
    char digits[48];
    int n = snprintf(digits, sizeof digits, "%lld%lld", (long long)updated, (long long)expires);
    sha1_ctx_t c;
    uint32_t actual[5];
    sha1_init(&c);
    sha1_update(&c, digits, (size_t)n);
    for (unsigned i = 0; i < out->count; ++i) {
        const leap_entry_t *e = &out->entries[i];
        n = snprintf(digits, sizeof digits, "%lld%d",
                     (long long)(e->unix_s + NTP_UNIX_OFFSET_S), e->tai_minus_utc);
        sha1_update(&c, digits, (size_t)n);
    }
    sha1_final(&c, actual);
    if (memcmp(actual, expected, sizeof actual)) return false;
    out->updated_unix_s = updated - NTP_UNIX_OFFSET_S;
    out->expires_unix_s = expires - NTP_UNIX_OFFSET_S;
    return true;
}

bool leap_table_load(const char *path, leap_table_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char *buf = malloc(LEAP_FILE_MAX + 1);
    size_t n = buf ? fread(buf, 1, LEAP_FILE_MAX + 1, f) : 0;
    bool ok = buf && !ferror(f) && n <= LEAP_FILE_MAX;
    if (fclose(f)) ok = false;
    if (ok) ok = leap_table_parse(buf, n, out);
    free(buf);
    return ok;
}

int leap_table_tai_minus_utc(const leap_table_t *t, int64_t unix_s)
{
    unsigned i = t->count;
    while (i > 1 && unix_s < t->entries[i - 1].unix_s) --i;
    return t->entries[i ? i - 1 : 0].tai_minus_utc;
}
