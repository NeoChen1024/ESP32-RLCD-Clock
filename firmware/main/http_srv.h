#ifndef RLCD_FW_HTTP_SRV_H
#define RLCD_FW_HTTP_SRV_H

#include <stdbool.h>

/*
 * Read-only debug HTTP server (esp_http_server, port 80 when free):
 *   GET /            tiny HTML page embedding the BMP snapshot
 *   GET /status      JSON with Wi-Fi / SNTP / time state
 *   GET /snapshot.pbm  P4 PBM of the current framebuffer
 *   GET /snapshot.bmp  1-bit BMP of the current framebuffer
 * See design notes §8.5. Read-only by construction; config stays on the CLI.
 */

/* Start the HTTP server. Returns false if the port is taken. */
bool http_srv_start(void);

#endif
