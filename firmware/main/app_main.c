#include <stdio.h>

#include "cli.h"
#include "config_mgr.h"
#include "audio_mgr.h"
#include "display.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "http_srv.h"
#include "nvs_flash.h"
#include "rtc_mgr.h"
#include "sensors.h"
#include "storage_mgr.h"
#include "sntp_mgr.h"
#include "wifi_mgr.h"

static const char *TAG = "app_main";

void app_main(void)
{
    ESP_LOGI(TAG, "RLCD bring-up firmware starting");

    /* NVS is required by esp_wifi internals (even though we never store
     * credentials — wifi_mgr sets WIFI_STORAGE_RAM). */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (!wifi_mgr_start()) {
        ESP_LOGE(TAG, "wifi_mgr_start failed");
    }

    sntp_mgr_start();

    if (!sensors_start()) {
        ESP_LOGE(TAG, "sensors_start failed");
    }
    if (!rtc_mgr_start()) {
        ESP_LOGW(TAG, "RTC unavailable for boot holdover");
    }

    if (display_start()) {
        display_task_start();
    } else {
        ESP_LOGE(TAG, "display_start failed");
    }

    /* Optional removable storage; a failed mount must not stop the clock. */
    storage_mgr_start();
    config_mgr_start();
    /* Needs the shared I2C bus from sensors_start(). */
    if (!audio_mgr_start()) {
        ESP_LOGW(TAG, "audio unavailable");
    }

    if (!http_srv_start()) {
        ESP_LOGE(TAG, "http_srv_start failed");
    }

    ESP_LOGI(TAG, "ready — type `help` (commands: wifi, ntp, rtc, tz, leap, config, audio, sensor, sd, flash, http)");
    cli_start();   /* blocks forever: REPL on USB-Serial/JTAG */
}
