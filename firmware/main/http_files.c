#include "http_files.h"
#include "storage_mgr.h"
#include "storage_files.h"
#include "config_mgr.h"
#include "cJSON.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static QueueHandle_t s_requests;
static SemaphoreHandle_t s_slots;
static TaskHandle_t s_worker;
extern const unsigned char files_html_start[] asm("_binary_files_html_start");
extern const unsigned char files_html_end[] asm("_binary_files_html_end");

static esp_err_t json_response(httpd_req_t *req, cJSON *json, const char *status)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}
static esp_err_t error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *j = cJSON_CreateObject();
    if (j) cJSON_AddStringToObject(j, "error", message);
    json_response(req, j, status);
    return ESP_FAIL; /* close this session, including any unread request body */
}
static esp_err_t success(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t volumes(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject(), *array = cJSON_AddArrayToObject(j, "volumes");
    const char *names[] = {"flash", "sd"};
    for (unsigned i = 0; i < 2; ++i) {
        cJSON *v = cJSON_CreateObject();
        cJSON_AddStringToObject(v, "name", names[i]);
        cJSON_AddStringToObject(v, "mount", storage_root(names[i]));
        cJSON_AddBoolToObject(v, "mounted", storage_mounted_locked(names[i]));
        uint64_t total, available;
        if (storage_space_locked(names[i], &total, &available)) {
            cJSON_AddNumberToObject(v, "total_bytes", (double)total);
            cJSON_AddNumberToObject(v, "free_bytes", (double)available);
        }
        cJSON_AddItemToArray(array, v);
    }
    return json_response(req, j, "200 OK");
}
static esp_err_t active_config(httpd_req_t *req)
{
    config_selection_t active;
    config_mgr_current_locked(&active);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "found", active.found);
    if (active.found) {
        cJSON_AddStringToObject(j, "volume", active.volume);
        cJSON_AddStringToObject(j, "path", active.path);
    }
    cJSON_AddNumberToObject(j, "tz_offset_minutes", active.tz_offset_minutes);
    cJSON_AddStringToObject(j, "ntp_server", active.ntp_server);
    return json_response(req, j, "200 OK");
}
static int name_compare(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static void add_entry(cJSON *entries, const char *name, bool dir, double size)
{
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "name", name);
    cJSON_AddStringToObject(entry, "type", dir ? "directory" : "file");
    cJSON_AddNumberToObject(entry, "size", size);
    cJSON_AddItemToArray(entries, entry);
}
static esp_err_t listing(httpd_req_t *req, const char *volume, const char *root, const char *relative)
{
    cJSON *j = cJSON_CreateObject(), *entries = cJSON_AddArrayToObject(j, "entries");
    cJSON_AddStringToObject(j, "volume", volume);
    cJSON_AddStringToObject(j, "path", relative);
    char path[STORAGE_PATH_MAX];
    if (!*relative) {
        add_entry(entries, "config", true, 0);
        add_entry(entries, "sounds", true, 0);
    } else {
        snprintf(path, sizeof path, "%s/%.*s", root, (int)strlen(relative) - 1, relative);
        DIR *dir = opendir(path);
        if (!dir && errno != ENOENT) { cJSON_Delete(j); return error(req, "500 Internal Server Error", "cannot list directory"); }
        if (dir) {
            struct dirent *ent;
            unsigned count = 0;
            bool truncated = false;
            bool oom = false;
            char **names = calloc(256, sizeof *names);
            if (!names) { closedir(dir); cJSON_Delete(j); return error(req, "500 Internal Server Error", "out of memory"); }
            while ((ent = readdir(dir))) {
                char rel[STORAGE_REL_MAX];
                int n = snprintf(rel, sizeof rel, "%s%s", relative, ent->d_name);
                if (n < 0 || (size_t)n >= sizeof rel || !storage_file_allowed(rel)) continue;
                snprintf(path, sizeof path, "%s/%s", root, rel);
                struct stat st;
                if (stat(path, &st) || !S_ISREG(st.st_mode)) continue;
                if (count < 256) {
                    char *copy = strdup(ent->d_name);
                    if (!copy) { oom = true; break; }
                    names[count++] = copy;
                }
                else {
                    truncated = true;
                    unsigned min = 0;
                    for (unsigned i = 1; i < count; ++i) if (strcmp(names[i], names[min]) < 0) min = i;
                    if (strcmp(ent->d_name, names[min]) > 0) {
                        char *copy = strdup(ent->d_name);
                        if (!copy) { oom = true; break; }
                        free(names[min]); names[min] = copy;
                    }
                }
            }
            closedir(dir);
            if (oom) {
                for (unsigned i = 0; i < count; ++i) free(names[i]);
                free(names); cJSON_Delete(j);
                return error(req, "500 Internal Server Error", "out of memory");
            }
            qsort(names, count, sizeof *names, name_compare);
            for (unsigned i = 0; i < count; ++i) {
                snprintf(path, sizeof path, "%s/%s%s", root, relative, names[i]);
                struct stat st;
                if (!stat(path, &st) && S_ISREG(st.st_mode)) add_entry(entries, names[i], false, (double)st.st_size);
                free(names[i]);
            }
            free(names);
            cJSON_AddBoolToObject(j, "truncated", truncated);
        }
    }
    return json_response(req, j, "200 OK");
}
static esp_err_t download(httpd_req_t *req, const char *path, const char *relative)
{
    FILE *f = fopen(path, "rb");
    if (!f) return error(req, errno == ENOENT ? "404 Not Found" : "500 Internal Server Error", "file unavailable");
    struct stat st;
    if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode)) { fclose(f); return error(req, "404 Not Found", "not a regular file"); }
    char disposition[STORAGE_REL_MAX + 40];
    const char *name = strrchr(relative, '/'); name = name ? name + 1 : relative;
    snprintf(disposition, sizeof disposition, "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_type(req, !strncmp(relative, "sounds/", 7) ? "audio/wav" : "application/json");
    char buffer[4096];
    esp_err_t result = ESP_OK;
    size_t n;
    int64_t deadline = esp_timer_get_time() + 120LL * 1000000;
    while ((n = fread(buffer, 1, sizeof buffer, f))) {
        if (esp_timer_get_time() >= deadline || httpd_resp_send_chunk(req, buffer, n) != ESP_OK) { result = ESP_FAIL; break; }
    }
    if (ferror(f)) result = ESP_FAIL;
    if (fclose(f)) result = ESP_FAIL;
    if (result == ESP_OK) result = httpd_resp_send_chunk(req, NULL, 0);
    return result;
}
static esp_err_t upload(httpd_req_t *req, const char *volume, const char *root, const char *relative)
{
    if (httpd_req_get_hdr_value_len(req, "Transfer-Encoding")) return error(req, "400 Bad Request", "use a Content-Length upload");
    if (!req->content_len || req->content_len > storage_file_limit(relative))
        return error(req, "413 Content Too Large", "JSON limit 16 KiB; WAV limit 16 MiB; empty files rejected");
    uint64_t total, available;
    if (!storage_space_locked(volume, &total, &available)) return error(req, "503 Service Unavailable", "cannot query storage");
    if (available < req->content_len + 65536ULL) return error(req, "507 Insufficient Storage", "need room for complete temporary upload and metadata");
    if (!strncmp(relative, "config/", 7)) {
        char destination[STORAGE_PATH_MAX];
        snprintf(destination, sizeof destination, "%s/%s", root, relative);
        struct stat existing;
        if (!stat(destination, &existing)) return error(req, "409 Conflict", "config versions cannot be overwritten");
        if (errno != ENOENT) return error(req, "500 Internal Server Error", "cannot check config version");
    }
    int fd = storage_txn_begin(root, relative);
    if (fd < 0) return error(req, "500 Internal Server Error", "cannot begin upload transaction");
    char buffer[4096];
    size_t remaining = req->content_len;
    bool ok = true;
    const char *fail_status = "500 Internal Server Error", *fail_text = "write failed";
    int64_t deadline = esp_timer_get_time() + 120LL * 1000000;
    while (remaining && ok) {
        if (esp_timer_get_time() >= deadline) { ok = false; fail_status = "408 Request Timeout"; fail_text = "upload deadline exceeded"; break; }
        int n = httpd_req_recv(req, buffer, remaining < sizeof buffer ? remaining : sizeof buffer);
        if (n <= 0) { ok = false; fail_status = "408 Request Timeout"; fail_text = "upload interrupted"; break; }
        ok = write(fd, buffer, n) == n;
        remaining -= n;
    }
    if (ok) ok = fsync(fd) == 0;
    if (close(fd)) ok = false;
    if (ok && !strncmp(relative, "config/", 7)) {
        char staged[STORAGE_PATH_MAX];
        snprintf(staged, sizeof staged, "%s/.rlcd-txn/upload", root);
        if (!config_mgr_file_valid(staged)) {
            ok = false; fail_status = "422 Unprocessable Content";
            fail_text = "invalid config: tz_offset_minutes must be -840..840, ntp_server must be a hostname or omitted";
        }
    }
    if (ok && storage_txn_commit(root)) {
        ok = false;
        if (errno == EINVAL) { fail_status = "422 Unprocessable Content"; fail_text = "expected a JSON object (depth <=16) or RIFF/WAVE file with matching length"; }
        else fail_text = "file replacement failed";
    }
    if (!ok) {
        if (storage_txn_recover(root)) fail_text = "transaction recovery required; data preserved for inspection";
        return error(req, fail_status, fail_text);
    }
    if (!strncmp(relative, "config/", 7)) config_mgr_reload_locked();
    return success(req);
}
static esp_err_t perform(httpd_req_t *req)
{
    if (!strcmp(req->uri, "/fs/") && req->method == HTTP_GET) return volumes(req);
    if (!strcmp(req->uri, "/fs/active") && req->method == HTTP_GET) return active_config(req);
    char volume[8], relative[STORAGE_REL_MAX];
    if (!storage_parse_uri(req->uri, volume, relative)) return error(req, "400 Bad Request", "invalid managed file path");
    if (!storage_mounted_locked(volume)) return error(req, "503 Service Unavailable", "volume is not mounted");
    const char *root = storage_root(volume);
    if (storage_txn_recover(root)) return error(req, "500 Internal Server Error", "transaction recovery required");
    if (storage_directory_allowed(relative)) {
        if (req->method != HTTP_GET) return error(req, "405 Method Not Allowed", "directories are read-only");
        return listing(req, volume, root, relative);
    }
    char path[STORAGE_PATH_MAX]; snprintf(path, sizeof path, "%s/%s", root, relative);
    if (req->method == HTTP_GET) return download(req, path, relative);
    if (req->method == HTTP_PUT) return upload(req, volume, root, relative);
    if (req->method == HTTP_DELETE) {
        if (unlink(path)) return error(req, errno == ENOENT ? "404 Not Found" : "500 Internal Server Error", "delete failed");
        if (!strncmp(relative, "config/", 7)) config_mgr_reload_locked();
        return success(req);
    }
    return error(req, "405 Method Not Allowed", "unsupported method");
}
static void file_worker(void *arg)
{
    (void)arg;
    httpd_req_t *req;
    for (;;) {
        if (xQueueReceive(s_requests, &req, portMAX_DELAY) != pdTRUE) continue;
        esp_err_t result;
        if (!storage_lock(1000)) result = error(req, "503 Service Unavailable", "storage busy");
        else { result = perform(req); storage_unlock(); }
        int fd = httpd_req_to_sockfd(req);
        httpd_handle_t server = req->handle;
        httpd_req_async_handler_complete(req);
        if (result != ESP_OK) httpd_sess_trigger_close(server, fd);
        xSemaphoreGive(s_slots);
    }
}
static esp_err_t enqueue(httpd_req_t *req)
{
    if (xSemaphoreTake(s_slots, 0) != pdTRUE) return error(req, "503 Service Unavailable", "file transfer queue full");
    httpd_req_t *copy;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        xSemaphoreGive(s_slots);
        return error(req, "503 Service Unavailable", "cannot allocate request");
    }
    if (xQueueSend(s_requests, &copy, 0) != pdTRUE) {
        error(copy, "503 Service Unavailable", "file transfer queue full");
        httpd_req_async_handler_complete(copy);
        xSemaphoreGive(s_slots);
        return ESP_FAIL;
    }
    return ESP_OK;
}
static esp_err_t page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)files_html_start, files_html_end - files_html_start - 1);
}
bool http_files_register(httpd_handle_t server)
{
    s_requests = xQueueCreate(2, sizeof(httpd_req_t *));
    s_slots = xSemaphoreCreateCounting(2, 2);
    if (!s_requests || !s_slots) goto fail;
    if (xTaskCreate(file_worker, "http_files", 12288, NULL, 4, &s_worker) != pdPASS) goto fail;
    const httpd_uri_t routes[] = {
        { .uri = "/files", .method = HTTP_GET, .handler = page },
        { .uri = "/fs/*", .method = HTTP_GET, .handler = enqueue },
        { .uri = "/fs/*", .method = HTTP_PUT, .handler = enqueue },
        { .uri = "/fs/*", .method = HTTP_DELETE, .handler = enqueue },
    };
    for (unsigned i = 0; i < sizeof routes / sizeof routes[0]; ++i)
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK) goto fail;
    return true;
fail:
    if (s_worker) vTaskDelete(s_worker);
    if (s_requests) vQueueDelete(s_requests);
    if (s_slots) vSemaphoreDelete(s_slots);
    return false;
}
