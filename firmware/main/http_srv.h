#ifndef RLCD_FW_HTTP_SRV_H
#define RLCD_FW_HTTP_SRV_H

#include <stdbool.h>

/*
 * HTTP server (esp_http_server, port 80 when free):
 *   GET /            English live display page
 *   GET /files       file manager; /fs/... handles managed files
 *   GET /status      JSON with Wi-Fi / SNTP / time state
 *   GET /snapshot.pbm  P4 PBM of the current framebuffer
 *   GET /snapshot.bmp  1-bit BMP of the current framebuffer
 * Snapshot routes are read-only; file mutations use /fs/...
 */

/* Start the HTTP server. Returns false if the port is taken. */
bool http_srv_start(void);

#endif
