#include "fake_idf.h"
#include <assert.h>
#include <string.h>
static int64_t now_us;
static ip_addr_t server;
const char *fake_server_name;
bool fake_sntp_running;
unsigned fake_connect_calls, fake_start_calls;
static void (*sync_cb)(struct timeval *);
static void (*policy_cb)(void *);
static void (*timer_cb)(void *);
static bool in_core;
const char *WIFI_EVENT = "wifi", *IP_EVENT = "ip";
static struct { esp_event_base_t base; void (*fn)(void *, esp_event_base_t, int32_t, void *); } handlers[3];
static unsigned handler_count;
static struct { esp_event_base_t base; int32_t id; unsigned char data[128]; } events[64];
static unsigned head, tail;

const char *esp_err_to_name(int err) { (void)err; return "fake"; }
int64_t esp_timer_get_time(void) { return now_us; }
char *ipaddr_ntoa_r(const ip_addr_t *ip, char *out, int n) {
    snprintf(out, n, "%u.%u.%u.%u", ip->addr >> 24, (ip->addr >> 16) & 255,
             (ip->addr >> 8) & 255, ip->addr & 255); return out;
}
void sntp_setoperatingmode(uint8_t mode) { assert(in_core); (void)mode; }
void sntp_set_time_sync_notification_cb(void (*cb)(struct timeval *)) { assert(in_core); sync_cb = cb; }
void sntp_stop(void) { assert(in_core); fake_sntp_running = false; }
void sntp_init(void) { assert(in_core); fake_sntp_running = true; }
void sntp_setserver(uint8_t idx, const ip_addr_t *ip) {
    assert(in_core && idx == 0); server = ip ? *ip : (ip_addr_t){0}; fake_server_name = NULL;
}
void sntp_setservername(uint8_t idx, const char *name) { assert(in_core && idx == 0); fake_server_name = name; }
const ip_addr_t *sntp_getserver(uint8_t idx) { assert(in_core && idx == 0); return &server; }
err_t tcpip_callback_wait(tcpip_callback_fn cb, void *arg) {
    assert(!in_core); in_core = true; cb(arg); in_core = false; return ERR_OK;
}
void sys_timeout(uint32_t ms, void (*cb)(void *), void *arg) { assert(in_core); (void)ms; (void)arg; policy_cb = cb; }
void fake_advance(int64_t us) { now_us += us; in_core = true; policy_cb(NULL); in_core = false; }
void fake_sync(void) { assert(fake_sntp_running); struct timeval tv = {0}; in_core = true; sync_cb(&tv); in_core = false; }
extern void __wrap_dhcp_set_ntp_servers(uint8_t, const ip4_addr_t *);
void fake_dhcp(uint32_t ip) { ip4_addr_t addr = {ip}; in_core = true; __wrap_dhcp_set_ntp_servers(ip ? 1 : 0, &addr); in_core = false; }
void esp_ip4addr_ntoa(const ip4_addr_t *ip, char *out, int n) { ipaddr_ntoa_r(ip, out, n); }
int esp_event_handler_register(esp_event_base_t base, int32_t id, void (*fn)(void *, esp_event_base_t, int32_t, void *), void *arg) {
    (void)id; (void)arg; assert(handler_count < 3); handlers[handler_count].base = base; handlers[handler_count++].fn = fn; return 0;
}
int esp_event_post(esp_event_base_t base, int32_t id, const void *data, size_t n, int wait) {
    (void)wait; assert(tail - head < 64 && n <= 128); unsigned i = tail++ % 64;
    events[i].base = base; events[i].id = id; if (n) memcpy(events[i].data, data, n); return 0;
}
void fake_event(esp_event_base_t base, int32_t id, void *data) {
    for (unsigned i = 0; i < handler_count; ++i) if (handlers[i].base == base) handlers[i].fn(NULL, base, id, data);
}
void fake_drain(void) {
    while (head < tail) { unsigned i = head++ % 64; fake_event(events[i].base, events[i].id, events[i].data); }
}
void fake_wifi_tick(void) { timer_cb(NULL); fake_drain(); }
int esp_event_loop_create_default(void) { return 0; }
int esp_netif_init(void) { return 0; }
void *esp_netif_create_default_wifi_sta(void) { return (void *)1; }
int esp_wifi_init(const wifi_init_config_t *x) { (void)x; return 0; }
int esp_wifi_set_storage(int x) { (void)x; return 0; }
int esp_wifi_set_mode(int x) { (void)x; return 0; }
int esp_wifi_set_config(int x, const wifi_config_t *c) { (void)x; (void)c; return 0; }
int esp_wifi_start(void) { fake_start_calls++; esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_START, NULL, 0, 0); return 0; }
int esp_wifi_stop(void) { esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL, 0, 0); esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL, 0, 0); return 0; }
int esp_wifi_connect(void) { fake_connect_calls++; return 0; }
int esp_wifi_disconnect(void) { esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL, 0, 0); return 0; }
int esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) { ap->rssi = -42; return 0; }
int esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *out) { timer_cb = a->callback; *out = (void *)1; return 0; }
int esp_timer_start_periodic(esp_timer_handle_t h, uint64_t period) { (void)h; (void)period; return 0; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { static int lock; return &lock; }
void xSemaphoreTake(SemaphoreHandle_t p, int wait) { (void)wait; assert(!*p); *p = 1; }
void xSemaphoreGive(SemaphoreHandle_t p) { assert(*p); *p = 0; }
