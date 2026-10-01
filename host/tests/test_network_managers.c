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
    assert(sntp_mgr_seed_rtc(60));
    assert(sntp_mgr_status().rtc_seeded && sntp_mgr_status().time_trusted);
    assert(!sntp_mgr_status().synced && !sntp_mgr_status().fresh);
    assert(!sntp_mgr_seed_rtc(0));
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
    /* Past 24 h the RTC cross-check decides between RTC_HOLD and INVALID. */
    assert(sntp_mgr_status().time_state == CLOCK_INVALID && !sntp_mgr_status().time_valid);
    fake_rtc_hold_ok = true;
    assert(sntp_mgr_status().time_state == CLOCK_RTC_HOLD && sntp_mgr_status().time_valid);
    fake_rtc_hold_ok = false;
    /* Implausible server times never reach the clock or the trust state. */
    unsigned sets = fake_clock_sets;
    fake_sync_at(clock_build_epoch() - 1);
    fake_sync_at(clock_build_epoch() + CLOCK_PLAUSIBLE_SPAN_S + 1);
    assert(fake_clock_sets == sets && sntp_mgr_status().rejected == 2);
    assert(sntp_mgr_status().time_state == CLOCK_INVALID);
    fake_sync();
    assert(fake_clock_sets == sets + 1 && sntp_mgr_status().time_state == CLOCK_TRUSTED);
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

    /* ---- AUTO: known networks (secrets/wifi.json) ---- */
    wifi_mgr_network_t known[3] = {{"home", "password1"}, {"lab", ""}, {"cafe", "password3"}};
    assert(wifi_mgr_set_known(known, 3)); fake_drain();
    assert(wifi_mgr_status().mode == WIFI_MGR_MODE_OFF && wifi_mgr_status().known == 3);
    assert(wifi_mgr_status().state == WIFI_MGR_DISCONNECTED); /* OFF ignores the list */
    unsigned scans = fake_scan_calls;
    const char *visible[] = {"other", "cafe", "lab", "cafe"};
    const int8_t rssi[] = {-30, -70, -50, -60};
    fake_scan_results(4, visible, rssi);
    wifi_mgr_reset(); fake_drain();
    assert(wifi_mgr_status().mode == WIFI_MGR_MODE_AUTO);
    assert(fake_scan_calls == scans + 1);
    /* Strongest visible known network first: lab -50 beats cafe's best -60. */
    assert(!strcmp((char *)fake_wifi_config.sta.ssid, "lab"));
    assert(fake_wifi_config.sta.threshold.authmode == WIFI_AUTH_OPEN);
    attempts = fake_connect_calls;
    fake_event(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL); /* lab fails */
    assert(fake_connect_calls == attempts + 1);
    assert(!strcmp((char *)fake_wifi_config.sta.ssid, "cafe"));
    assert(!strcmp((char *)fake_wifi_config.sta.password, "password3"));
    got_ip();
    assert(wifi_mgr_status().state == WIFI_MGR_CONNECTED && !strcmp(wifi_mgr_status().ssid, "cafe"));

    /* An unchanged entry keeps the link; a changed password rescans. */
    scans = fake_scan_calls;
    assert(wifi_mgr_set_known(known, 3)); fake_drain();
    assert(wifi_mgr_status().state == WIFI_MGR_CONNECTED && fake_scan_calls == scans);
    snprintf(known[2].password, sizeof known[2].password, "changed-pass");
    assert(wifi_mgr_set_known(known, 3)); fake_drain();
    assert(fake_scan_calls == scans + 1 && !strcmp(wifi_mgr_status().ssid, "lab"));
    got_ip();

    /* A drop after success rescans on the next tick. */
    scans = fake_scan_calls;
    fake_event(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    fake_wifi_tick();
    assert(fake_scan_calls == scans + 1 && !strcmp(wifi_mgr_status().ssid, "lab"));
    got_ip();

    /* No known network visible: rescan after 10 s, then 20 s. */
    const char *none[] = {"other"};
    const int8_t none_rssi[] = {-40};
    fake_scan_results(1, none, none_rssi);
    scans = fake_scan_calls;
    fake_event(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    fake_wifi_tick();
    assert(fake_scan_calls == scans + 1);
    assert(wifi_mgr_status().state == WIFI_MGR_CONNECTING && !wifi_mgr_status().ssid[0]);
    fake_advance(9000000); fake_wifi_tick(); assert(fake_scan_calls == scans + 1);
    fake_advance(1000000); fake_wifi_tick(); assert(fake_scan_calls == scans + 2);
    fake_advance(19000000); fake_wifi_tick(); assert(fake_scan_calls == scans + 2);
    fake_advance(1000000); fake_wifi_tick(); assert(fake_scan_calls == scans + 3);

    /* A stalled attempt is aborted and the next candidate is tried. */
    const char *two[] = {"home", "lab"};
    const int8_t two_rssi[] = {-40, -45};
    fake_scan_results(2, two, two_rssi);
    fake_advance(60000000); fake_wifi_tick();
    assert(!strcmp(wifi_mgr_status().ssid, "home"));
    fake_advance(30000000); fake_wifi_tick();
    assert(!strcmp(wifi_mgr_status().ssid, "lab"));
    got_ip();

    /* MANUAL outranks known networks until reset. */
    assert(wifi_mgr_connect("manual", "", 0)); fake_drain();
    got_ip();
    assert(wifi_mgr_status().mode == WIFI_MGR_MODE_MANUAL && !strcmp(wifi_mgr_status().ssid, "manual"));
    assert(wifi_mgr_set_known(known, 2)); fake_drain();
    assert(wifi_mgr_status().state == WIFI_MGR_CONNECTED && !strcmp(wifi_mgr_status().ssid, "manual"));
    wifi_mgr_reset(); fake_drain();
    assert(wifi_mgr_status().mode == WIFI_MGR_MODE_AUTO && !strcmp(wifi_mgr_status().ssid, "home"));
    got_ip();

    /* Removing every known network disconnects and stops scanning. */
    assert(wifi_mgr_set_known(NULL, 0)); fake_drain();
    assert(wifi_mgr_status().state == WIFI_MGR_DISCONNECTED && !wifi_mgr_status().known);
    scans = fake_scan_calls;
    fake_advance(400000000); fake_wifi_tick();
    assert(fake_scan_calls == scans);
    puts("SNTP selection, time state, plausibility, DHCP recovery, Wi-Fi lifecycle and known-network scanning OK");
}
