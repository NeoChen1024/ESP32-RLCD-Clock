#include "config_mgr.h"
#include "model.h"
#include "sntp_mgr.h"
#include "storage_mgr.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "config";
static config_selection_t s_selected;
static bool s_started;

static bool parse_config(const char *path, config_selection_t *parsed)
{
    if (!storage_validate_file("config/check.json", path)) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return false; }
    long length = ftell(f);
    if (length < 2 || length > STORAGE_JSON_MAX || fseek(f, 0, SEEK_SET)) { fclose(f); return false; }
    char *buf = malloc((size_t)length + 1);
    bool ok = buf && fread(buf, 1, length, f) == (size_t)length;
    if (fclose(f)) ok = false;
    if (!ok) { free(buf); return false; }
    buf[length] = 0;
    cJSON *json = cJSON_ParseWithLengthOpts(buf, (size_t)length + 1, NULL, true);
    free(buf);
    if (!cJSON_IsObject(json)) { cJSON_Delete(json); return false; }
    memset(parsed, 0, sizeof *parsed);
    parsed->tz_offset_minutes = 480;
    cJSON *tz = cJSON_GetObjectItemCaseSensitive(json, "tz_offset_minutes");
    if (tz) {
        if (!cJSON_IsNumber(tz) || tz->valuedouble < -840 || tz->valuedouble > 840 ||
            tz->valuedouble != (int)tz->valuedouble) { cJSON_Delete(json); return false; }
        parsed->tz_offset_minutes = (int)tz->valuedouble;
    }
    cJSON *server = cJSON_GetObjectItemCaseSensitive(json, "ntp_server");
    if (server && !cJSON_IsNull(server)) {
        if (!cJSON_IsString(server) || !server->valuestring) { cJSON_Delete(json); return false; }
        size_t n = strlen(server->valuestring);
        if (!n || n >= sizeof parsed->ntp_server) { cJSON_Delete(json); return false; }
        for (const unsigned char *p = (const unsigned char *)server->valuestring; *p; ++p)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= 'a' && *p <= 'z') || *p == '.' || *p == '-')) {
                cJSON_Delete(json); return false;
            }
        memcpy(parsed->ntp_server, server->valuestring, n + 1);
    }
    cJSON_Delete(json);
    return true;
}
bool config_mgr_file_valid(const char *path)
{
    config_selection_t parsed;
    return parse_config(path, &parsed);
}
static bool candidate(const char *path, void *context)
{
    config_selection_t *parsed = context;
    return parse_config(path, parsed);
}
bool config_mgr_reload_locked(void)
{
    const char *volumes[] = {"sd", "flash"};
    config_selection_t next = {.tz_offset_minutes = 480};
    for (unsigned i = 0; i < 2; ++i) {
        const char *volume = volumes[i];
        if (!storage_mounted_locked(volume)) continue;
        config_selection_t parsed;
        char selected[STORAGE_REL_MAX];
        int result = storage_config_select(storage_root(volume), candidate, &parsed, selected);
        if (result < 0) ESP_LOGW(TAG, "cannot scan %s config directory", volume);
        if (result != 1) continue;
        next = parsed;
        next.found = true;
        snprintf(next.volume, sizeof next.volume, "%s", volume);
        snprintf(next.path, sizeof next.path, "%s", selected);
        break;
    }
    model_tz_set_config(next.tz_offset_minutes);
    sntp_mgr_set_config_server(next.ntp_server[0] ? next.ntp_server : NULL);
    s_selected = next;
    if (next.found) ESP_LOGI(TAG, "selected %s/%s (TZ %+d, NTP %s)",
                            next.volume, next.path, next.tz_offset_minutes,
                            next.ntp_server[0] ? next.ntp_server : "DHCP/fallback");
    else ESP_LOGI(TAG, "no usable config; default TZ and DHCP/fallback NTP");
    return next.found;
}
void config_mgr_current_locked(config_selection_t *out) { *out = s_selected; }
bool config_mgr_reload(void)
{
    if (!storage_lock(5000)) return false;
    bool loaded = config_mgr_reload_locked();
    storage_unlock();
    return loaded;
}
void config_mgr_start(void)
{
    s_started = true;
    config_mgr_reload();
}
/* Used by storage diagnostics after SD/flash mount changes. */
bool config_mgr_started(void) { return s_started; }
