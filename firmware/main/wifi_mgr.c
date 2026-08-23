#include "wifi_mgr.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define MAX_RETRIES 5          /* total connect attempts per request */
#define RETRY_DELAY_MS 3000    /* between attempts (AP comeback/backoff) */

/* Credentials blob handed to the connect task (not persisted anywhere). */
typedef struct {
    char ssid[33];
    char password[65];
} creds_t;

static EventGroupHandle_t s_events;
static wifi_mgr_status_t  s_status;
static SemaphoreHandle_t  s_lock;
static int                s_retries_left;
static bool               s_auto_reconnect;   /* set while connect task alive */

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t event_id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state = WIFI_MGR_DISCONNECTED;
        s_status.ip[0] = '\0';
        bool retry = s_auto_reconnect && s_retries_left > 0;
        if (retry) s_retries_left--;
        xSemaphoreGive(s_lock);
        if (retry) {
            ESP_LOGW(TAG, "Wi-Fi disconnected; retrying in %d ms (%d left)",
                     RETRY_DELAY_MS, s_retries_left);
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "Wi-Fi disconnected");
            xEventGroupSetBits(s_events, WIFI_FAIL_BIT);
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base,
                        int32_t event_id, void *data)
{
    (void)arg; (void)base;
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        char ip[16];
        esp_ip4addr_ntoa(&evt->ip_info.ip, ip, sizeof ip);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state = WIFI_MGR_CONNECTED;
        strncpy(s_status.ip, ip, sizeof s_status.ip - 1);
        xSemaphoreGive(s_lock);
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "got IP: %s", ip);
    }
}

static void wifi_task(void *arg)
{
    creds_t *creds = (creds_t *)arg;
    wifi_config_t cfg = { .sta = {
        .threshold.authmode = WIFI_AUTH_WPA2_PSK,
    } };
    memcpy(cfg.sta.ssid, creds->ssid, sizeof cfg.sta.ssid);
    if (creds->password[0]) {
        memcpy(cfg.sta.password, creds->password, sizeof cfg.sta.password);
    }
    free(creds);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to \"%s\"", (char *)cfg.sta.ssid);

    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    EventBits_t bits = xEventGroupWaitBits(s_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(120000));
    bool ok = (bits & WIFI_CONNECTED_BIT) != 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_auto_reconnect = false;
    if (!ok) {
        s_status.state = WIFI_MGR_DISCONNECTED;
        esp_wifi_stop();
    }
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

bool wifi_mgr_start(void)
{
    s_events = xEventGroupCreate();
    s_lock = xSemaphoreCreateMutex();
    if (!s_events || !s_lock) return false;
    memset(&s_status, 0, sizeof s_status);
    s_status.state = WIFI_MGR_DISCONNECTED;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    /* Non-persistent: never write credentials/config to NVS. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_ip_event, NULL));
    return true;
}

bool wifi_mgr_connect(const char *ssid, const char *password, int timeout_s)
{
    (void)timeout_s;
    if (!ssid || !*ssid) return false;

    creds_t *creds = calloc(1, sizeof *creds);
    if (!creds) return false;
    strncpy(creds->ssid, ssid, sizeof creds->ssid - 1);
    if (password) strncpy(creds->password, password, sizeof creds->password - 1);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = WIFI_MGR_CONNECTING;
    strncpy(s_status.ssid, creds->ssid, sizeof s_status.ssid - 1);
    s_retries_left = MAX_RETRIES;
    s_auto_reconnect = true;
    xSemaphoreGive(s_lock);

    return xTaskCreate(wifi_task, "wifi", 4096, creds, 5, NULL) == pdPASS;
}

void wifi_mgr_disconnect(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_auto_reconnect = false;
    xSemaphoreGive(s_lock);
    esp_wifi_stop();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = WIFI_MGR_DISCONNECTED;
    s_status.ip[0] = '\0';
    xSemaphoreGive(s_lock);
}

wifi_mgr_status_t wifi_mgr_status(void)
{
    wifi_mgr_status_t s;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s = s_status;
    xSemaphoreGive(s_lock);

    /* Real RSSI when connected (0 = unknown). */
    if (s.state == WIFI_MGR_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s.rssi_dbm = ap.rssi;
        }
    }
    return s;
}
