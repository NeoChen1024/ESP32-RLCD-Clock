#ifndef RLCD_WIFI_SECRETS_H
#define RLCD_WIFI_SECRETS_H
#include <stdbool.h>

/*
 * Known Wi-Fi networks from secrets/wifi.json:
 *   {"networks": [{"ssid": "...", "password": "..."}, {"ssid": "open-ap"}]}
 * At most WIFI_MGR_KNOWN_MAX entries; SSIDs are 1..32 bytes and unique; a
 * password is omitted/empty (open network), 8..63 printable ASCII
 * characters, or 64 hex digits. Unknown keys are ignored.
 *
 * Only the active volume's file is used (storage_active_volume_locked());
 * it is never versioned, never served over HTTP and passwords are never
 * logged.
 */

typedef struct {
    bool found;
    char volume[8];
    unsigned count;
} wifi_secrets_info_t;

bool wifi_secrets_file_valid(const char *path);
/* Storage owner must already hold the shared mutex. */
bool wifi_secrets_reload_locked(void);
void wifi_secrets_current_locked(wifi_secrets_info_t *out);
#endif
