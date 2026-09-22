#include "fake_idf.h"
#include "sntp_mgr.h"
#include "wifi_mgr.h"
#include "clock_health.h"
#include <assert.h>
#include <string.h>

static void got_ip(void)
{
    ip_event_got_ip_t evt = { .ip_info.ip.addr = 0xc0000202 };
    fake_event(IP_EVENT, IP_EVENT_STA_GOT_IP, &evt);
}
int main(void)
{
    assert(wifi_mgr_start());
    sntp_mgr_start();
    assert(!fake_sntp_running);
    assert(!sntp_mgr_status().time_trusted);
    assert(wifi_mgr_connect("test-ap", "test-only", 0));
    fake_drain();
    assert(fake_connect_calls == 1);
    fake_dhcp(0xc0000201);
    got_ip();
    assert(sntp_mgr_status().using_dhcp);
    fake_sync();
    /* Display/HTTP/CLI reads must never consume a successful sync. */
    for (int i = 0; i < 30; ++i) {
        fake_advance(1000000);
        assert(sntp_mgr_status().fresh);
        assert(sntp_mgr_status().using_dhcp);
    }
    assert(sntp_mgr_status().ntp_age_s == 30);
    sntp_mgr_resync();
    assert(sntp_mgr_status().time_trusted && !sntp_mgr_status().fresh);
    fake_sync();

    assert(sntp_mgr_set_server("manual.invalid"));
    assert(!strcmp(fake_server_name, "manual.invalid"));
    fake_dhcp(0xc0000203); /* renewal under manual cannot clobber it */
    fake_advance(1000000);
    assert(sntp_mgr_status().using_manual);
    assert(!strcmp(fake_server_name, "manual.invalid"));
    sntp_mgr_reset_servers();
    assert(sntp_mgr_status().using_dhcp);
    assert(!strcmp(sntp_mgr_status().server0_ip, "192.0.2.3"));
    assert(!sntp_mgr_status().fresh); /* old source sync cannot pass new trial */
    fake_advance(CLOCK_DHCP_TRIAL_US);
    assert(!sntp_mgr_status().using_dhcp);
    assert(!strcmp(fake_server_name, "pool.ntp.org"));
    fake_sync();
    fake_advance(CLOCK_DHCP_RETRY_US);
    assert(sntp_mgr_status().using_dhcp); /* periodic recovery probe */
    fake_sync();
    fake_advance(CLOCK_DHCP_TRIAL_US);
    assert(sntp_mgr_status().using_dhcp);

    /* DHCP renewal removes option 42: discard old lease immediately. */
    fake_dhcp(0);
    fake_advance(1000000);
    assert(!sntp_mgr_status().using_dhcp);
    assert(sntp_mgr_set_server("another.invalid"));
    sntp_mgr_reset_servers();
    assert(!strcmp(fake_server_name, "pool.ntp.org"));
    fake_sync();
    fake_advance(CLOCK_FRESH_S * 1000000LL);
    assert(sntp_mgr_status().time_trusted && !sntp_mgr_status().fresh);
    fake_advance((CLOCK_HOLDOVER_S - CLOCK_FRESH_S) * 1000000LL);
    assert(sntp_mgr_status().synced && !sntp_mgr_status().time_trusted);
    fake_sync();
    assert(sntp_mgr_status().fresh);

    /* A selected config outranks DHCP, but a manual server outranks config. */
    sntp_mgr_set_config_server("config.example");
    assert(sntp_mgr_status().using_config);
    assert(!strcmp(fake_server_name, "config.example"));
    assert(sntp_mgr_set_server("manual.invalid"));
    assert(sntp_mgr_status().using_manual);
    sntp_mgr_reset_servers();
    assert(sntp_mgr_status().using_config);
    sntp_mgr_set_config_server(NULL);
    assert(!sntp_mgr_status().using_config);

    /* A drop AFTER initial success must reconnect without a new CLI command. */
    unsigned attempts = fake_connect_calls;
    fake_event(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(!fake_sntp_running);
    assert(sntp_mgr_status().time_trusted);
    fake_advance(1000000);
    fake_wifi_tick();
    assert(fake_connect_calls == attempts + 1);
    fake_dhcp(0); got_ip();
    assert(wifi_mgr_status().state == WIFI_MGR_CONNECTED);
    assert(!sntp_mgr_status().using_dhcp);

    attempts = fake_connect_calls;
    wifi_mgr_reconnect(); fake_drain();
    assert(!fake_sntp_running);
    fake_advance(3000000); fake_wifi_tick();
    assert(fake_connect_calls == attempts + 1);
    got_ip();

    /* User disconnect cancels even queued retries; credentials replaced
     * during an outstanding stop are serialized, with no duplicate task. */
    wifi_mgr_disconnect(); fake_drain();
    attempts = fake_connect_calls;
    fake_advance(60000000); fake_wifi_tick();
    assert(fake_connect_calls == attempts);
    assert(wifi_mgr_status().state == WIFI_MGR_DISCONNECTED);
    assert(wifi_mgr_connect("first", "", 0)); fake_drain();
    assert(wifi_mgr_connect("second", "", 0));
    assert(wifi_mgr_connect("latest", "", 0)); fake_drain();
    assert(!strcmp(wifi_mgr_status().ssid, "latest"));
    got_ip();
    assert(wifi_mgr_status().state == WIFI_MGR_CONNECTED);

    /* Repeated failures outlive the previous five-attempt budget. */
    for (int i = 0; i < 10; ++i) {
        attempts = fake_connect_calls;
        fake_event(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
        fake_advance(30000000); fake_wifi_tick();
        assert(fake_connect_calls == attempts + 1);
    }
    /* A connect attempt without GOT_IP cannot strand the retry policy. */
    attempts = fake_connect_calls;
    fake_advance(30000000); fake_wifi_tick();
    fake_advance(30000000); fake_wifi_tick();
    assert(fake_connect_calls == attempts + 1);
    wifi_mgr_disconnect(); fake_drain();
    puts("SNTP selection, trust, DHCP recovery and Wi-Fi lifecycle OK");
}
