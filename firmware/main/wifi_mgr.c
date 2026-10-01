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
enum { CONTROL_CONNECT, CONTROL_DISCONNECT, CONTROL_TICK, CONTROL_RECONNECT,
       CONTROL_RESET, CONTROL_KNOWN };
typedef struct { unsigned count; wifi_mgr_network_t networks[WIFI_MGR_KNOWN_MAX]; } known_t;

#define ATTEMPT_US        (30LL * 1000000)  /* association + DHCP budget */
#define ABORT_GRACE_US    (5LL * 1000000)   /* wait for our own disconnect event */
#define SCAN_US           (15LL * 1000000)
#define AUTO_BACKOFF_MIN  10
#define AUTO_BACKOFF_MAX  300
#define SCAN_RECORDS_MAX  32

/* The default event loop owns the driver, scan and reconnect policy. API
 * commands are copied into that loop; its handlers never sleep for backoff.
 * The mutex only protects status snapshots against CLI/display readers. */
static SemaphoreHandle_t s_lock;
static wifi_mgr_status_t s_status;
static wifi_config_t s_config;
static wifi_mgr_mode_t s_mode = WIFI_MGR_MODE_AUTO;
static known_t s_known;
static bool s_desired, s_running, s_stopping, s_attempt, s_aborting, s_scanning;
static unsigned s_backoff_s = 1;
static int64_t s_retry_at_us, s_attempt_until_us, s_scan_until_us;
static esp_timer_handle_t s_timer;
/* AUTO: known-network indexes visible in the last scan, strongest first. */
static uint8_t s_candidates[WIFI_MGR_KNOWN_MAX];
static unsigned s_candidate_count, s_candidate_next;
static int s_current = -1;   /* known index being tried or joined */
static wifi_ap_record_t s_records[SCAN_RECORDS_MAX];

static void link_down(void)
{
    s_attempt = s_aborting = false;
    s_status.state = s_desired ? WIFI_MGR_CONNECTING : WIFI_MGR_DISCONNECTED;
    s_status.ip[0] = '\0';
    s_status.rssi_dbm = 0;
}

/* MANUAL: 1 s doubling to 30 s. AUTO: 10 s doubling to 5 min between scans. */
static void schedule_retry(void)
{
    link_down();
    s_retry_at_us = esp_timer_get_time() + (int64_t)s_backoff_s * 1000000;
    unsigned cap = s_mode == WIFI_MGR_MODE_AUTO ? AUTO_BACKOFF_MAX : 30;
    if (s_backoff_s < cap) s_backoff_s = s_backoff_s * 2 > cap ? cap : s_backoff_s * 2;
}

static void schedule_scan_now(void)
{
    link_down();
    s_backoff_s = AUTO_BACKOFF_MIN;
    s_retry_at_us = esp_timer_get_time();
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
        s_aborting = false;
        s_attempt_until_us = esp_timer_get_time() + ATTEMPT_US;
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

static void set_station_config(const char *ssid, const char *password)
{
    memset(&s_config, 0, sizeof s_config);
    memcpy(s_config.sta.ssid, ssid, strlen(ssid));
    memcpy(s_config.sta.password, password, strlen(password));
    s_config.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    /* Among APs sharing an SSID (mesh), join the strongest. */
    s_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    s_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
}

/* ---- AUTO mode ---- */

static void start_scan(void)
{
    s_current = -1;
    s_status.ssid[0] = '\0';
    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err == ESP_OK) {
        s_scanning = true;
        s_scan_until_us = esp_timer_get_time() + SCAN_US;
        s_status.state = WIFI_MGR_CONNECTING;
    } else {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        schedule_retry();
    }
}

