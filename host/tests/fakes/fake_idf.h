#ifndef FAKE_IDF_H
#define FAKE_IDF_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <stdio.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ERR_OK 0
#define ESP_ERROR_CHECK(expr) do { if ((expr) != 0) abort(); } while (0)
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
typedef int esp_err_t;
typedef int err_t;
const char *esp_err_to_name(int err);
int64_t esp_timer_get_time(void);
typedef struct { uint32_t addr; } ip_addr_t;
typedef ip_addr_t ip4_addr_t;
#define ip_addr_copy_from_ip4(dst, src) ((dst) = (src))
#define ip_addr_cmp(a, b) ((a)->addr == (b)->addr)
char *ipaddr_ntoa_r(const ip_addr_t *, char *, int);
#define SNTP_MAX_SERVERS 1
#define SNTP_OPMODE_POLL 0
void sntp_setoperatingmode(uint8_t);
void sntp_set_time_sync_notification_cb(void (*)(struct timeval *));
void sntp_stop(void);
void sntp_init(void);
void sntp_setserver(uint8_t, const ip_addr_t *);
void sntp_setservername(uint8_t, const char *);
const ip_addr_t *sntp_getserver(uint8_t);
typedef void (*tcpip_callback_fn)(void *);
err_t tcpip_callback_wait(tcpip_callback_fn, void *);
void sys_timeout(uint32_t, void (*)(void *), void *);

typedef const char *esp_event_base_t;
#define ESP_EVENT_DEFINE_BASE(name) const char *name = #name
extern const char *WIFI_EVENT, *IP_EVENT;
#define ESP_EVENT_ANY_ID -1
enum { WIFI_EVENT_STA_START, WIFI_EVENT_STA_STOP, WIFI_EVENT_STA_DISCONNECTED, WIFI_EVENT_SCAN_DONE };
enum { IP_EVENT_STA_GOT_IP, IP_EVENT_STA_LOST_IP };
typedef struct { struct { ip4_addr_t ip; } ip_info; } ip_event_got_ip_t;
void esp_ip4addr_ntoa(const ip4_addr_t *, char *, int);
int esp_event_handler_register(esp_event_base_t, int32_t, void (*)(void *, esp_event_base_t, int32_t, void *), void *);
int esp_event_post(esp_event_base_t, int32_t, const void *, size_t, int);
int esp_event_loop_create_default(void);
int esp_netif_init(void);
void *esp_netif_create_default_wifi_sta(void);
typedef struct { struct { uint8_t ssid[32], password[64]; struct { int authmode; } threshold; int scan_method, sort_method; } sta; } wifi_config_t;
typedef struct { int unused; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
#define WIFI_IF_STA 0
#define WIFI_MODE_STA 0
#define WIFI_STORAGE_RAM 0
#define WIFI_AUTH_WPA2_PSK 2
#define WIFI_AUTH_OPEN 0
#define WIFI_ALL_CHANNEL_SCAN 1
#define WIFI_CONNECT_AP_BY_SIGNAL 0
typedef struct { uint8_t ssid[33]; int8_t rssi; } wifi_ap_record_t;
typedef struct { uint8_t reason; } wifi_event_sta_disconnected_t;
int esp_wifi_init(const wifi_init_config_t *);
int esp_wifi_set_storage(int);
int esp_wifi_set_mode(int);
int esp_wifi_set_config(int, const wifi_config_t *);
int esp_wifi_start(void);
int esp_wifi_stop(void);
int esp_wifi_connect(void);
int esp_wifi_disconnect(void);
int esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
int esp_wifi_scan_start(const void *, bool);
int esp_wifi_scan_stop(void);
int esp_wifi_scan_get_ap_records(uint16_t *, wifi_ap_record_t *);
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
typedef void *esp_timer_handle_t;
int esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *);
int esp_timer_start_periodic(esp_timer_handle_t, uint64_t);
typedef int *SemaphoreHandle_t;
#define portMAX_DELAY -1
SemaphoreHandle_t xSemaphoreCreateMutex(void);
void xSemaphoreTake(SemaphoreHandle_t, int);
void xSemaphoreGive(SemaphoreHandle_t);

void fake_advance(int64_t us);
void fake_sync(void);
void fake_dhcp(uint32_t ip);
void fake_event(esp_event_base_t, int32_t, void *);
void fake_drain(void);
void fake_wifi_tick(void);
extern const char *fake_server_name;
extern unsigned fake_connect_calls, fake_start_calls, fake_scan_calls;
/* Next scan result: visible SSIDs and RSSI; the scan completes on drain. */
void fake_scan_results(unsigned n, const char *const *ssids, const int8_t *rssi);
extern wifi_config_t fake_wifi_config; /* last esp_wifi_set_config */
extern bool fake_sntp_running;
#endif
