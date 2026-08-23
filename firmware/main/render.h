#ifndef RLCD_FW_RENDER_H
#define RLCD_FW_RENDER_H

#include "u8g2.h"

/*
 * Shared face renderer. Draws the current instrument state (Wi-Fi / time /
 * HTTP hints) into the given u8g2 framebuffer. Called by both the periodic
 * display task and the HTTP snapshot handler — identical draw calls, so the
 * exported snapshot is exactly what the panel shows.
 */

void render_frame(u8g2_t *g);

#endif
