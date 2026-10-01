#ifndef RLCD_CONFIG_MGR_H
#define RLCD_CONFIG_MGR_H
#include <stdbool.h>
#include "storage_files.h"
#include "tz_rule.h"

typedef struct {
    bool found;
    char volume[8];
    char path[STORAGE_REL_MAX];
    tz_rule_t tz;   /* "tz" POSIX rule, else legacy tz_offset_minutes, else UTC+8 */
    char ntp_server[64];
} config_selection_t;

/* Semantic validation of supported fields in a JSON config object.
 * Unknown fields are retained for future config schemas. When both "tz" and
 * the legacy "tz_offset_minutes" are present, both must be valid and "tz"
 * applies. */
bool config_mgr_file_valid(const char *path);
/* Boot or explicit reload; these functions acquire the storage mutex.
 * A reload also reselects the leap-seconds.list table (leap_mgr.h) and the
 * known Wi-Fi networks (wifi_secrets.h), so volume mount changes refresh
 * all three. */
void config_mgr_start(void);
bool config_mgr_started(void);
bool config_mgr_reload(void);
/* Storage owner must already hold the shared mutex. */
bool config_mgr_reload_locked(void);
void config_mgr_current_locked(config_selection_t *out);

/* Remove config versions older than the one this volume would select on
 * its own; newer (invalid) versions and other volumes are untouched. With
 * apply false, only lists them. kept receives the retained version. */
#define CONFIG_CLEANUP_UNMOUNTED  (-2)
#define CONFIG_CLEANUP_NO_VALID   (-3)
int config_mgr_cleanup_locked(const char *volume, bool apply, char kept[STORAGE_REL_MAX],
                              void (*visit)(const char *relative, void *context), void *context);
#endif
