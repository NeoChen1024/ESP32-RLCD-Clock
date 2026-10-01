#include "storage_mgr.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "diskio_sdmmc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"
#include "wear_levelling.h"
#include "storage_files.h"
#include "config_mgr.h"
#include "audio_mgr.h"

static const char *TAG = "storage";
static SemaphoreHandle_t s_lock;
static sdmmc_card_t *s_card;
static esp_err_t s_mount_error = ESP_ERR_INVALID_STATE;
static wl_handle_t s_flash = WL_INVALID_HANDLE;
static esp_err_t s_flash_error = ESP_ERR_INVALID_STATE;
static uint32_t s_generation;

static bool mount_flash(bool initialize)
{
    if (s_flash != WL_INVALID_HANDLE) return true;
    esp_vfs_fat_mount_config_t cfg = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    cfg.format_if_mount_failed = initialize; /* only explicit `flash init` */
    cfg.max_files = 5;
    cfg.allocation_unit_size = 4096;
    s_flash_error = esp_vfs_fat_spiflash_mount_rw_wl("/flash", "storage", &cfg, &s_flash);
    if (s_flash_error != ESP_OK) {
        s_flash = WL_INVALID_HANDLE;
        ESP_LOGW(TAG, "flash mount: %s; use `flash init` for the new data partition", esp_err_to_name(s_flash_error));
        return false;
    }
    s_generation++;
    ESP_LOGI(TAG, "mounted /flash with wear levelling (4096-byte sectors)");
    return true;
}

const char *storage_root(const char *volume)
{
    return !strcmp(volume, "sd") ? "/sdcard" : !strcmp(volume, "flash") ? "/flash" : NULL;
}

