#ifndef RLCD_HOST_SDL3_BACKEND_H
#define RLCD_HOST_SDL3_BACKEND_H

#include "u8g2.h"
#include <stdbool.h>

/*
 * SDL3 display backend for u8g2.
 *
 * u8g2 is the only drawing engine; SDL3 only presents the final 1-bit
 * framebuffer. Visible area is 400x300; the internal u8g2 buffer is padded
 * to 400x304 (38 tile rows) because 300 is not a multiple of 8.
 */

#define DISP_W 400
#define DISP_H 300
#define BUF_H  304          /* tile_height = 38, padded */

/* Initialize SDL3 window/renderer/texture. Call once before u8g2 setup. */
bool sdl3_backend_init(int scale);

/* Shutdown SDL3. */
void sdl3_backend_shutdown(void);

/* Configure a u8g2_t to use the SDL3 backend (full-buffer, no rotation). */
void sdl3_backend_setup_u8g2(u8g2_t *u8g2);

/* Copy the u8g2 framebuffer to the SDL texture and present. Called by the
 * u8g2 display callback on U8X8_MSG_DISPLAY_REFRESH (i.e. after SendBuffer). */
void sdl3_present(void);

/* Pump SDL events. Returns false if the window was closed / ESC pressed. */
bool sdl3_backend_pump_events(void);

/* Export the current visible framebuffer to a PBM file. */
void sdl3_backend_save_pbm(const char *path);

#endif
