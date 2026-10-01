#include "sntp_mgr.h"
#include "clock_health.h"
#include "rtc_mgr.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_timer.h"
/* Raw lwIP calls below run exclusively in tcpip_thread (including the DHCP
 * hook and sync callback). Disable IDF's task-context translating inlines. */
#define ESP_LWIP_COMPONENT_BUILD
#include "esp_sntp.h"
#undef ESP_LWIP_COMPONENT_BUILD
#undef SNTP_OPMODE_POLL
#include "lwip/apps/sntp.h"
#include "lwip/tcpip.h"
#include "lwip/timeouts.h"

static const char *TAG = "sntp_mgr";
typedef enum { SOURCE_FALLBACK, SOURCE_DHCP, SOURCE_CONFIG, SOURCE_MANUAL } source_t;
static bool s_started, s_online, s_manual, s_config, s_dhcp_dirty;
static source_t s_source;
static clock_health_t s_health;
static bool s_ntp_synced, s_rtc_seeded;
static uint32_t s_rejected;
static char s_manual_name[64];
static char s_config_name[64];
static char s_name[64] = "pool.ntp.org";
static ip_addr_t s_dhcp[SNTP_MAX_SERVERS];
static unsigned s_dhcp_count;
static int64_t s_retry_at_us;

static void on_sync_time(struct timeval *tv)
{
    clock_health_sync(&s_health, esp_timer_get_time());
    s_ntp_synced = true;
    rtc_mgr_on_sync(tv->tv_sec);
    ESP_LOGI(TAG, "SNTP synchronized (%s)", s_name);
}

/* Overrides lwIP's weak hook so an implausible server time never reaches
 * the system clock (runs in tcpip_thread, like the rest of this module). */
void sntp_sync_time(struct timeval *tv)
{
    if (!clock_wall_plausible(tv->tv_sec, clock_build_epoch())) {
        s_rejected++;
        ESP_LOGW(TAG, "rejected SNTP time %lld from %s: outside the build window",
                 (long long)tv->tv_sec, s_name);
        return;
    }
    settimeofday(tv, NULL);
    sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
    on_sync_time(tv);
}

static void select_source(source_t source)
{
    sntp_stop();
    s_source = source;
    clock_health_select(&s_health, esp_timer_get_time());
    for (unsigned i = 0; i < SNTP_MAX_SERVERS; ++i) sntp_setserver(i, NULL);
    if (source == SOURCE_MANUAL) {
        snprintf(s_name, sizeof s_name, "%s", s_manual_name);
        sntp_setservername(0, s_name);
    } else if (source == SOURCE_CONFIG) {
        snprintf(s_name, sizeof s_name, "%s", s_config_name);
        sntp_setservername(0, s_name);
    } else if (source == SOURCE_DHCP) {
        for (unsigned i = 0; i < s_dhcp_count; ++i) sntp_setserver(i, &s_dhcp[i]);
        ipaddr_ntoa_r(&s_dhcp[0], s_name, sizeof s_name);
    } else {
        snprintf(s_name, sizeof s_name, "pool.ntp.org");
        sntp_setservername(0, s_name);
        /* Stay within the configured server-table size. */
#if SNTP_MAX_SERVERS > 1
        sntp_setservername(1, "time.google.com");
#endif
        s_retry_at_us = esp_timer_get_time() + CLOCK_DHCP_RETRY_US;
    }
    if (s_online) sntp_init();
    ESP_LOGI(TAG, "SNTP source: %s (%s)", source == SOURCE_MANUAL ? "manual" :
             source == SOURCE_CONFIG ? "config" : source == SOURCE_DHCP ? "dhcp" : "fallback", s_name);
}

static void select_preferred(void)
{
    select_source(s_manual ? SOURCE_MANUAL : s_config ? SOURCE_CONFIG :
                  s_dhcp_count ? SOURCE_DHCP : SOURCE_FALLBACK);
}

/* Linker wrapper of lwIP's DHCP option-42 receiver. Cache the lease's list
 * even under a manual override; never let DHCP overwrite the active table.
 * DHCP calls this for every ACK, including num==0 (option absent). Processing
 * is deferred until after ACK/bind, so renewal needs no GOT_IP event. */
void __wrap_dhcp_set_ntp_servers(uint8_t num, const ip4_addr_t *servers)
{
    unsigned count = num < SNTP_MAX_SERVERS ? num : SNTP_MAX_SERVERS;
    bool changed = count != s_dhcp_count;
    for (unsigned i = 0; i < count; ++i) {
        ip_addr_t addr;
        ip_addr_copy_from_ip4(addr, servers[i]);
        if (!ip_addr_cmp(&addr, &s_dhcp[i])) changed = true;
        s_dhcp[i] = addr;
    }
    s_dhcp_count = count;
    s_dhcp_dirty |= changed;
}

