#ifndef RLCD_FW_SNTP_MGR_H
#define RLCD_FW_SNTP_MGR_H

#include <stdbool.h>
#include <stdint.h>

/*
 * SNTP manager.
 *
 * Server selection (in priority order):
 *   1. Manual override via `ntp server <host|ip>` (not persisted).
 *   2. DHCP option 42 (when the network advertises NTP servers).
 *   3. Public pool fallback (pool.ntp.org / time.google.com).
 *
 * The DHCP-provided list is picked up automatically after Wi-Fi connects;
 * a manual `ntp server` command overrides it until reboot or `ntp reset`.
 * DHCP servers are stored as IPs by lwIP, so `server0` shows the name we
 * configured while `server0_ip` shows the address actually in use.
 */

typedef struct {
    bool started;
    bool synced;            /* SNTP_SYNC_STATUS_COMPLETED seen */
    int64_t unix_sec;       /* current system time at snapshot */
    uint32_t ntp_age_s;     /* seconds since last successful sync */
    bool using_dhcp;        /* servers currently sourced from DHCP */
    bool using_manual;      /* servers currently sourced from manual override */
    char server0[64];       /* configured server (host or IP) */
    char server0_ip[16];    /* address actually in use ("0.0.0.0" if none) */
} sntp_mgr_status_t;

/* Start the SNTP service (DHCP + fallback servers; called once at boot). */
void sntp_mgr_start(void);

/* Called after Wi-Fi obtains an IP: re-read DHCP-provided NTP servers and
 * apply them unless a manual override is in effect. */
void sntp_mgr_wifi_connected(void);

/* Set the NTP server explicitly (hostname or IPv4). Overrides DHCP until
 * sntp_mgr_reset_servers() or reboot. Returns false on invalid input. */
bool sntp_mgr_set_server(const char *server);

/* Clear a manual override and return to DHCP/fallback selection. */
void sntp_mgr_reset_servers(void);

/* Force a resync (restart SNTP). */
void sntp_mgr_resync(void);

sntp_mgr_status_t sntp_mgr_status(void);

#endif