static void try_next_candidate(void)
{
    if (s_candidate_next >= s_candidate_count) {
        ESP_LOGI(TAG, "no known network joined; rescan in %u s", s_backoff_s);
        s_current = -1;
        schedule_retry();
        return;
    }
    s_current = s_candidates[s_candidate_next++];
    const wifi_mgr_network_t *n = &s_known.networks[s_current];
    set_station_config(n->ssid, n->password);
    memcpy(s_status.ssid, n->ssid, sizeof s_status.ssid);
    s_status.state = WIFI_MGR_CONNECTING;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &s_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set config failed: %s", esp_err_to_name(err));
        try_next_candidate();
        return;
    }
    ESP_LOGI(TAG, "joining known network \"%s\"", n->ssid);
    connect_station();
}

static void scan_done(void)
{
    s_scanning = false;
    uint16_t count = SCAN_RECORDS_MAX;
    if (esp_wifi_scan_get_ap_records(&count, s_records) != ESP_OK) count = 0;
    if (s_mode != WIFI_MGR_MODE_AUTO || !s_desired || s_stopping) return;
    int best[WIFI_MGR_KNOWN_MAX];
    for (unsigned k = 0; k < s_known.count; ++k) best[k] = -1000;
    for (unsigned i = 0; i < count; ++i)
        for (unsigned k = 0; k < s_known.count; ++k)
            if (!strcmp((const char *)s_records[i].ssid, s_known.networks[k].ssid) &&
                s_records[i].rssi > best[k]) best[k] = s_records[i].rssi;
    /* Strongest first; a stable insertion sort keeps file order on ties. */
    s_candidate_count = s_candidate_next = 0;
    for (unsigned k = 0; k < s_known.count; ++k) {
        if (best[k] == -1000) continue;
        unsigned j = s_candidate_count++;
        while (j && best[s_candidates[j - 1]] < best[k]) { s_candidates[j] = s_candidates[j - 1]; --j; }
        s_candidates[j] = (uint8_t)k;
    }
    if (!s_candidate_count) ESP_LOGI(TAG, "scan found %u APs, none known", count);
    try_next_candidate();
}

/* (Re)start AUTO from a clean station so manual or stale state cannot leak. */
static void begin_auto(void)
{
    s_desired = s_known.count > 0;
    s_backoff_s = AUTO_BACKOFF_MIN;
    s_attempt = s_aborting = s_scanning = false;
    s_current = -1;
    s_candidate_count = s_candidate_next = 0;
    memset(&s_config, 0, sizeof s_config);
    memset(&s_status, 0, sizeof s_status);
    s_status.state = s_desired ? WIFI_MGR_CONNECTING : WIFI_MGR_DISCONNECTED;
    sntp_mgr_wifi_disconnected();
    if (s_running) stop_station();          /* STA_STOP restarts if desired */
    else if (s_desired) start_station();    /* STA_START begins the scan */
}

static bool same_known(const known_t *a, const known_t *b)
{
    if (a->count != b->count) return false;
    for (unsigned i = 0; i < a->count; ++i)
        if (strcmp(a->networks[i].ssid, b->networks[i].ssid) ||
            strcmp(a->networks[i].password, b->networks[i].password)) return false;
    return true;
}

static void apply_known(known_t *k)
{
    /* Storage remounts reload an identical list; leave scans and links alone. */
    if (same_known(k, &s_known)) { memset(k, 0, sizeof *k); return; }
    bool keep = false;
    if (s_mode == WIFI_MGR_MODE_AUTO && s_status.state == WIFI_MGR_CONNECTED && s_current >= 0) {
        const wifi_mgr_network_t *joined = &s_known.networks[s_current];
        for (unsigned i = 0; i < k->count && !keep; ++i) {
            if (!strcmp(k->networks[i].ssid, joined->ssid) &&
                !strcmp(k->networks[i].password, joined->password)) {
                keep = true;
                s_current = (int)i;
            }
        }
    }
    s_known = *k;
    memset(k, 0, sizeof *k); /* erase event-loop credential copy */
    if (s_mode != WIFI_MGR_MODE_AUTO) return;
    if (keep) { ESP_LOGI(TAG, "known networks updated; keeping \"%s\"", s_status.ssid); return; }
    begin_auto();
}

