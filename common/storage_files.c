#include "storage_files.h"
#include "leap_table.h"
#include "audio_source.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cJSON.h"

static const char magic[] = "RLCD-TXN-1\n";
static void path_join(char *out, const char *root, const char *relative)
{
    snprintf(out, STORAGE_PATH_MAX, "%s/%s", root, relative);
}
bool storage_directory_allowed(const char *s)
{
    return !strcmp(s, "") || !strcmp(s, "sounds/") || !strcmp(s, "config/") ||
           !strcmp(s, "time/") || !strcmp(s, "secrets/");
}
static bool managed_basename(const char *name, size_t suffix_length)
{
    size_t n = strlen(name);
    if (n <= suffix_length || name[0] == '.' || name[0] == ' ' || name[n - 1] == ' ') return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.' || *p == ' ')) return false;
    return true;
}
bool storage_file_allowed(const char *s)
{
    size_t n = strlen(s);
    if (n >= STORAGE_REL_MAX || n < 12) return false;
    if (!strcmp(s, STORAGE_LEAP_FILE) || !strcmp(s, STORAGE_WIFI_SECRETS)) return true;
    if (!strncmp(s, "config/", 7)) {
        const char *name = s + 7;
        size_t len = strlen(name);
        return len > 5 && !strcmp(name + len - 5, ".json") && managed_basename(name, 5);
    }
    if (!strncmp(s, "sounds/", 7)) {
        const char *name = s + 7;
        size_t len = strlen(name);
        return (len > 4 && !strcmp(name + len - 4, ".wav") && managed_basename(name, 4)) ||
               (len > 5 && !strcmp(name + len - 5, ".flac") && managed_basename(name, 5));
    }
    return false;
}
static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool storage_parse_uri(const char *uri, char volume[8], char relative[STORAGE_REL_MAX])
{
    if (strncmp(uri, "/fs/", 4)) return false;
    const char *p = uri + 4, *slash = strchr(p, '/');
    if (!slash || slash - p >= 8 || slash == p) return false;
    memcpy(volume, p, slash - p); volume[slash - p] = 0;
    if (strcmp(volume, "sd") && strcmp(volume, "flash")) return false;
    size_t n = 0;
    for (p = slash + 1; *p; ++p) {
        unsigned char ch = *p;
        if (ch == '%') {
            if (!p[1] || !p[2] || hex(p[1]) < 0 || hex(p[2]) < 0) return false;
            ch = (unsigned char)(hex(p[1]) * 16 + hex(p[2])); p += 2;
            /* Encoded separators and NUL must never acquire path meaning. */
            if (!ch || ch == '/' || ch == '\\') return false;
        }
        if (n + 1 >= STORAGE_REL_MAX) return false;
        relative[n++] = (char)ch;
    }
    relative[n] = 0;
    return storage_directory_allowed(relative) || storage_file_allowed(relative);
}
size_t storage_file_limit(const char *relative)
{
    if (!strcmp(relative, STORAGE_LEAP_FILE)) return LEAP_FILE_MAX;
    return !strncmp(relative, "sounds/", 7) ? STORAGE_WAV_MAX : STORAGE_JSON_MAX;
}

int storage_config_select(const char *root, storage_config_accept_fn accept,
                          void *context, char selected[STORAGE_REL_MAX])
{
    if (!root || !accept || !selected || strlen(root) > 24) return -1;
    selected[0] = 0;
    char directory[STORAGE_PATH_MAX];
    snprintf(directory, sizeof directory, "%s/config", root);
    char upper[STORAGE_REL_MAX] = {0};
    for (;;) {
        DIR *dir = opendir(directory);
        if (!dir) return errno == ENOENT ? 0 : -1;
        char best[STORAGE_REL_MAX] = {0};
        struct dirent *entry;
        errno = 0;
        while ((entry = readdir(dir))) {
            char relative[STORAGE_REL_MAX];
            int n = snprintf(relative, sizeof relative, "config/%s", entry->d_name);
            if (n < 0 || (size_t)n >= sizeof relative || !storage_file_allowed(relative)) continue;
            if (upper[0] && strcmp(relative, upper) >= 0) continue;
            if (!best[0] || strcmp(relative, best) > 0) memcpy(best, relative, (size_t)n + 1);
            errno = 0;
        }
        int scan_error = errno;
        if (closedir(dir) || scan_error) return -1;
        if (!best[0]) return 0;
        char path[STORAGE_PATH_MAX]; path_join(path, root, best);
        if (accept(path, context)) { memcpy(selected, best, strlen(best) + 1); return 1; }
        memcpy(upper, best, strlen(best) + 1);
    }
}