bool storage_lock(unsigned timeout_ms)
{
    return s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
void storage_unlock(void) { xSemaphoreGive(s_lock); }

bool storage_mounted_locked(const char *volume)
{
    return !strcmp(volume, "sd") ? s_card != NULL :
           !strcmp(volume, "flash") && s_flash != WL_INVALID_HANDLE;
}
uint32_t storage_generation_locked(void) { return s_generation; }
bool storage_space_locked(const char *volume, uint64_t *total, uint64_t *free_bytes)
{
    const char *root = storage_root(volume);
    return root && storage_mounted_locked(volume) && esp_vfs_fat_info(root, total, free_bytes) == ESP_OK;
}


static bool mount_card(bool allow_format)
{
    if (s_card) return true;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT; /* 20 MHz; vendor's 1-bit wiring */
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = GPIO_NUM_38;
    slot.cmd = GPIO_NUM_21;
    slot.d0 = GPIO_NUM_39;
    esp_vfs_fat_mount_config_t cfg = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    cfg.format_if_mount_failed = allow_format;
    cfg.max_files = 5;
    cfg.allocation_unit_size = 16 * 1024;
    cfg.disk_status_check_enable = true;
    sdmmc_card_t *card = NULL;
    s_mount_error = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot, &cfg, &card);
    if (s_mount_error != ESP_OK) {
        ESP_LOGW(TAG, "mount failed: %s; card contents preserved", esp_err_to_name(s_mount_error));
        return false;
    }
    s_card = card;
    s_generation++;
    ESP_LOGI(TAG, "mounted %s: %.5s, %llu MiB, 1-bit SDMMC", SDCARD_MOUNT_POINT,
             card->cid.name, (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
    return true;
}

bool storage_mgr_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = mount_card(false);
    mount_flash(false);
    if (s_card && storage_txn_recover("/sdcard")) ESP_LOGW(TAG, "SD transaction needs recovery");
    if (s_flash != WL_INVALID_HANDLE && storage_txn_recover("/flash")) ESP_LOGW(TAG, "flash transaction needs recovery");
    xSemaphoreGive(s_lock);
    return ok;
}

static int status(FILE *out)
{
    fprintf(out, "mounted: %s\npath: %s\nbus: SDMMC 1-bit, CLK=38 CMD=21 D0=39\n",
            s_card ? "yes" : "no", SDCARD_MOUNT_POINT);
    if (!s_card) {
        fprintf(out, "last mount: %s\n", esp_err_to_name(s_mount_error));
        return 1;
    }
    esp_err_t err = sdmmc_get_status(s_card);
    fprintf(out, "card status: %s\n", esp_err_to_name(err));
    if (err != ESP_OK) return 1;
    sdmmc_card_print_info(out, s_card);
    /* Use the actual assigned drive; never assume SD is logical drive zero. */
    char drive[8];
    snprintf(drive, sizeof drive, "%u:", ff_diskio_get_pdrv_card(s_card));
    FATFS *fs;
    DWORD free_clusters;
    FRESULT res = f_getfree(drive, &free_clusters, &fs);
    if (res != FR_OK) {
        fprintf(out, "filesystem query failed: FatFs %d\n", (int)res);
        return 1;
    }
    uint64_t cluster_bytes = (uint64_t)fs->csize * s_card->csd.sector_size;
    fprintf(out, "filesystem: %s\ncluster: %llu bytes\nvolume: %llu bytes\nfree: %llu bytes\n",
            fs->fs_type == FS_FAT32 ? "FAT32" : fs->fs_type == FS_FAT16 ? "FAT16" : "FAT12",
            (unsigned long long)cluster_bytes,
            (unsigned long long)(fs->n_fatent - 2) * cluster_bytes,
            (unsigned long long)free_clusters * cluster_bytes);
    return 0;
}

/* No drive prefixes, absolute paths, dot components or alternate separators. */
static bool card_path(const char *rel, char *out, size_t size)
{
    if (!rel || *rel == '/') return false;
    const char *component = rel;
    for (const char *p = rel;; ++p) {
        if (*p == '\0' || *p == '/') {
            size_t n = p - component;
            if ((n == 1 && component[0] == '.') ||
                (n == 2 && component[0] == '.' && component[1] == '.') ||
                (n == 0 && *rel != '\0')) return false;
            if (!*p) break;
            component = p + 1;
        } else if ((unsigned char)*p < 32 || *p == 127 || *p == ':' || *p == '\\') {
            return false;
        }
    }
    int n = snprintf(out, size, "%s/%s", SDCARD_MOUNT_POINT, rel);
    return n >= 0 && (size_t)n < size;
}

static int list_files(const char *path, FILE *out)
{
    DIR *dir = opendir(path);
    if (!dir) { fprintf(out, "open directory: %s\n", strerror(errno)); return 1; }
    struct dirent *entry;
    int result = 0;
    errno = 0;
    while ((entry = readdir(dir))) {
        /* Size errors are reported rather than presenting them as zero bytes. */
        char full[512];
        int n = snprintf(full, sizeof full, "%s/%s", path, entry->d_name);
        struct stat st;
        if (n < 0 || (size_t)n >= sizeof full || stat(full, &st)) {
            fprintf(out, "? %s (stat failed)\n", entry->d_name);
            result = 1;
        } else {
            fprintf(out, "%c %10lld %s\n", S_ISDIR(st.st_mode) ? 'd' : 'f',
                    (long long)st.st_size, entry->d_name);
        }
        errno = 0;
    }
    if (errno) { fprintf(out, "read directory: %s\n", strerror(errno)); result = 1; }
    if (closedir(dir)) result = 1;
    return result;
}

static int read_file(const char *path, FILE *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(out, "open file: %s\n", strerror(errno)); return 1; }
    /* Bounded preview; binary/control bytes escaped for a safe serial display. */
    size_t n = 0;
    int c;
    while (n < 4096 && (c = fgetc(f)) != EOF) {
        if (c == '\n' || c == '\r' || c == '\t' || (c >= 32 && c < 127)) fputc(c, out);
        else fprintf(out, "\\x%02x", (unsigned)c);
        n++;
    }
    bool failed = ferror(f);
    bool more = !failed && n == 4096 && fgetc(f) != EOF;
    failed |= ferror(f) != 0;
    if (fclose(f)) failed = true;
    fprintf(out, "\n[%zu bytes%s%s]\n", n, more ? "; preview truncated" : "", failed ? "; read failed" : "");
    return failed ? 1 : 0;
}

