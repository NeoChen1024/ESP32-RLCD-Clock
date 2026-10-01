#include "http_audio.h"
#include "audio_mgr.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int http_audio_status_json(char *out, size_t size)
{
    /* Managed paths are restricted to JSON-safe ASCII. */
    audio_status_t a;
    audio_mgr_status(&a);
    bool playing = a.state == AUDIO_PLAYING;
    return snprintf(out, size,
                    "{\"available\":%s,\"state\":\"%s\",\"file\":\"%s%s%s\",\"format\":\"%s\","
                    "\"source_bits\":%u,\"sample_rate\":%lu,\"channels\":%u,\"loop\":%s,"
                    "\"loops\":%lu,\"position_ms\":%lu,\"elapsed_ms\":%lu,\"duration_ms\":%lu,"
                    "\"loop_limit_ms\":%lu,\"volume\":%d,\"volume_source\":\"%s\","
                    "\"underruns\":%lu,\"error\":\"%s\"}",
                    a.available ? "true" : "false", playing ? "playing" : "idle",
                    playing ? a.volume_name : "", playing ? "/" : "", playing ? a.relative : "",
                    playing ? a.format : "", playing ? a.source_bits : 0,
                    playing ? (unsigned long)a.sample_rate : 0UL, playing ? a.channels : 0,
                    playing && a.loop ? "true" : "false", (unsigned long)a.loops,
                    (unsigned long)a.position_ms, (unsigned long)a.elapsed_ms,
                    (unsigned long)a.duration_ms, (unsigned long)AUDIO_LOOP_LIMIT_MS,
                    a.volume, a.volume_override ? "cli" : "config",
                    (unsigned long)a.underruns, a.last_error);
}

static esp_err_t send_status(httpd_req_t *req, const char *status)
{
    char body[512];
    http_audio_status_json(body, sizeof body);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *message)
{
    char body[160];
    snprintf(body, sizeof body, "{\"error\":\"%s\"}", message);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* Small JSON request body; an empty body yields an empty object. */
static cJSON *read_json(httpd_req_t *req, const char **error)
{
    char body[384];
    if (req->content_len >= sizeof body) { *error = "request body too large"; return NULL; }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) { *error = "request body interrupted"; return NULL; }
        got += (size_t)n;
    }
    body[got] = 0;
    cJSON *json = got ? cJSON_ParseWithLength(body, got) : cJSON_CreateObject();
    if (!cJSON_IsObject(json)) { cJSON_Delete(json); *error = "expected a JSON object"; return NULL; }
    return json;
}

static esp_err_t get_status(httpd_req_t *req) { return send_status(req, "200 OK"); }

static esp_err_t post_play(httpd_req_t *req)
{
    const char *error = NULL;
    cJSON *json = read_json(req, &error);
    if (!json) return fail(req, "400 Bad Request", error);
    cJSON *file = cJSON_GetObjectItemCaseSensitive(json, "file");
    cJSON *loop = cJSON_GetObjectItemCaseSensitive(json, "loop");
    if (!cJSON_IsString(file) || (loop && !cJSON_IsBool(loop))) {
        cJSON_Delete(json);
        return fail(req, "400 Bad Request", "need file (string) and optional loop (bool)");
    }
    char relative[STORAGE_REL_MAX];
    const char *name = file->valuestring;
    snprintf(relative, sizeof relative, "%s%s", strncmp(name, "sounds/", 7) ? "sounds/" : "", name);
    bool ok = audio_mgr_play(relative, cJSON_IsTrue(loop));
    cJSON_Delete(json);
    if (!ok) return fail(req, "400 Bad Request", "invalid sounds/ path or audio unavailable");
    /* Accepted: open/format errors appear in GET /audio once processed. */
    return send_status(req, "202 Accepted");
}

static esp_err_t post_stop(httpd_req_t *req)
{
    audio_mgr_stop();
    return send_status(req, "200 OK");
}

static esp_err_t post_volume(httpd_req_t *req)
{
    const char *error = NULL;
    cJSON *json = read_json(req, &error);
    if (!json) return fail(req, "400 Bad Request", error);
    cJSON *level = cJSON_GetObjectItemCaseSensitive(json, "level");
    cJSON *reset = cJSON_GetObjectItemCaseSensitive(json, "reset");
    bool ok = true;
    if (cJSON_IsTrue(reset) && !level) audio_mgr_reset_volume();
    else if (cJSON_IsNumber(level) && !reset && level->valuedouble >= 0 && level->valuedouble <= 100 &&
             level->valuedouble == (int)level->valuedouble) audio_mgr_set_volume((int)level->valuedouble);
    else ok = false;
    cJSON_Delete(json);
    if (!ok) return fail(req, "400 Bad Request", "use {\\\"level\\\": 0..100} or {\\\"reset\\\": true}");
    return send_status(req, "200 OK");
}

bool http_audio_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        { .uri = "/audio",        .method = HTTP_GET,  .handler = get_status },
        { .uri = "/audio/play",   .method = HTTP_POST, .handler = post_play },
        { .uri = "/audio/stop",   .method = HTTP_POST, .handler = post_stop },
        { .uri = "/audio/volume", .method = HTTP_POST, .handler = post_volume },
    };
    for (unsigned i = 0; i < sizeof routes / sizeof routes[0]; ++i)
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK) return false;
    return true;
}
