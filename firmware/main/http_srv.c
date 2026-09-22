#include "http_srv.h"
#include "http_files.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "sntp_mgr.h"
#include "snapshot.h"
#include "display.h"
#include "wifi_mgr.h"

static const char *TAG = "http_srv";
extern const unsigned char home_html_start[] asm("_binary_home_html_start");
extern const unsigned char home_html_end[] asm("_binary_home_html_end");
extern const unsigned char web_style_css_start[] asm("_binary_web_style_css_start");
extern const unsigned char web_style_css_end[] asm("_binary_web_style_css_end");

/* ---- shared: snapshot streamed through fopencookie ---- */

typedef struct {
    httpd_req_t *req;
    bool ok;
} snap_stream_t;

static ssize_t snap_stream_write(void *cookie, const char *buf, size_t size)
{
    snap_stream_t *s = (snap_stream_t *)cookie;
    if (!s->ok) return -1;
    esp_err_t err = httpd_resp_send_chunk(s->req, buf, size);
    if (err != ESP_OK) {
        s->ok = false;
        return -1;
    }
    return (ssize_t)size;
}

static int snap_stream_close(void *cookie)
{
    snap_stream_t *s = (snap_stream_t *)cookie;
    /* Terminate the chunked response. */
    httpd_resp_send_chunk(s->req, NULL, 0);
    return 0;
}

static esp_err_t serve_snapshot(httpd_req_t *req, bool bmp)
{
    httpd_resp_set_type(req, bmp ? "image/bmp" : "image/x-portable-bitmap");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    snap_stream_t stream = { .req = req, .ok = true };
    cookie_io_functions_t io = {
        .write = snap_stream_write,
        .close = snap_stream_close,
    };
    FILE *f = fopencookie(&stream, "w", io);
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stream setup failed");
        return ESP_FAIL;
    }
    bool ok = bmp ? snapshot_write_bmp(f) : snapshot_write_pbm(f);
    fclose(f);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snapshot failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t handler_snapshot_pbm(httpd_req_t *req)
{
    return serve_snapshot(req, false);
}

static esp_err_t handler_snapshot_bmp(httpd_req_t *req)
{
    return serve_snapshot(req, true);
}

/* ---- /status: JSON ---- */

static esp_err_t handler_status(httpd_req_t *req)
{
    char body[512];
    int n = 0;

    wifi_mgr_status_t w = wifi_mgr_status();
    const char *wstate = w.state == WIFI_MGR_CONNECTED ? "connected" :
                         w.state == WIFI_MGR_CONNECTING ? "connecting" : "disconnected";

    sntp_mgr_status_t s = sntp_mgr_status();

    n += snprintf(body + n, sizeof body - (size_t)n,
                  "{\"wifi\":{\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d},",
                  wstate, w.ssid, w.ip, w.rssi_dbm);
    n += snprintf(body + n, sizeof body - (size_t)n,
                  "\"sntp\":{\"started\":%s,\"synced\":%s,\"trusted\":%s,"
                  "\"fresh\":%s,\"age_s\":%lu,\"source\":\"%s\",\"unix\":%lld},"
                  "\"display_frames\":%lu}",
                  s.started ? "true" : "false",
                  s.synced ? "true" : "false",
                  s.time_trusted ? "true" : "false",
                  s.fresh ? "true" : "false",
                  (unsigned long)s.ntp_age_s,
                  s.using_manual ? "manual" : s.using_config ? "config" :
                  s.using_dhcp ? "dhcp" : "fallback",
                  (long long)s.unix_sec, (unsigned long)display_frame_count());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t handler_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)home_html_start, home_html_end - home_html_start - 1);
}
static esp_err_t handler_style(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)web_style_css_start,
                           web_style_css_end - web_style_css_start - 1);
}

bool http_srv_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 12;

    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed (port busy?)");
        return false;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET, .handler = handler_index,
          .user_ctx = NULL },
        { .uri = "/status",       .method = HTTP_GET, .handler = handler_status,
          .user_ctx = NULL },
        { .uri = "/web_style.css", .method = HTTP_GET, .handler = handler_style,
          .user_ctx = NULL },
        { .uri = "/snapshot.pbm", .method = HTTP_GET, .handler = handler_snapshot_pbm,
          .user_ctx = NULL },
        { .uri = "/snapshot.bmp", .method = HTTP_GET, .handler = handler_snapshot_bmp,
          .user_ctx = NULL },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        if (httpd_register_uri_handler(server, &uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "failed to register %s", uris[i].uri);
            return false;
        }
    }
    if (!http_files_register(server)) { httpd_stop(server); return false; }
    ESP_LOGI(TAG, "HTTP server on :%d", cfg.server_port);
    return true;
}
