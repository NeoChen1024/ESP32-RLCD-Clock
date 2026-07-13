#ifndef RLCD_HOST_DISPLAY_GEOMETRY_H
#define RLCD_HOST_DISPLAY_GEOMETRY_H

/* Shared renderer/backend geometry. The u8g2 buffer is tile-aligned while
 * only the top 300 rows are visible on the RLCD. */
#define DISP_W 400
#define DISP_H 300
#define BUF_H  304

#endif
