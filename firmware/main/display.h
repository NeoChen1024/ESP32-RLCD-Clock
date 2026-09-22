#ifndef RLCD_FW_DISPLAY_H
#define RLCD_FW_DISPLAY_H

#include <stdbool.h>

#include "u8g2.h"
#include "u8g2_st7305.h"

/*
 * ST7305 display ownership.
 *
 * One u8g2 instance, backed by the real RLCD panel over SPI. A mutex guards
 * the framebuffer so the periodic display task and the HTTP snapshot handler
 * never draw concurrently.
 *
 * Geometry: the panel is 400x300 landscape (visible); the physical u8g2 full buffer is
 * 304x400 in panel orientation. snapshot.c remaps it to the host layout
 * before shared encoding.
 */

/* Initialize the panel. Call first; returns false on failure. */
bool display_start(void);

/* Start the 1 Hz refresh task (call after display_start). */
void display_task_start(void);

/* Completed panel refreshes (HTTP renders do not increment this counter). */
uint32_t display_frame_count(void);

/* Lock the framebuffer (blocking). Returns the u8g2 instance. */
u8g2_t *display_lock(void);

/* Unlock the framebuffer. */
void display_unlock(void);

#endif
