#ifndef RLCD_HTTP_AUDIO_H
#define RLCD_HTTP_AUDIO_H
#include <stdbool.h>
#include <stddef.h>
#include "esp_http_server.h"

/*
 * Playback control (no storage worker needed; requests only queue work):
 *   GET  /audio          status JSON
 *   POST /audio/play     {"file": "x.wav" | "sounds/x.wav", "storage": "sd"|"flash", "loop": bool}
 *   POST /audio/stop
 *   POST /audio/volume   {"level": 0..100} (RAM override) or {"reset": true}
 * Unauthenticated like the rest of the LAN API.
 */
bool http_audio_register(httpd_handle_t server);
/* The "audio" object for /status; returns snprintf's length. */
int http_audio_status_json(char *out, size_t size);
#endif
