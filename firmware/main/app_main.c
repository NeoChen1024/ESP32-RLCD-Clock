#include <stdio.h>

#include "cli.h"
#include "display.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "http_srv.h"
#include "nvs_flash.h"
#include "sensors.h"
#include "sntp_mgr.h"
#include "wifi_mgr.h"

static const char *TAG = "app_main";

static void on_ip_event(void *arg, esp_event_base_t base,
                        int32_t event_id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (event_id == IP_EVENT_STA_GOT_IP) {
        /* Network is up: re-apply SNTP server selection so DHCP-provided
         * NTP servers (option 42) are picked up. */
        sntp_mgr_wifi_connected();
    }
}

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

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_ip_event, NULL));

    sntp_mgr_start();

    if (!sensors_start()) {
        ESP_LOGE(TAG, "sensors_start failed");
    }

    if (display_start()) {
        display_task_start();
    } else {
        ESP_LOGE(TAG, "display_start failed");
    }

    if (!http_srv_start()) {
        ESP_LOGE(TAG, "http_srv_start failed");
    }

    ESP_LOGI(TAG, "ready — type `help` (commands: wifi, ntp, http)");
    cli_start();   /* blocks forever: REPL on the console UART */
}
