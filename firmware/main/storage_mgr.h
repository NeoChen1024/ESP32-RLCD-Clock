#ifndef RLCD_STORAGE_MGR_H
#define RLCD_STORAGE_MGR_H
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#define SDCARD_MOUNT_POINT "/sdcard"
/* Initialize SD and internal flash once at boot. Missing filesystems are
 * nonfatal; ordinary mounts never format. Explicit CLI commands may format. */
bool storage_mgr_start(void);
/* Serial diagnostics; owns the storage mutex for the whole operation.
 * Paths are relative to /sdcard. Unmount before removing the card.
 * HTTP and future audio users share this ownership, not bypass it. */
int storage_mgr_command(int argc, char **argv, FILE *out);
/* Shared ownership for HTTP, diagnostics and future audio. Roots are fixed;
 * acquire before touching mounted state/files and release after closing files. */
const char *storage_root(const char *volume);
bool storage_lock(unsigned timeout_ms);
void storage_unlock(void);
bool storage_mounted_locked(const char *volume);
/* Increments on every mount, unmount or format of either volume. A holder
 * that releases the mutex mid-operation (long HTTP transfers yield to the
 * audio reader) must abandon open files if this changed meanwhile. */
uint32_t storage_generation_locked(void);
bool storage_space_locked(const char *volume, uint64_t *total, uint64_t *free_bytes);
int storage_flash_command(int argc, char **argv, FILE *out);
#endif
