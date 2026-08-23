#ifndef RLCD_FRAME_EXPORT_H
#define RLCD_FRAME_EXPORT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Shared 1-bit framebuffer exporters (pure C; no SDL, no u8g2, no hardware
 * deps). Used identically by the host simulator and the ESP32 target so the
 * PBM/BMP output is byte-for-byte comparable across platforms (host vs target
 * screenshot diff).
 *
 * Buffer layout contract — u8g2 full buffer, vertical_top_lsb:
 *   byte at (y/8)*w + x holds 8 vertical pixels, bit 0 = top pixel of the
 *   group. This is what u8g2_SetupBuffer(..., u8g2_ll_hvline_vertical_top_lsb,
 *   ...) produces and what both backends present.
 *
 * Only the top `h` rows are exported; a padded buffer (e.g. 400x304 backing
 * a 400x300 visible area) passes h = visible height.
 */

/* Write P4 PBM: header "P4\n<w> <h>\n", then MSB-first, row-major,
 * 1 = ink (black). Returns false on write error or invalid args. */
bool frame_export_pbm(FILE *out, const uint8_t *fb, int w, int h);

/* Write 1-bit BMP, top-down (negative biHeight): 14-byte file header +
 * 40-byte info header + 8-byte palette (index 0 = white/paper, index 1 =
 * black/ink) + MSB-first rows padded to a 4-byte boundary. */
bool frame_export_bmp(FILE *out, const uint8_t *fb, int w, int h);

#endif
