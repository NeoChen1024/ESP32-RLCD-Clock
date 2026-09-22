#include "wifi_mgr.h"
#include "sntp_mgr.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "wifi_mgr";
ESP_EVENT_DEFINE_BASE(RLCD_WIFI_CONTROL);
enum { CONTROL_CONNECT, CONTROL_DISCONNECT, CONTROL_TICK, CONTROL_RECONNECT };
typedef struct { char ssid[33]; char password[65]; } credentials_t;

/* The default event loop owns the driver and reconnect policy. API commands
 * are copied into that loop; its handlers never sleep for retry backoff.
 * The mutex only protects status snapshots against CLI/display readers. */
static SemaphoreHandle_t s_lock;
static wifi_mgr_status_t s_status;
static wifi_config_t s_config;
static bool s_desired, s_running, s_stopping, s_attempt;
static unsigned s_backoff_s = 1;
static int64_t s_retry_at_us, s_attempt_until_us;
static esp_timer_handle_t s_timer;

static void schedule_retry(void)
{
    s_attempt = false;
    s_status.state = s_desired ? WIFI_MGR_CONNECTING : WIFI_MGR_DISCONNECTED;
    s_status.ip[0] = '\0';
    s_status.rssi_dbm = 0;
    s_retry_at_us = esp_timer_get_time() + (int64_t)s_backoff_s * 1000000;
    if (s_backoff_s < 30) s_backoff_s = s_backoff_s * 2 > 30 ? 30 : s_backoff_s * 2;
}

static void start_station(void)
{
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &s_config);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err == ESP_OK) {
        s_running = true;
        s_status.state = WIFI_MGR_CONNECTING;
    } else {
        ESP_LOGW(TAG, "station start failed: %s", esp_err_to_name(err));
        schedule_retry();
    }
}

static void connect_station(void)
{
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        s_attempt = true;
        s_attempt_until_us = esp_timer_get_time() + 30LL * 1000000;
    } else {
        ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
        schedule_retry();
    }
}

static void stop_station(void)
{
    if (!s_running || s_stopping) return;
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_OK) s_stopping = true;
    else ESP_LOGW(TAG, "station stop failed: %s", esp_err_to_name(err));
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (base == RLCD_WIFI_CONTROL) {
        if (id == CONTROL_CONNECT) {
            credentials_t *c = data;
            s_desired = true;
            s_backoff_s = 1;
            s_attempt = false;
            memset(&s_config, 0, sizeof s_config);
            memcpy(s_config.sta.ssid, c->ssid, strlen(c->ssid));
            memcpy(s_config.sta.password, c->password, strlen(c->password));
            s_config.sta.threshold.authmode = c->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
            memset(&s_status, 0, sizeof s_status);
            memcpy(s_status.ssid, c->ssid, sizeof s_status.ssid);
            memset(c, 0, sizeof *c); /* erase event-loop credential copy */
            s_status.state = WIFI_MGR_CONNECTING;
            sntp_mgr_wifi_disconnected();
            if (s_running) stop_station();
            else start_station();
        } else if (id == CONTROL_DISCONNECT) {
            s_desired = false;
            s_attempt = false;
            s_status.state = WIFI_MGR_DISCONNECTED;
            s_status.ip[0] = '\0';
            s_status.rssi_dbm = 0;
            memset(&s_config, 0, sizeof s_config);
            sntp_mgr_wifi_disconnected();
            stop_station();
        } else if (id == CONTROL_RECONNECT && s_desired && !s_stopping) {
            /* Exercise the same disconnect event/backoff path as an AP drop,
             * retaining RAM credentials and the user's reconnect intent. */
            esp_wifi_disconnect();
            schedule_retry();
            sntp_mgr_wifi_disconnected();
        } else if (id == CONTROL_TICK && s_desired && !s_stopping &&
                   s_status.state != WIFI_MGR_CONNECTED) {
            int64_t now = esp_timer_get_time();
            if (s_attempt && now >= s_attempt_until_us) {
                /* Includes association success followed by a stalled DHCP. */
                esp_wifi_disconnect();
                schedule_retry();
                sntp_mgr_wifi_disconnected();
            } else if (!s_attempt && now >= s_retry_at_us) {
                if (s_running) connect_station();
                else start_station();
            }
        }
    } else if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (s_desired && !s_stopping) connect_station();
        } else if (id == WIFI_EVENT_STA_STOP) {
            s_running = s_stopping = s_attempt = false;
            if (s_desired) start_station(); /* latest queued credentials win */
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            sntp_mgr_wifi_disconnected();
            if (!s_stopping) {
                schedule_retry();
                ESP_LOGI(TAG, "disconnected; reconnect %s", s_desired ? "scheduled" : "disabled");
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (s_desired && s_running && !s_stopping) {
            ip_event_got_ip_t *evt = data;
            esp_ip4addr_ntoa(&evt->ip_info.ip, s_status.ip, sizeof s_status.ip);
            s_status.state = WIFI_MGR_CONNECTED;
            s_attempt = false;
            s_backoff_s = 1;
            sntp_mgr_wifi_connected();
            ESP_LOGI(TAG, "got IP: %s", s_status.ip);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        if (s_desired && !s_stopping) {
            esp_wifi_disconnect();
            schedule_retry();
            sntp_mgr_wifi_disconnected();
        }
    }
    xSemaphoreGive(s_lock);
}

static void timer_tick(void *arg)
{
    (void)arg;
    /* A full event queue can skip a tick; the next periodic tick retries. */
    esp_event_post(RLCD_WIFI_CONTROL, CONTROL_TICK, NULL, 0, 0);
}

bool wifi_mgr_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(RLCD_WIFI_CONTROL, ESP_EVENT_ANY_ID, on_event, NULL));
    esp_timer_create_args_t timer = { .callback = timer_tick, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&timer, &s_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer, 1000000));
    return true;
}

bool wifi_mgr_connect(const char *ssid, const char *password, int timeout_s)
{
    (void)timeout_s; /* legacy parameter; connect requests are asynchronous */
    if (!ssid || !*ssid || strlen(ssid) > 32 || (password && strlen(password) > 64)) return false;
    credentials_t c = {0};
    memcpy(c.ssid, ssid, strlen(ssid));
    if (password) memcpy(c.password, password, strlen(password));
    esp_err_t err = esp_event_post(RLCD_WIFI_CONTROL, CONTROL_CONNECT, &c, sizeof c, portMAX_DELAY);
    memset(&c, 0, sizeof c);
    return err == ESP_OK;
}

void wifi_mgr_disconnect(void)
{
    ESP_ERROR_CHECK(esp_event_post(RLCD_WIFI_CONTROL, CONTROL_DISCONNECT, NULL, 0, portMAX_DELAY));
}

void wifi_mgr_reconnect(void)
{
    ESP_ERROR_CHECK(esp_event_post(RLCD_WIFI_CONTROL, CONTROL_RECONNECT, NULL, 0, portMAX_DELAY));
}

wifi_mgr_status_t wifi_mgr_status(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    wifi_mgr_status_t s = s_status;
    xSemaphoreGive(s_lock);
    if (s.state == WIFI_MGR_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s.rssi_dbm = ap.rssi;
    }
    return s;
}
