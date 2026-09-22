#include "model.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "sensors.h"
#include "sntp_mgr.h"
#include "wifi_mgr.h"

static const char *TAG = "model";

#define TZ_DEFAULT_MINUTES (8 * 60)   /* UTC+8 */

static int s_tz_minutes = TZ_DEFAULT_MINUTES;
static int s_config_tz_minutes = TZ_DEFAULT_MINUTES;
static bool s_cli_tz;

void model_tz_set_default(void) { s_cli_tz = false; s_tz_minutes = s_config_tz_minutes; }
void model_tz_set_config(int minutes)
{
    s_config_tz_minutes = minutes;
    if (!s_cli_tz) s_tz_minutes = minutes;
}
int  model_tz_get(void)         { return s_tz_minutes; }

void model_tz_set(int minutes)
{
    s_cli_tz = true;
    s_tz_minutes = minutes;
    ESP_LOGI(TAG, "TZ offset set to %+d min (%c%02d:%02d)", minutes,
             minutes < 0 ? '-' : '+', (minutes < 0 ? -minutes : minutes) / 60,
             (minutes < 0 ? -minutes : minutes) % 60);
}

/* ---- platform hooks for the shared time model ---- */

int tz_offset_minutes(void)
{
    return s_tz_minutes;
}

void time_model_now(clock_model_t *m)
{
    memset(m, 0, sizeof *m);

    sntp_mgr_status_t s = sntp_mgr_status();
    m->unix_ms = s.unix_ms;
    wifi_mgr_status_t w = wifi_mgr_status();

    /* Trust gating: only a completed SNTP sync makes the time trustworthy.
     * The top-bar sync state is the single source of trust (notes §4.1). */
    if (s.started && s.time_trusted) {
        m->time_trusted = true;
        if (w.state == WIFI_MGR_CONNECTED) {
            m->sync = s.fresh ? SYNC_NTP_OK : SYNC_RTC_HOLD;
        } else {
            m->sync = SYNC_WIFI_LOST;
        }
    } else if (s.synced || s.rtc_seeded) {
        m->sync = SYNC_TIME_UNSAFE;
    } else if (w.state == WIFI_MGR_CONNECTED) {
        m->sync = SYNC_SYNCING;          /* connected, waiting on SNTP */
    } else if (w.state == WIFI_MGR_CONNECTING) {
        m->sync = SYNC_SYNCING;
    } else {
        m->sync = SYNC_BOOT_UNS;
    }

    m->ntp_age_s = s.ntp_age_s;
    m->wifi_rssi_dbm = w.rssi_dbm;

    /* Real telemetry (sensors wired since bring-up). SHTC3 read takes ~20ms;
     * failures leave the previous values in place. */
    static float last_temp = 0.0f, last_rh = 0.0f;
    if (sensors_read_temp_humi(&last_temp, &last_rh)) {
        m->temp_c = last_temp;
        m->rh_pct = last_rh;
    } else {
        m->temp_c = last_temp;
        m->rh_pct = last_rh;
    }
    m->batt_v = sensors_read_batt_v();
}
