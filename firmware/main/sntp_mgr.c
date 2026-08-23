#include "sntp_mgr.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "lwip/ip_addr.h"

static const char *TAG = "sntp_mgr";

static bool s_started;
static bool s_synced;
static bool s_manual;        /* manual override in effect */
static bool s_dhcp_active;   /* last applied servers came from DHCP */
static int64_t s_synced_at_s; /* unix time of last successful sync */

static char s_server0[64] = "pool.ntp.org";

static void on_sync_time(struct timeval *tv)
{
    s_synced = true;
    s_synced_at_s = (int64_t)tv->tv_sec;
    ESP_LOGI(TAG, "SNTP time synchronized");
}

/* Apply the built-in fallback servers (used when neither manual nor DHCP). */
static void apply_fallback_servers(void)
{
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    strncpy(s_server0, "pool.ntp.org", sizeof s_server0 - 1);
    s_dhcp_active = false;
    ESP_LOGI(TAG, "SNTP servers: fallback pool.ntp.org");
}

static void apply_servers(void)
{
    if (s_manual) {
        /* Manual override wins. DHCP list is bypassed (sntp_servermode_dhcp
         * disabled below so it cannot overwrite the manual entry). */
        sntp_servermode_dhcp(0);
        esp_sntp_setservername(0, s_server0);
        ESP_LOGI(TAG, "SNTP servers: manual %s", s_server0);
        s_dhcp_active = false;
        return;
    }
    /* DHCP mode: lwIP writes option-42 server IPs straight into the sntp
     * server table when the lease arrives. Set the fallback names here so
     * they exist until/unless DHCP overrides them, but NEVER re-apply them
     * after the lease (that would clobber the DHCP-provided addresses). */
    sntp_servermode_dhcp(1);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    strncpy(s_server0, "pool.ntp.org", sizeof s_server0 - 1);
    s_dhcp_active = true;
    ESP_LOGI(TAG, "SNTP servers: DHCP option 42 (fallback pool.ntp.org)");
}

static void sntp_reconfigure(void)
{
    if (!s_started) return;
    /* Restart SNTP so it picks up whatever is in the server table now. For
     * DHCP mode this must happen WITHOUT re-applying fallback names, which
     * would overwrite the addresses lwIP stored from option 42. */
    esp_sntp_stop();
    if (s_manual) {
        sntp_servermode_dhcp(0);
        esp_sntp_setservername(0, s_server0);
    } else {
        sntp_servermode_dhcp(1);
    }
    esp_sntp_init();
}

void sntp_mgr_start(void)
{
    if (s_started) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_set_time_sync_notification_cb(on_sync_time);
    apply_servers();
    esp_sntp_init();
    s_started = true;
    ESP_LOGI(TAG, "SNTP started");
}

static void sntp_dhcp_check_task(void *arg)
{
    (void)arg;
    /* Give the DHCP lease a moment to be fully processed (the NTP option 42
     * servers are written by lwIP after the IP event). */
    vTaskDelay(pdMS_TO_TICKS(3000));
    for (int i = 0; i < SNTP_MAX_SERVERS; i++) {
        const ip_addr_t *addr = esp_sntp_getserver(i);
        if (addr && !ip_addr_isany(addr)) {
            char ipbuf[16];
            ip4addr_ntoa_r(ip_2_ip4(addr), ipbuf, sizeof ipbuf);
            ESP_LOGI(TAG, "DHCP check: sntp server[%d] = %s", i, ipbuf);
        }
    }
    /* If a DHCP-provided server never syncs, fall back to the public pool so
     * a misconfigured option 42 (e.g. pointing at a host without an NTP
     * daemon) cannot leave us without time. */
    vTaskDelay(pdMS_TO_TICKS(15000));
    if (!s_manual && esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "DHCP NTP server not syncing; falling back to pool.ntp.org");
        sntp_servermode_dhcp(0);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        strncpy(s_server0, "pool.ntp.org", sizeof s_server0 - 1);
        s_dhcp_active = false;
        esp_sntp_stop();
        esp_sntp_init();
    }
    vTaskDelete(NULL);
}

void sntp_mgr_wifi_connected(void)
{
    /* Re-apply server selection now that the network (and DHCP info) exists.
     * Manual override survives; otherwise DHCP servers take over. */
    sntp_reconfigure();
    xTaskCreate(sntp_dhcp_check_task, "sntp_dhcp", 3072, NULL, 5, NULL);
}

bool sntp_mgr_set_server(const char *server)
{
    if (!server || !*server || strlen(server) >= sizeof s_server0) return false;
    strncpy(s_server0, server, sizeof s_server0 - 1);
    s_manual = true;
    sntp_reconfigure();
    ESP_LOGI(TAG, "SNTP manual server: %s", server);
    return true;
}

void sntp_mgr_reset_servers(void)
{
    s_manual = false;
    sntp_reconfigure();
}

void sntp_mgr_resync(void)
{
    if (!s_started) return;
    s_synced = false;
    esp_sntp_stop();
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP resync requested");
}

/* Update s_server0_ip to reflect the actually-configured server (DHCP writes
 * IPs directly via dhcp_set_ntp_servers). */
static void refresh_actual_server(char *out_ip, size_t out_sz)
{
    const ip_addr_t *addr = esp_sntp_getserver(0);
    if (addr && !ip_addr_isany(addr)) {
        ip4addr_ntoa_r(ip_2_ip4(addr), out_ip, out_sz);
    } else {
        strncpy(out_ip, "0.0.0.0", out_sz - 1);
    }
}

sntp_mgr_status_t sntp_mgr_status(void)
{
    sntp_mgr_status_t st;
    memset(&st, 0, sizeof st);
    st.started = s_started;
    /* The notification callback can be reset by esp_sntp_stop(); query the
     * lwIP sync status directly as the source of truth. */
    st.synced = (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) || s_synced;
    st.using_manual = s_manual;
    st.using_dhcp = s_dhcp_active;
    strncpy(st.server0, s_server0, sizeof st.server0 - 1);
    refresh_actual_server(st.server0_ip, sizeof st.server0_ip);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    st.unix_sec = (int64_t)tv.tv_sec;
    st.ntp_age_s = s_synced && s_synced_at_s > 0
                       ? (uint32_t)(st.unix_sec - s_synced_at_s)
                       : 0;
    return st;
}
