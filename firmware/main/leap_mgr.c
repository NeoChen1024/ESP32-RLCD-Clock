#include "leap_mgr.h"
#include "model.h"
#include "storage_mgr.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "leap";
/* Serialized by the storage mutex; kept off small task stacks. */
static leap_table_t s_best;

bool leap_mgr_reload_locked(void)
{
    const char *volume = storage_active_volume_locked(), *selected = NULL;
    if (volume) {
        char path[96];
        snprintf(path, sizeof path, "%s/%s", storage_root(volume), LEAP_MGR_RELATIVE);
        if (leap_table_load(path, &s_best)) selected = volume;
    }
    model_leap_set(selected ? &s_best : NULL, selected);
    if (selected) ESP_LOGI(TAG, "using %s/%s (%u entries, expires unix %lld)", selected,
                           LEAP_MGR_RELATIVE, s_best.count, (long long)s_best.expires_unix_s);
    else ESP_LOGI(TAG, "no verified leap-seconds.list; built-in TAI-UTC %d s", TAI_MINUS_UTC_BUILTIN_S);
    return selected != NULL;
}
