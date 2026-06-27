#include "render_faces.h"
#include "sdl3_backend.h"   /* DISP_W / DISP_H */
#include "time_model.h"
#include "u8g2.h"

#include <stdio.h>
#include <string.h>

/*
 * Single face. All time-scale telemetry on one 400x300 page.
 *
 * Fonts (built into u8g2):
 *   big time     : u8g2_font_inr42_mr   (42px, has ':' digits symbols)
 *   scale values : u8g2_font_courB24_tr (24px courier bold, full charset)
 *   labels/mono  : u8g2_font_7x13_tr
 *   top/bottom   : u8g2_font_5x8_tr
 */

#define FONT_BIG     u8g2_font_inr42_mr
#define FONT_SCALE   u8g2_font_courB24_tr
#define FONT_MONO    u8g2_font_7x13_tr
#define FONT_SMALL   u8g2_font_5x8_tr

#define LABEL_X  8
#define VALUE_X  78
#define RIGHT_X  (DISP_W - 8)

static const char *sync_label(sync_state_t s)
{
    switch (s) {
    case SYNC_BOOT_UNS:  return "BOOT UNS";
    case SYNC_SYNCING:   return "SYNC...";
    case SYNC_NTP_OK:    return "NTP OK";
    case SYNC_RTC_HOLD:  return "RTC HOLD";
    case SYNC_WIFI_LOST: return "WIFI LOST";
    }
    return "????????";
}

static void draw_str_right(u8g2_t *g, int x_right, int y, const char *s)
{
    int w = u8g2_GetStrWidth(g, s);
    u8g2_DrawStr(g, x_right - w, y, s);
}

/* ---- field formatting ---- */

static void fmt_local_date(const clock_model_t *m, int tz, char *out, size_t n)
{
    int y, mo, d, wd, h, mi, s;
    civil_fields(m, tz, &y, &mo, &d, &wd, &h, &mi, &s);
    const char *wdn[] = {"MON","TUE","WED","THU","FRI","SAT","SUN"};
    snprintf(out, n, "%04d-%02d-%02d %s", y, mo, d, wdn[wd]);
    (void)h; (void)mi; (void)s;
}

static void fmt_hms(const clock_model_t *m, int tz, char *out, size_t n)
{
    int y, mo, d, wd, h, mi, s;
    civil_fields(m, tz, &y, &mo, &d, &wd, &h, &mi, &s);
    snprintf(out, n, "%02d:%02d:%02d", h, mi, s);
    (void)y; (void)mo; (void)d; (void)wd;
}

static void fmt_tz_str(int tz, char *out, size_t n)
{
    int sign = tz < 0 ? -1 : 1;
    int az = tz < 0 ? -tz : tz;
    snprintf(out, n, "%c%02d%02d", sign < 0 ? '-' : '+', az / 60, az % 60);
}

static void fmt_utc_hms(const clock_model_t *m, char *out, size_t n)
{
    int y, mo, d, wd, h, mi, s;
    civil_fields(m, 0, &y, &mo, &d, &wd, &h, &mi, &s);
    snprintf(out, n, "%02d:%02d:%02dZ", h, mi, s);
    (void)y; (void)mo; (void)d; (void)wd;
}

static void fmt_mjd_tai(const clock_model_t *m, char *out, size_t n)
{
    int64_t day, frac;
    mjd_tai(m, &day, &frac);
    snprintf(out, n, "%07lld.%07lld", (long long)day, (long long)frac);
}

static void fmt_gps(const clock_model_t *m, char *out, size_t n)
{
    int64_t w, sow;
    gps_week_sow(m, &w, &sow);
    snprintf(out, n, "W=%04lld SOW=%06lld", (long long)w, (long long)sow);
}

static void fmt_iso_week(const clock_model_t *m, char *out, size_t n)
{
    int iy, iw, iwd;
    iso_week_date(m, &iy, &iw, &iwd);
    snprintf(out, n, "%04d-W%02d-%d", iy, iw, iwd);
}

/* ---- helpers ---- */

static void draw_label_value(u8g2_t *g, int y, const char *label, const char *value)
{
    u8g2_SetFont(g, FONT_MONO);
    u8g2_DrawStr(g, LABEL_X, y, label);
    u8g2_SetFont(g, FONT_SCALE);
    u8g2_DrawStr(g, VALUE_X, y, value);
}