static void policy_tick(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (s_online && !s_manual && !s_config) {
        if (s_dhcp_dirty) {
            select_preferred();
        } else if (s_source == SOURCE_DHCP && clock_health_dhcp_failed(&s_health, now)) {
            select_source(SOURCE_FALLBACK);
        } else if (s_source == SOURCE_FALLBACK && s_dhcp_count && now >= s_retry_at_us) {
            select_source(SOURCE_DHCP);
        }
    }
    s_dhcp_dirty = false;
    sys_timeout(1000, policy_tick, NULL);
}

static void start_core(void *arg)
{
    (void)arg;
    if (s_started) return;
    sntp_setoperatingmode(SNTP_OPMODE_POLL);   /* sync results arrive via sntp_sync_time() */
    s_started = true;
    select_preferred();
    sys_timeout(1000, policy_tick, NULL);
}

static void run_core(tcpip_callback_fn fn, void *arg)
{
    /* Wait for completion: stack arguments remain valid and CLI commands are
     * ordered with sync callbacks, DHCP updates and status snapshots. */
    ESP_ERROR_CHECK(tcpip_callback_wait(fn, arg) == ERR_OK ? ESP_OK : ESP_FAIL);
}

void sntp_mgr_start(void) { run_core(start_core, NULL); }

typedef struct { uint32_t age_s; bool accepted; } rtc_seed_arg_t;
static void seed_rtc_core(void *arg)
{
    rtc_seed_arg_t *seed = arg;
    seed->accepted = clock_health_seed_rtc(&s_health, esp_timer_get_time(), seed->age_s);
    if (seed->accepted) s_rtc_seeded = true;
}
bool sntp_mgr_seed_rtc(uint32_t age_s)
{
    rtc_seed_arg_t seed = {.age_s = age_s};
    run_core(seed_rtc_core, &seed);
    return seed.accepted;
}
static void seed_rtc_hold_core(void *arg)
{
    (void)arg;
    clock_health_seed_rtc_hold(&s_health);
    s_rtc_seeded = true;
}
void sntp_mgr_seed_rtc_hold(void) { run_core(seed_rtc_hold_core, NULL); }

static void network_core(void *arg)
{
    bool online = *(bool *)arg;
    if (!s_started) return;
    if (!online) {
        s_online = false;
        sntp_stop();
        s_dhcp_count = 0;  /* never carry an old AP's option 42 to another AP */
        s_dhcp_dirty = false;
        s_health.source_synced = false;
    } else {
        s_online = true;
        s_dhcp_dirty = false;
        select_preferred();
    }
}
void sntp_mgr_wifi_connected(void) { bool up = true; run_core(network_core, &up); }
void sntp_mgr_wifi_disconnected(void) { bool up = false; run_core(network_core, &up); }

static void manual_core(void *arg)
{
    s_manual = true;
    snprintf(s_manual_name, sizeof s_manual_name, "%s", (const char *)arg);
    select_preferred();
}
bool sntp_mgr_set_server(const char *server)
{
    if (!server || !*server || strlen(server) >= sizeof s_manual_name) return false;
    run_core(manual_core, (void *)server);
    return true;
}
static void config_core(void *arg)
{
    const char *server = arg;
    bool enabled = server && *server;
    if (enabled == s_config && (!enabled || !strcmp(server, s_config_name))) return;
    s_config = enabled;
    if (enabled) snprintf(s_config_name, sizeof s_config_name, "%s", server);
    else s_config_name[0] = 0;
    if (!s_manual) select_preferred();
}
void sntp_mgr_set_config_server(const char *server)
{
    run_core(config_core, (void *)server);
}
static void reset_core(void *arg)
{
    (void)arg;
    s_manual = false;
    select_preferred();
}
void sntp_mgr_reset_servers(void) { run_core(reset_core, NULL); }
static void resync_core(void *arg)
{
    (void)arg;
    select_source(s_source); /* preserves last good sync and monotonic age */
}
void sntp_mgr_resync(void) { run_core(resync_core, NULL); }

static void status_core(void *arg)
{
    sntp_mgr_status_t *st = arg;
    memset(st, 0, sizeof *st);
    int64_t now = esp_timer_get_time();
    st->started = s_started;
    st->synced = s_ntp_synced;
    st->rtc_seeded = s_rtc_seeded;

    st->fresh = s_online && clock_health_fresh(&s_health, now);
    st->ntp_age_s = clock_health_age(&s_health, now);
    st->using_manual = s_source == SOURCE_MANUAL;
    st->using_dhcp = s_source == SOURCE_DHCP;
    st->using_config = s_source == SOURCE_CONFIG;
    snprintf(st->server0, sizeof st->server0, "%s", s_name);
    ipaddr_ntoa_r(sntp_getserver(0), st->server0_ip, sizeof st->server0_ip);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    st->unix_sec = tv.tv_sec;
    st->unix_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    st->time_state = clock_health_state(&s_health, now, st->unix_sec, rtc_mgr_hold_ok());
    st->time_trusted = st->time_state == CLOCK_TRUSTED;
    st->time_valid = st->time_state != CLOCK_INVALID;
    st->rejected = s_rejected;
}
sntp_mgr_status_t sntp_mgr_status(void)
{
    sntp_mgr_status_t st;
    run_core(status_core, &st);
    return st;
}
