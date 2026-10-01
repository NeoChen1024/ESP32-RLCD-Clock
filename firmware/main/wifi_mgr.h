#ifndef RLCD_FW_WIFI_MGR_H
#define RLCD_FW_WIFI_MGR_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Wi-Fi STA manager. Nothing is written to NVS (ESP_WIFI_STORAGE_RAM).
 *
 * Modes:
 *   AUTO    (boot default) scan whenever there is no active connection and
 *           join the strongest visible known network, trying the next
 *           candidate on failure. Known networks come from secrets/wifi.json.
 *   MANUAL  `wifi connect`: one network, retried with capped backoff; known
 *           networks are ignored until `wifi reset`.
 *   OFF     `wifi disconnect`: no automatic connection until `wifi reset`.
 */

#define WIFI_MGR_KNOWN_MAX 8

typedef enum {
    WIFI_MGR_DISCONNECTED = 0,
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED,
} wifi_mgr_state_t;

typedef enum {
    WIFI_MGR_MODE_AUTO = 0,
    WIFI_MGR_MODE_MANUAL,
    WIFI_MGR_MODE_OFF,
} wifi_mgr_mode_t;

typedef struct {
    char ssid[33];
    char password[65];      /* empty: open network */
} wifi_mgr_network_t;

typedef struct {
    wifi_mgr_state_t state;
    wifi_mgr_mode_t mode;
    unsigned known;         /* known networks available to AUTO */
    char ssid[33];          /* requested or joined SSID */
    char ip[16];            /* "0.0.0.0" when not connected */
    int8_t rssi_dbm;        /* 0 when unknown */
} wifi_mgr_status_t;

/* Initialize Wi-Fi STA (event loop + netif + esp_wifi). Call once at boot. */
bool wifi_mgr_start(void);

/* MANUAL mode. Returns true when accepted, not when connected. Retries with
 * capped backoff until disconnect, reset or a replacement request.
 * timeout_s is a legacy, ignored argument. */
bool wifi_mgr_connect(const char *ssid, const char *password, int timeout_s);

/* OFF mode: disconnect and stop Wi-Fi. */
void wifi_mgr_disconnect(void);

/* Return to AUTO mode and scan for known networks. */
void wifi_mgr_reset(void);

/* Replace the known-network list (count 0 clears it). In AUTO mode an
 * established connection survives only if its SSID and password are still
 * listed; otherwise the manager rescans. Other modes just store the list. */
bool wifi_mgr_set_known(const wifi_mgr_network_t *networks, unsigned count);

/* Force a link drop and let the normal retry/scan policy reconnect. */
void wifi_mgr_reconnect(void);

/* Snapshot of current status. */
wifi_mgr_status_t wifi_mgr_status(void);

#endif