static void draw_scale_placeholder(u8g2_t *g, int y, const char *label, const char *dash)
{
    u8g2_SetFont(g, FONT_MONO);
    u8g2_DrawStr(g, LABEL_X, y, label);
    u8g2_SetFont(g, FONT_SCALE);
    u8g2_DrawStr(g, VALUE_X, y, dash);
}

/* ====================================================================== */
/* Single face                                                            */
/* ====================================================================== */

static void render_face_single(u8g2_t *g, const clock_model_t *m)
{
    int tz = host_tz_offset_minutes();
    char buf[40];

    /* ---- Top bar ---- */
    u8g2_SetFont(g, FONT_SMALL);
    fmt_local_date(m, tz, buf, sizeof buf);
    u8g2_DrawStr(g, 4, 14, buf);

    char topbar_right[40];
    snprintf(topbar_right, sizeof topbar_right, "[wifi] %s", sync_label(m->sync));
    draw_str_right(g, RIGHT_X, 14, topbar_right);

    /* ---- Main time (big, centered) ---- */
    u8g2_SetFont(g, FONT_BIG);
    fmt_hms(m, tz, buf, sizeof buf);
    int tw = u8g2_GetStrWidth(g, buf);
    u8g2_DrawStr(g, (DISP_W - tw) / 2, 70, buf);

    /* tz offset + UTC time under the big time (inr42 has a 21px descender,
     * so the big time reaches ~y91; place this line below it) */
    u8g2_SetFont(g, FONT_MONO);
    char tzbuf[8]; fmt_tz_str(tz, tzbuf, sizeof tzbuf);
    char utcbuf[16]; fmt_utc_hms(m, utcbuf, sizeof utcbuf);
    snprintf(buf, sizeof buf, "%s   UTC %s", tzbuf, utcbuf);
    int uw = u8g2_GetStrWidth(g, buf);
    u8g2_DrawStr(g, (DISP_W - uw) / 2, 106, buf);

    /* separator */
    u8g2_DrawHLine(g, 8, 118, DISP_W - 16);

    /* ---- Time-scale rows ---- */
    int y = 142;

    /* ISO week date (own row) */
    u8g2_SetFont(g, FONT_MONO);
    u8g2_DrawStr(g, LABEL_X, y, "ISO");
    char isobuf[20]; fmt_iso_week(m, isobuf, sizeof isobuf);
    u8g2_SetFont(g, FONT_SCALE);
    u8g2_DrawStr(g, VALUE_X, y, isobuf);

    /* MJD(TAI) — signature field */
    y += 30;
    if (m->time_trusted) {
        char mjdbuf[24]; fmt_mjd_tai(m, mjdbuf, sizeof mjdbuf);
        draw_label_value(g, y, "MJDTAI", mjdbuf);
    } else {
        draw_scale_placeholder(g, y, "MJDTAI", "--------.-------");
    }

    /* GPS week / SOW */
    y += 30;
    if (m->time_trusted) {
        char gpsbuf[24]; fmt_gps(m, gpsbuf, sizeof gpsbuf);
        draw_label_value(g, y, "GPS", gpsbuf);
    } else {
        draw_scale_placeholder(g, y, "GPS", "W---- SOW------");
    }

    /* offset constants row: all three TAI/UTC/GPS deltas together */
    y += 28;
    u8g2_SetFont(g, FONT_MONO);
    snprintf(buf, sizeof buf, "dAT +%ds   dUT +%ds   dTG +%ds",
             TAI_MINUS_UTC_SECONDS,
             GPS_MINUS_UTC_SECONDS,
             TAI_MINUS_UTC_SECONDS - GPS_MINUS_UTC_SECONDS);
    draw_str_right(g, RIGHT_X, y, buf);

    /* ---- Telemetry (bottom) ---- */
    u8g2_DrawHLine(g, 8, 268, DISP_W - 16);
    u8g2_SetFont(g, FONT_SMALL);
    snprintf(buf, sizeof buf, "[tmp] +%.1fC   [rh] %.0f%%   [bat] %.2fV",
             m->temp_c, m->rh_pct, m->batt_v);
    u8g2_DrawStr(g, 4, 284, buf);

    char tel2[48];
    snprintf(tel2, sizeof tel2, "ntp +%lus   wifi %d dBm",
             (unsigned long)m->ntp_age_s, m->wifi_rssi_dbm);
    u8g2_DrawStr(g, 4, 296, tel2);
}

void render_face(u8g2_t *g, const clock_model_t *m)
{
    u8g2_ClearBuffer(g);
    render_face_single(g, m);
}
