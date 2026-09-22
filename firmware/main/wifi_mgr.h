#ifndef RLCD_FW_WIFI_MGR_H
#define RLCD_FW_WIFI_MGR_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Minimal Wi-Fi STA manager. Non-persistent by design: nothing is written to
 * NVS (ESP_WIFI_STORAGE_RAM); the caller provides SSID/password per connect.
 */

typedef enum {
    WIFI_MGR_DISCONNECTED = 0,
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED,
} wifi_mgr_state_t;

typedef struct {
    wifi_mgr_state_t state;
    char ssid[33];          /* last requested SSID */
    char ip[16];            /* "0.0.0.0" when not connected */
    int8_t rssi_dbm;        /* 0 when unknown */
} wifi_mgr_status_t;

/* Initialize Wi-Fi STA (event loop + netif + esp_wifi). Call once at boot. */
bool wifi_mgr_start(void);

/* Queue a connect request. Returns true when accepted, not when connected.
 * Retries with capped backoff until disconnect or a replacement request.
 * Credentials remain only in RAM. timeout_s is a legacy, ignored argument. */
bool wifi_mgr_connect(const char *ssid, const char *password, int timeout_s);

/* Disconnect and stop Wi-Fi. */
void wifi_mgr_disconnect(void);

/* Force a link drop and let the normal retry policy reconnect using RAM credentials. */
void wifi_mgr_reconnect(void);

/* Snapshot of current status. */
wifi_mgr_status_t wifi_mgr_status(void);

#endif