static int compare_names(const void *a, const void *b) { return strcmp(a, b); }

int storage_config_cleanup(const char *root, const char *keep, bool remove,
                           void (*visit)(const char *relative, void *context), void *context)
{
    if (!root || !keep || strncmp(keep, "config/", 7) || strlen(root) > 24) return -1;
    char directory[STORAGE_PATH_MAX];
    snprintf(directory, sizeof directory, "%s/config", root);
    /* Collect first: unlinking while readdir is open is unspecified. */
    char (*names)[STORAGE_REL_MAX] = NULL;
    size_t count = 0, capacity = 0;
    DIR *dir = opendir(directory);
    if (!dir) return errno == ENOENT ? 0 : -1;
    struct dirent *entry;
    int result = 0;
    errno = 0;
    while ((entry = readdir(dir))) {
        char relative[STORAGE_REL_MAX];
        int n = snprintf(relative, sizeof relative, "config/%s", entry->d_name);
        if (n < 0 || (size_t)n >= sizeof relative || !storage_file_allowed(relative) ||
            strcmp(relative, keep) >= 0) { errno = 0; continue; }
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            void *grown = realloc(names, next * sizeof *names);
            if (!grown) { result = -1; break; }
            names = grown; capacity = next;
        }
        memcpy(names[count++], relative, (size_t)n + 1);
        errno = 0;
    }
    if (errno) result = -1;
    if (closedir(dir)) result = -1;
    if (count) qsort(names, count, sizeof *names, compare_names);
    for (size_t i = 0; i < count && result >= 0; ++i) {
        if (visit) visit(names[i], context);
        if (remove) {
            char path[STORAGE_PATH_MAX]; path_join(path, root, names[i]);
            if (unlink(path)) { result = -1; break; }
        }
        ++result;
    }
    free(names);
    return result;
}

bool storage_validate_file(const char *relative, const char *path)
{
    if (!strcmp(relative, STORAGE_LEAP_FILE)) {
        static leap_table_t table;   /* callers hold the storage lock */
        return leap_table_load(path, &table);
    }
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    struct stat st;
    if (fstat(fileno(f), &st) || st.st_size <= 0 || (uint64_t)st.st_size > storage_file_limit(relative)) {
        fclose(f); return false;
    }
    bool ok = false;
    if (!strncmp(relative, "sounds/", 7)) {
        /* Only files the player can decode are accepted. */
        ok = audio_probe(relative, f, (uint64_t)st.st_size) == AUDIO_SRC_OK;
    } else {
        size_t n = (size_t)st.st_size;
        char *buf = malloc(n + 1);
        if (buf && fread(buf, 1, n, f) == n && !memchr(buf, 0, n)) {
            buf[n] = 0;
            /* Bound nesting before calling cJSON: reject adversarial depth
             * without letting its recursive parser exhaust a task stack. */
            int depth = 0;
            bool string = false, escaped = false, bounded = true;
            for (size_t i = 0; i < n; ++i) {
                char c = buf[i];
                if (string) {
                    if (escaped) escaped = false;
                    else if (c == '\\') escaped = true;
                    else if (c == '"') string = false;
                } else if (c == '"') string = true;
                else if (c == '{' || c == '[') { if (++depth > 16) { bounded = false; break; } }
                else if (c == '}' || c == ']') depth--;
            }
            if (bounded) {
                cJSON *json = cJSON_ParseWithLengthOpts(buf, n + 1, NULL, true);
                ok = cJSON_IsObject(json);
                cJSON_Delete(json);
            }
        }
        free(buf);
    }
    if (fclose(f)) ok = false;
    return ok;
}

