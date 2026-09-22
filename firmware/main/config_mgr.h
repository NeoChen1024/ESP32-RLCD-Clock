#ifndef RLCD_CONFIG_MGR_H
#define RLCD_CONFIG_MGR_H
#include <stdbool.h>
#include "storage_files.h"

typedef struct {
    bool found;
    char volume[8];
    char path[STORAGE_REL_MAX];
    int tz_offset_minutes;
    char ntp_server[64];
} config_selection_t;

/* Semantic validation of supported fields in a JSON config object.
 * Unknown fields are retained for future config schemas. */
bool config_mgr_file_valid(const char *path);
/* Boot or explicit reload; these functions acquire the storage mutex. */
void config_mgr_start(void);
bool config_mgr_started(void);
bool config_mgr_reload(void);
/* Storage owner must already hold the shared mutex. */
bool config_mgr_reload_locked(void);
void config_mgr_current_locked(config_selection_t *out);
#endif