static void on_control(int32_t id, void *data)
{
    if (id == CONTROL_CONNECT) {
        wifi_mgr_network_t *c = data;
        s_mode = WIFI_MGR_MODE_MANUAL;
        s_desired = true;
        s_backoff_s = 1;
        s_attempt = s_aborting = s_scanning = false;
        s_current = -1;
        set_station_config(c->ssid, c->password);
        memset(&s_status, 0, sizeof s_status);
        memcpy(s_status.ssid, c->ssid, sizeof s_status.ssid);
        memset(c, 0, sizeof *c); /* erase event-loop credential copy */
        s_status.state = WIFI_MGR_CONNECTING;
        sntp_mgr_wifi_disconnected();
        if (s_running) stop_station();
        else start_station();
    } else if (id == CONTROL_DISCONNECT) {
        s_mode = WIFI_MGR_MODE_OFF;
        s_desired = false;
        s_attempt = s_aborting = s_scanning = false;
        s_current = -1;
        s_status.state = WIFI_MGR_DISCONNECTED;
        s_status.ip[0] = '\0';
        s_status.rssi_dbm = 0;
        memset(&s_config, 0, sizeof s_config);
        sntp_mgr_wifi_disconnected();
        stop_station();
    } else if (id == CONTROL_RESET) {
        s_mode = WIFI_MGR_MODE_AUTO;
        begin_auto();
    } else if (id == CONTROL_KNOWN) {
        apply_known(data);
    } else if (id == CONTROL_RECONNECT && s_desired && !s_stopping) {
        /* Exercise the same disconnect event/backoff path as an AP drop,
         * retaining credentials and the user's reconnect intent. */
        if (s_mode == WIFI_MGR_MODE_AUTO) {
            if (s_status.state == WIFI_MGR_CONNECTED) esp_wifi_disconnect(); /* event rescans */
        } else {
            esp_wifi_disconnect();
            schedule_retry();
        }
        sntp_mgr_wifi_disconnected();
    } else if (id == CONTROL_TICK && s_desired && !s_stopping &&
               s_status.state != WIFI_MGR_CONNECTED) {
        int64_t now = esp_timer_get_time();
        if (s_scanning) {
            if (now >= s_scan_until_us) {
                esp_wifi_scan_stop();
                s_scanning = false;
                schedule_retry();
            }
        } else if (s_attempt && now >= s_attempt_until_us) {
            /* Includes association success followed by a stalled DHCP. */
            if (s_mode == WIFI_MGR_MODE_AUTO && !s_aborting) {
                /* Let our own disconnect event advance to the next candidate;
                 * if it never arrives, advance after a short grace period. */
                s_aborting = true;
                s_attempt_until_us = now + ABORT_GRACE_US;
                esp_wifi_disconnect();
            } else if (s_mode == WIFI_MGR_MODE_AUTO) {
                s_attempt = s_aborting = false;
                try_next_candidate();
            } else {
                esp_wifi_disconnect();
                schedule_retry();
            }
            sntp_mgr_wifi_disconnected();
        } else if (!s_attempt && now >= s_retry_at_us) {
            if (!s_running) start_station();
            else if (s_mode == WIFI_MGR_MODE_AUTO) start_scan();
            else connect_station();
        }
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (base == RLCD_WIFI_CONTROL) {
        on_control(id, data);
    } else if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (s_desired && !s_stopping) {
                if (s_mode == WIFI_MGR_MODE_AUTO) start_scan();
                else connect_station();
            }
        } else if (id == WIFI_EVENT_STA_STOP) {
            s_running = s_stopping = s_attempt = s_aborting = s_scanning = false;
            if (s_desired) start_station(); /* latest queued request wins */
        } else if (id == WIFI_EVENT_SCAN_DONE) {
            if (s_scanning) scan_done();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            sntp_mgr_wifi_disconnected();
            if (s_stopping) {
                /* stop in progress; STA_STOP decides what happens next */
            } else if (s_mode != WIFI_MGR_MODE_AUTO) {
                schedule_retry();
                ESP_LOGI(TAG, "disconnected; reconnect %s", s_desired ? "scheduled" : "disabled");
            } else if (s_attempt) {
                const wifi_event_sta_disconnected_t *d = data;
                int reason = d ? d->reason : -1;
                ESP_LOGI(TAG, "\"%s\" failed (reason %d)", s_status.ssid, reason);
                (void)reason;
                s_attempt = s_aborting = false;
                try_next_candidate();               /* this candidate failed */
            } else if (s_status.state == WIFI_MGR_CONNECTED) {
                ESP_LOGI(TAG, "disconnected from \"%s\"; rescanning", s_status.ssid);
                s_current = -1;
                schedule_scan_now();
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (s_desired && s_running && !s_stopping) {
            ip_event_got_ip_t *evt = data;
            esp_ip4addr_ntoa(&evt->ip_info.ip, s_status.ip, sizeof s_status.ip);
            s_status.state = WIFI_MGR_CONNECTED;
            s_attempt = s_aborting = false;
            s_backoff_s = s_mode == WIFI_MGR_MODE_AUTO ? AUTO_BACKOFF_MIN : 1;
            sntp_mgr_wifi_connected();
            ESP_LOGI(TAG, "got IP: %s", s_status.ip);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        if (s_desired && !s_stopping) {
            esp_wifi_disconnect();
            if (s_mode != WIFI_MGR_MODE_AUTO) schedule_retry();
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

static bool network_valid(const char *ssid, const char *password)
{
    if (!ssid || !*ssid || strlen(ssid) > 32) return false;
    size_t n = password ? strlen(password) : 0;
    return n <= 64;
}

bool wifi_mgr_connect(const char *ssid, const char *password, int timeout_s)
{
    (void)timeout_s; /* legacy parameter; connect requests are asynchronous */
    if (!network_valid(ssid, password)) return false;
    wifi_mgr_network_t c = {0};
    memcpy(c.ssid, ssid, strlen(ssid));
    if (password) memcpy(c.password, password, strlen(password));
    esp_err_t err = esp_event_post(RLCD_WIFI_CONTROL, CONTROL_CONNECT, &c, sizeof c, portMAX_DELAY);
    memset(&c, 0, sizeof c);
    return err == ESP_OK;
}

bool wifi_mgr_set_known(const wifi_mgr_network_t *networks, unsigned count)
{
    if (count > WIFI_MGR_KNOWN_MAX || (count && !networks)) return false;
    known_t k = { .count = count };
    for (unsigned i = 0; i < count; ++i) {
        if (!network_valid(networks[i].ssid, networks[i].password)) return false;
        k.networks[i] = networks[i];
    }
    esp_err_t err = esp_event_post(RLCD_WIFI_CONTROL, CONTROL_KNOWN, &k, sizeof k, portMAX_DELAY);
    memset(&k, 0, sizeof k);
    return err == ESP_OK;
}

void wifi_mgr_disconnect(void)
{
    ESP_ERROR_CHECK(esp_event_post(RLCD_WIFI_CONTROL, CONTROL_DISCONNECT, NULL, 0, portMAX_DELAY));
}

void wifi_mgr_reset(void)
{
    ESP_ERROR_CHECK(esp_event_post(RLCD_WIFI_CONTROL, CONTROL_RESET, NULL, 0, portMAX_DELAY));
}

void wifi_mgr_reconnect(void)
{
    ESP_ERROR_CHECK(esp_event_post(RLCD_WIFI_CONTROL, CONTROL_RECONNECT, NULL, 0, portMAX_DELAY));
}

wifi_mgr_status_t wifi_mgr_status(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    wifi_mgr_status_t s = s_status;
    s.mode = s_mode;
    s.known = s_known.count;
    xSemaphoreGive(s_lock);
    if (s.state == WIFI_MGR_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s.rssi_dbm = ap.rssi;
    }
    return s;
}
