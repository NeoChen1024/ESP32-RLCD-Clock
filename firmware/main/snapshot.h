#ifndef RLCD_FW_SNAPSHOT_H
#define RLCD_FW_SNAPSHOT_H

#include <stdio.h>
#include <stdbool.h>

/*
 * Snapshot rendering: draws the current instrument state into the 400x300
 * u8g2 framebuffer (shared buffer, mutex-protected) and encodes it to PBM or
 * BMP via the shared frame_export module (same bytes as the host simulator).
 */

/* Render a fresh frame and write P4 PBM to `out`. */
bool snapshot_write_pbm(FILE *out);

/* Render a fresh frame and write 1-bit BMP to `out`. */
bool snapshot_write_bmp(FILE *out);

#endif