static int selftest(FILE *out)
{
    const char *path = SDCARD_MOUNT_POINT "/.rlcd-sd-selftest.tmp";
    /* Exclusive creation protects any existing file, including an interrupted
     * previous test. Only remove files created successfully by THIS run. */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { fprintf(out, "test file not created: %s\n", strerror(errno)); return 1; }
    uint8_t expected[512], actual[512];
    for (size_t i = 0; i < sizeof expected; ++i) expected[i] = (uint8_t)(i * 37 + 11);
    bool ok = true;
    for (unsigned i = 0; i < 8 && ok; ++i)
        ok = write(fd, expected, sizeof expected) == sizeof expected;
    if (ok) ok = fsync(fd) == 0;
    if (close(fd)) ok = false;
    if (ok) {
        fd = open(path, O_RDONLY);
        if (fd < 0) ok = false;
        else {
            for (unsigned i = 0; i < 8 && ok; ++i)
                ok = read(fd, actual, sizeof actual) == sizeof actual && !memcmp(expected, actual, sizeof actual);
            if (ok) ok = read(fd, actual, 1) == 0;
            if (close(fd)) ok = false;
        }
    }
    if (unlink(path)) {
        fprintf(out, "test cleanup failed: %s\n", strerror(errno));
        ok = false;
    }
    fprintf(out, "SD write/fsync/close/reopen/read/compare/delete: %s (4096 bytes)\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int storage_mgr_command(int argc, char **argv, FILE *out)
{
    if (!s_lock) { fprintf(out, "SD manager unavailable\n"); return 1; }
    const char *op = argc == 1 ? "status" : argv[1];
    int result = 1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if ((!strcmp(op, "format") || !strcmp(op, "unmount")) && argc == 2) audio_mgr_release_locked("sd", NULL);
    if (!strcmp(op, "format") && argc == 2) {
        /* Explicit destructive command; normal mounts always pass false. */
        fprintf(out, "Formatting SD FAT volume; existing files will be erased.\n");
        if (mount_card(true)) {
            esp_err_t err = esp_vfs_fat_sdcard_format(SDCARD_MOUNT_POINT, s_card);
            s_generation++;
            fprintf(out, "SD format: %s\n", esp_err_to_name(err));
            if (err == ESP_OK) {
                if (config_mgr_started()) config_mgr_reload_locked();
                result = status(out);
            }
        }
    } else if (!strcmp(op, "status") && argc <= 2) result = status(out);
    else if (!strcmp(op, "mount") && argc == 2) {
        if (mount_card(false)) {
            if (config_mgr_started()) config_mgr_reload_locked();
            result = status(out);
        }
        else fprintf(out, "mount failed: %s (no formatting)\n", esp_err_to_name(s_mount_error));
    } else if (!strcmp(op, "unmount") && argc == 2) {
        esp_err_t err = s_card ? esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card) : ESP_OK;
        if (err == ESP_OK) {
            s_card = NULL; result = 0;
            s_generation++;
            if (config_mgr_started()) config_mgr_reload_locked();
        }
        fprintf(out, "unmount: %s\n", esp_err_to_name(err));
    } else if ((!strcmp(op, "ls") && argc <= 3) || (!strcmp(op, "cat") && argc == 3)) {
        char path[256];
        if (!s_card) fprintf(out, "SD not mounted\n");
        else if (!card_path(argc == 3 ? argv[2] : "", path, sizeof path)) fprintf(out, "invalid relative SD path\n");
        else result = !strcmp(op, "ls") ? list_files(path, out) : read_file(path, out);
    } else if (!strcmp(op, "test") && argc == 2) {
        if (!s_card) fprintf(out, "SD not mounted\n");
        else result = selftest(out);
    } else fprintf(out, "usage: sd [status] | mount | unmount | ls [relative-dir] | cat <relative-file> | test | format (ERASE SD)\n");
    xSemaphoreGive(s_lock);
    return result;
}

int storage_flash_command(int argc, char **argv, FILE *out)
{
    if (!s_lock) return 1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int result = 1;
    const char *op = argc > 1 ? argv[1] : "status";
    if (argc <= 2 && (!strcmp(op, "init") || !strcmp(op, "mount") || !strcmp(op, "status"))) {
        if (!strcmp(op, "init")) { audio_mgr_release_locked("flash", NULL); mount_flash(true); }
        else if (!strcmp(op, "mount")) mount_flash(false);
        if (s_flash != WL_INVALID_HANDLE) {
            if (storage_txn_recover("/flash")) fprintf(out, "transaction recovery failed\n");
            if (config_mgr_started() && strcmp(op, "status")) config_mgr_reload_locked();
            uint64_t total, free_bytes;
            if (storage_space_locked("flash", &total, &free_bytes)) {
                fprintf(out, "mounted: yes\npath: /flash\nwear levelling: enabled, sector 4096 bytes\nvolume: %llu bytes\nfree: %llu bytes\n",
                        (unsigned long long)total, (unsigned long long)free_bytes);
                result = 0;
            }
        } else fprintf(out, "mounted: no\nlast mount: %s\n", esp_err_to_name(s_flash_error));
    } else fprintf(out, "usage: flash status | mount | init (formats if unmountable)\n");
    xSemaphoreGive(s_lock);
    return result;
}
