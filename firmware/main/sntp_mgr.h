#ifndef RLCD_FW_SNTP_MGR_H
#define RLCD_FW_SNTP_MGR_H

#include <stdbool.h>
#include <stdint.h>

/*
 * SNTP manager.
 *
 * Server selection (in priority order):
 *   1. Manual override via `ntp server <host|ip>` (not persisted).
 *   2. Selected SD/flash config's `ntp_server`, when present.
 *   3. DHCP option 42 (when the network advertises NTP servers).
 *   4. Public pool fallback (pool.ntp.org / time.google.com).
 *
 * The DHCP-provided list is cached on each lease ACK, even under manual or
 * config mode; a manual `ntp server` command overrides both until reboot or
 * `ntp reset`.
 * `server0` shows the selected hostname or DHCP address; `server0_ip`
 * shows slot zero's current address. Current builds configure one slot.
 */

typedef struct {
    bool started;
    bool synced;            /* at least one successful sync this boot */
    bool rtc_seeded;        /* boot time came from the RTC before SNTP */
    bool time_trusted;      /* last sync younger than holdover limit */
    bool fresh;             /* current source synced recently, network up */
    int64_t unix_sec;       /* current system time at snapshot */
    int64_t unix_ms;        /* same snapshot as trust state, for rendering */
    uint32_t ntp_age_s;     /* seconds since last successful sync */
    bool using_dhcp;        /* servers currently sourced from DHCP */
    bool using_manual;      /* servers currently sourced from manual override */
    bool using_config;      /* active versioned config supplied the server */
    char server0[64];       /* configured server (host or IP) */
    char server0_ip[16];    /* address actually in use ("0.0.0.0" if none) */
} sntp_mgr_status_t;

/* Start the SNTP service; config is supplied after storage mounts at boot. */
void sntp_mgr_start(void);
/* Import RTC time whose last-sync age was already checked against the 24 h
 * policy. Does not count as an SNTP sync or make the current source fresh. */
bool sntp_mgr_seed_rtc(uint32_t age_s);

/* Called from the Wi-Fi event loop after GOT_IP: select the current priority
 * source. Disconnect stops SNTP and clears
 * the lease cache, but preserves last-good system-clock trust. */
void sntp_mgr_wifi_connected(void);
void sntp_mgr_wifi_disconnected(void);

/* Set the NTP server explicitly (hostname or IPv4). Overrides DHCP until
 * sntp_mgr_reset_servers() or reboot. Returns false on invalid input. */
bool sntp_mgr_set_server(const char *server);
/* Set/clear the selected config server; manual CLI override still wins. */
void sntp_mgr_set_config_server(const char *server);

/* Clear a manual override and return to config/DHCP/fallback selection. */
void sntp_mgr_reset_servers(void);

/* Force a resync (restart SNTP). */
void sntp_mgr_resync(void);

sntp_mgr_status_t sntp_mgr_status(void);

#endif
