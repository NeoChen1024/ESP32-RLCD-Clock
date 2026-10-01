#ifndef RLCD_STORAGE_FILES_H
#define RLCD_STORAGE_FILES_H
#include <stdbool.h>
#include <stddef.h>

#define STORAGE_REL_MAX 112
#define STORAGE_JSON_MAX (16U * 1024U)
#define STORAGE_WAV_MAX (64U * 1024U * 1024U)
#define STORAGE_PATH_MAX 192
#define STORAGE_LEAP_FILE "time/leap-seconds.list"
#define STORAGE_WIFI_SECRETS "secrets/wifi.json"
/* Canonical managed files: config/<sortable-name>.json,
 * sounds/<ASCII-name>.wav, the single time/leap-seconds.list table and the
 * write-only secrets/wifi.json. Directory paths end with '/'. */
bool storage_file_allowed(const char *relative);
bool storage_directory_allowed(const char *relative);
bool storage_parse_uri(const char *uri, char volume[8], char relative[STORAGE_REL_MAX]);
size_t storage_file_limit(const char *relative);
bool storage_validate_file(const char *relative, const char *path);
typedef bool (*storage_config_accept_fn)(const char *path, void *context);
/* Visit config versions in descending bytewise filename order until accept
 * succeeds. Return 1 with selected relative path, 0 if none is usable,
 * -1 if the directory cannot be scanned. No heap allocation or fixed
 * version count. Caller owns the volume lock. */
int storage_config_select(const char *root, storage_config_accept_fn accept,
                          void *context, char selected[STORAGE_REL_MAX]);

/* Visit config versions that sort strictly before keep (the version this
 * volume selects); newer, necessarily invalid versions are never visited.
 * With remove, each visited file is unlinked after its visit. Return the
 * number visited, or -1 on a scan or unlink error (earlier removals stay).
 * Caller owns the volume lock. */
int storage_config_cleanup(const char *root, const char *keep, bool remove,
                           void (*visit)(const char *relative, void *context), void *context);

/* Caller holds the volume lock. Single in-flight transaction per volume.
 * Recoverable replacement for FAT's non-overwriting rename, not a guarantee
 * against filesystem metadata damage on power loss. The private journal is
 * removed LAST. Unknown or malformed transaction state is left untouched. */
int storage_txn_recover(const char *root);
int storage_txn_begin(const char *root, const char *relative); /* open upload fd */
int storage_txn_commit(const char *root); /* caller has fsynced/closed upload */
#endif
