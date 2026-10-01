#include "model.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor_health.h"
#include "sensors.h"
#include "sntp_mgr.h"
#include "wifi_mgr.h"

static const char *TAG = "model";

/* Render, CLI, HTTP and storage tasks share these; copies are short. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

#define DEFAULT_RULE { .text = MODEL_TZ_DEFAULT, .std_east_s = 8 * 3600, .dst_east_s = 8 * 3600 }
static tz_rule_t s_tz = DEFAULT_RULE;
static tz_rule_t s_config_tz = DEFAULT_RULE;
static bool s_cli_tz;

static leap_table_t s_leap;
static model_leap_info_t s_leap_info;

void model_tz_set_default(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_cli_tz = false;
    s_tz = s_config_tz;
    taskEXIT_CRITICAL(&s_lock);
}

void model_tz_set_config(const tz_rule_t *rule)
{
    static const tz_rule_t fallback = DEFAULT_RULE;
    taskENTER_CRITICAL(&s_lock);
    s_config_tz = rule ? *rule : fallback;
    if (!s_cli_tz) s_tz = s_config_tz;
    taskEXIT_CRITICAL(&s_lock);
}

void model_tz_set(const tz_rule_t *rule)
{
    taskENTER_CRITICAL(&s_lock);
    s_cli_tz = true;
    s_tz = *rule;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "TZ rule set to %s", rule->text);
}

void model_tz_get(tz_rule_t *out, bool *cli_override)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_tz;
    if (cli_override) *cli_override = s_cli_tz;
    taskEXIT_CRITICAL(&s_lock);
}

void model_leap_set(const leap_table_t *table, const char *volume)
{
    taskENTER_CRITICAL(&s_lock);
    memset(&s_leap_info, 0, sizeof s_leap_info);
    if (table) {
        s_leap = *table;
        s_leap_info.loaded = true;
        snprintf(s_leap_info.volume, sizeof s_leap_info.volume, "%s", volume);
        s_leap_info.updated_unix_s = table->updated_unix_s;
        s_leap_info.expires_unix_s = table->expires_unix_s;
    }
    taskEXIT_CRITICAL(&s_lock);
}

void model_leap_info(model_leap_info_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_leap_info;
    taskEXIT_CRITICAL(&s_lock);
}

int model_tai_minus_utc(int64_t unix_s)
{
    taskENTER_CRITICAL(&s_lock);
    int value = s_leap_info.loaded ? leap_table_tai_minus_utc(&s_leap, unix_s)
                                   : TAI_MINUS_UTC_BUILTIN_S;
    taskEXIT_CRITICAL(&s_lock);
    return value;
}

/* ---- platform hook for the shared time model ---- */

void time_model_now(clock_model_t *m)
{
    memset(m, 0, sizeof *m);

    sntp_mgr_status_t s = sntp_mgr_status();
    m->unix_ms = s.unix_ms;
    wifi_mgr_status_t w = wifi_mgr_status();

    /* The time state (clock_health.h) decides validity; the top-bar label
     * adds freshness and link detail. */
    m->time_valid = s.started && s.time_valid;
    if (m->time_valid && s.time_state == CLOCK_RTC_HOLD) {
        m->sync = SYNC_RTC_HOLD;
    } else if (m->time_valid) {
        if (w.state == WIFI_MGR_CONNECTED) {
            m->sync = s.fresh ? SYNC_NTP_OK : SYNC_HOLDOVER;
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

    int64_t unix_s = m->unix_ms / 1000 - (m->unix_ms % 1000 < 0);
    tz_rule_t rule;
    model_tz_get(&rule, NULL);
    m->tz_offset_min = tz_rule_offset_minutes(&rule, unix_s, NULL);
    m->tai_minus_utc_s = model_tai_minus_utc(unix_s);

    m->ntp_age_s = s.ntp_age_s;
    m->wifi_rssi_dbm = w.rssi_dbm;

    /* Keep a short SHTC3 grace period for transient I2C failures; never
     * present an indefinitely old sample as a live measurement. */
    static sensor_sample_cache_t shtc3_cache;
    float temp, rh;
    bool measured = sensors_read_temp_humi(&temp, &rh);
    int64_t now_us = esp_timer_get_time();
    if (measured) sensor_sample_cache_record(&shtc3_cache, now_us, temp, rh);
    m->temp_humi_valid = sensor_sample_cache_current(&shtc3_cache, now_us,
                                                       &m->temp_c, &m->rh_pct);
    m->batt_valid = sensors_read_batt_v(&m->batt_v);
}