/* 0 missing, 1 regular file, -1 error/non-file. */
static int exists(const char *path)
{
    struct stat st;
    if (!stat(path, &st)) return S_ISREG(st.st_mode) ? 1 : -1;
    return errno == ENOENT ? 0 : -1;
}
static int remove_if_present(const char *path)
{
    int e = exists(path);
    if (e < 0) return -1;
    return e ? unlink(path) : 0;
}
static int load_record(const char *root, char relative[STORAGE_REL_MAX])
{
    char path[STORAGE_PATH_MAX]; path_join(path, root, ".rlcd-txn/record");
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? 0 : -1;
    char data[sizeof magic + STORAGE_REL_MAX];
    size_t n = fread(data, 1, sizeof data, f);
    bool error = ferror(f);
    if (fclose(f)) error = true;
    if (error || n <= sizeof magic - 1 || n >= sizeof data || memcmp(data, magic, sizeof magic - 1)) return -1;
    size_t len = n - (sizeof magic - 1);
    if (len >= STORAGE_REL_MAX || memchr(data, 0, n)) return -1;
    memcpy(relative, data + sizeof magic - 1, len); relative[len] = 0;
    return storage_file_allowed(relative) ? 1 : -1;
}
int storage_txn_recover(const char *root)
{
    char relative[STORAGE_REL_MAX], temp[STORAGE_PATH_MAX], backup[STORAGE_PATH_MAX];
    path_join(temp, root, ".rlcd-txn/upload"); path_join(backup, root, ".rlcd-txn/backup");
    int record = load_record(root, relative);
    if (record < 0) return -1;
    if (!record) return exists(temp) == 0 && exists(backup) == 0 ? 0 : -1;
    char dest[STORAGE_PATH_MAX]; path_join(dest, root, relative);
    int old = exists(backup), current = exists(dest);
    if (old < 0 || current < 0) return -1;
    if (old && !current) { if (rename(backup, dest)) return -1; }
    else if (old && unlink(backup)) return -1;
    if (remove_if_present(temp)) return -1;
    char journal[STORAGE_PATH_MAX]; path_join(journal, root, ".rlcd-txn/record");
    return unlink(journal);
}
int storage_txn_begin(const char *root, const char *relative)
{
    if (!storage_file_allowed(relative) || strlen(root) > 24) { errno = EINVAL; return -1; }
    if (storage_txn_recover(root)) { errno = EIO; return -1; }
    char path[STORAGE_PATH_MAX]; path_join(path, root, ".rlcd-txn");
    if (mkdir(path, 0700) && errno != EEXIST) return -1;
    const char *slash = strchr(relative, '/');
    if (slash) {
        char directory[STORAGE_REL_MAX];
        memcpy(directory, relative, (size_t)(slash - relative));
        directory[slash - relative] = 0;
        path_join(path, root, directory);
        if (mkdir(path, 0755) && errno != EEXIST) return -1;
    }
    path_join(path, root, ".rlcd-txn/record");
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    size_t n = strlen(relative);
    bool ok = write(fd, magic, sizeof magic - 1) == sizeof magic - 1 && write(fd, relative, n) == (ssize_t)n;
    if (ok) ok = fsync(fd) == 0;
    if (close(fd)) ok = false;
    if (!ok) { unlink(path); errno = EIO; return -1; }
    path_join(path, root, ".rlcd-txn/upload");
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { int saved = errno; storage_txn_recover(root); errno = saved; }
    return fd;
}
int storage_txn_commit(const char *root)
{
    char relative[STORAGE_REL_MAX], dest[STORAGE_PATH_MAX], temp[STORAGE_PATH_MAX], backup[STORAGE_PATH_MAX];
    if (load_record(root, relative) != 1) { errno = EIO; return -1; }
    path_join(dest, root, relative); path_join(temp, root, ".rlcd-txn/upload");
    path_join(backup, root, ".rlcd-txn/backup");
    if (!storage_validate_file(relative, temp)) { errno = EINVAL; return -1; }
    int e = exists(dest);
    if (e < 0 || (e && rename(dest, backup))) return -1;
    if (rename(temp, dest)) return -1;
    return storage_txn_recover(root);
}
