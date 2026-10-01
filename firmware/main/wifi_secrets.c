#include "wifi_secrets.h"
#include "wifi_mgr.h"
#include "storage_files.h"
#include "storage_mgr.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "wifi_secrets";
static wifi_secrets_info_t s_current;
/* Serialized by the storage mutex; kept off small task stacks. */
static wifi_mgr_network_t s_networks[WIFI_MGR_KNOWN_MAX];

static bool password_valid(const char *p)
{
    size_t n = strlen(p);
    if (!n) return true;
    if (n == 64) {
        for (size_t i = 0; i < n; ++i)
            if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f') ||
                  (p[i] >= 'A' && p[i] <= 'F'))) return false;
        return true;
    }
    if (n < 8 || n > 63) return false;
    for (size_t i = 0; i < n; ++i) if (p[i] < 0x20 || p[i] > 0x7e) return false;
    return true;
}

static bool parse(const char *path, wifi_mgr_network_t *out, unsigned *count)
{
    if (!storage_validate_file(STORAGE_WIFI_SECRETS, path)) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char *buf = malloc(STORAGE_JSON_MAX + 1);
    size_t n = buf ? fread(buf, 1, STORAGE_JSON_MAX, f) : 0;
    bool ok = buf && !ferror(f);
    if (fclose(f)) ok = false;
    cJSON *json = ok ? cJSON_ParseWithLength(buf, n) : NULL;
    if (buf) { memset(buf, 0, n); free(buf); }
    cJSON *networks = cJSON_GetObjectItemCaseSensitive(json, "networks");
    ok = cJSON_IsObject(json) && cJSON_IsArray(networks) &&
         cJSON_GetArraySize(networks) <= WIFI_MGR_KNOWN_MAX;
    *count = 0;
    cJSON *entry;
    if (ok) cJSON_ArrayForEach(entry, networks) {
        cJSON *ssid = cJSON_GetObjectItemCaseSensitive(entry, "ssid");
        cJSON *password = cJSON_GetObjectItemCaseSensitive(entry, "password");
        if (!cJSON_IsObject(entry) || !cJSON_IsString(ssid) || !ssid->valuestring ||
            !*ssid->valuestring || strlen(ssid->valuestring) > 32 ||
            (password && !cJSON_IsNull(password) &&
             (!cJSON_IsString(password) || !password->valuestring ||
              !password_valid(password->valuestring)))) { ok = false; break; }
        for (unsigned i = 0; i < *count; ++i)
            if (!strcmp(out[i].ssid, ssid->valuestring)) ok = false;
        if (!ok) break;
        wifi_mgr_network_t *net = &out[(*count)++];
        memset(net, 0, sizeof *net);
        memcpy(net->ssid, ssid->valuestring, strlen(ssid->valuestring));
        if (cJSON_IsString(password))
            memcpy(net->password, password->valuestring, strlen(password->valuestring));
    }
    if (json && networks) {
        /* Scrub passwords from cJSON's heap copies before freeing them. */
        cJSON_ArrayForEach(entry, networks) {
            cJSON *password = cJSON_GetObjectItemCaseSensitive(entry, "password");
            if (cJSON_IsString(password) && password->valuestring)
                memset(password->valuestring, 0, strlen(password->valuestring));
        }
    }
    cJSON_Delete(json);
    if (!ok) { memset(out, 0, sizeof *out * WIFI_MGR_KNOWN_MAX); *count = 0; }
    return ok;
}

bool wifi_secrets_file_valid(const char *path)
{
    unsigned count;
    bool ok = parse(path, s_networks, &count);
    memset(s_networks, 0, sizeof s_networks);
    return ok;
}

bool wifi_secrets_reload_locked(void)
{
    const char *volume = storage_active_volume_locked();
    wifi_secrets_info_t next = {0};
    if (volume) {
        char path[STORAGE_PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", storage_root(volume), STORAGE_WIFI_SECRETS);
        if (parse(path, s_networks, &next.count)) {
            next.found = true;
            snprintf(next.volume, sizeof next.volume, "%s", volume);
        }
    }
    wifi_mgr_set_known(s_networks, next.count);
    if (next.found) {
        ESP_LOGI(TAG, "%s/%s: %u known network%s", next.volume, STORAGE_WIFI_SECRETS,
                 next.count, next.count == 1 ? "" : "s");
        for (unsigned i = 0; i < next.count; ++i) ESP_LOGI(TAG, "  \"%s\"", s_networks[i].ssid);
    } else {
        ESP_LOGI(TAG, "no valid %s; no known networks", STORAGE_WIFI_SECRETS);
    }
    memset(s_networks, 0, sizeof s_networks);
    s_current = next;
    return next.found;
}

void wifi_secrets_current_locked(wifi_secrets_info_t *out) { *out = s_current; }
