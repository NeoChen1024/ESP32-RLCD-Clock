#include "snapshot.h"

#include <string.h>

#include "display.h"
#include "display_geometry.h"
#include "frame_export.h"
#include "render.h"
#include "u8g2.h"

/*
 * HTTP snapshot: lock the display framebuffer, draw the current face, then
 * export the panel's actual contents as PBM/BMP.
 *
 * Layout translation: the ST7305 u8g2 buffer is stored in the panel's native
 * memory orientation (300 wide x 400 tall, vertical_top_lsb, tile pitch 304
 * bytes), while the face is drawn in logical landscape 400x300 coordinates
 * via U8G2_R1. To keep exported snapshots byte-comparable with the host
 * simulator (which uses a 400x304 buffer), we re-map the physical buffer
 * into the host's 400x300 logical layout before encoding.
 */

static void remap_to_host_layout(u8g2_t *g, uint8_t *host_buf)
{
    const uint8_t *phys = u8g2_GetBufferPtr(g);
    int tile_w = u8g2_GetBufferTileWidth(g);       /* 38 -> 304 px pitch */
    int tile_h = u8g2_GetBufferTileHeight(g);      /* 50 */
    int pitch = tile_w * 8;                        /* bytes per 8-row group */

    /* Logical face: 400 wide x 300 tall (landscape). Physical panel memory:
     * 300 wide x 400 tall. U8G2_R1 maps logical (x,y) to physical
     * (px,py) = (299 - y, x). */
    memset(host_buf, 0, (size_t)DISP_W * BUF_H / 8);
    for (int y = 0; y < DISP_H; y++) {
        for (int x = 0; x < DISP_W; x++) {
            int px = 299 - y;
            int py = x;
            if (px < 0 || px >= 300 || py >= 400) continue;
            int bit = (phys[(py / 8) * pitch + px] >> (py & 7)) & 1;
            if (bit) host_buf[(y / 8) * DISP_W + x] |= (uint8_t)(1 << (y & 7));
        }
    }
}

static bool snapshot_write(FILE *out, bool bmp)
{
    if (!out) return false;
    u8g2_t *g = display_lock();
    if (!g) return false;

    render_frame(g);   /* draw in logical 400x300 landscape coords */

    /* Encode the *displayed* contents, re-mapped to the host layout. */
    static uint8_t host_buf[DISP_W * BUF_H / 8];
    remap_to_host_layout(g, host_buf);

    bool ok = bmp ? frame_export_bmp(out, host_buf, DISP_W, DISP_H)
                  : frame_export_pbm(out, host_buf, DISP_W, DISP_H);
    display_unlock();
    return ok;
}

bool snapshot_write_pbm(FILE *out) { return snapshot_write(out, false); }
bool snapshot_write_bmp(FILE *out) { return snapshot_write(out, true); }
