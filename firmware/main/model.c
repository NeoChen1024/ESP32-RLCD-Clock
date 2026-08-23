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

void model_tz_set_default(void) { s_tz_minutes = TZ_DEFAULT_MINUTES; }
int  model_tz_get(void)         { return s_tz_minutes; }

void model_tz_set(int minutes)
{
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

    struct timeval tv;
    gettimeofday(&tv, NULL);
    m->unix_ms = (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;

    sntp_mgr_status_t s = sntp_mgr_status();
    wifi_mgr_status_t w = wifi_mgr_status();

    /* Trust gating: only a completed SNTP sync makes the time trustworthy.
     * The top-bar sync state is the single source of trust (notes §4.1). */
    if (s.started && s.synced) {
        m->time_trusted = true;
        if (w.state == WIFI_MGR_CONNECTED) {
            m->sync = SYNC_NTP_OK;
        } else {
            m->sync = SYNC_WIFI_LOST;
        }
        m->ntp_age_s = s.ntp_age_s;
    } else if (w.state == WIFI_MGR_CONNECTED) {
        m->sync = SYNC_SYNCING;          /* connected, waiting on SNTP */
    } else if (w.state == WIFI_MGR_CONNECTING) {
        m->sync = SYNC_SYNCING;
    } else {
        m->sync = SYNC_BOOT_UNS;
    }

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
