#include "frame_export.h"

/* u8g2 vertical_top_lsb layout: byte at (y/8)*w + x, bit0 = top pixel. */
static int fb_pixel(const uint8_t *fb, int w, int x, int y)
{
    return (fb[(y / 8) * w + x] >> (y & 7)) & 1;
}

/* Pack one output row (MSB-first) into `row`; returns bytes written. */
static int pack_row(const uint8_t *fb, int w, int y, uint8_t *row)
{
    int n = 0;
    for (int x = 0; x < w; x += 8) {
        uint8_t b = 0;
        for (int i = 0; i < 8 && x + i < w; i++) {
            if (fb_pixel(fb, w, x + i, y)) b |= (uint8_t)(1 << (7 - i));
        }
        row[n++] = b;
    }
    return n;
}

bool frame_export_pbm(FILE *out, const uint8_t *fb, int w, int h)
{
    if (!out || !fb || w <= 0 || h <= 0) return false;
    if (fprintf(out, "P4\n%d %d\n", w, h) < 0) return false;
    uint8_t row[512];
    for (int y = 0; y < h; y++) {
        int n = pack_row(fb, w, y, row);
        if (n > (int)sizeof row) return false;
        if (fwrite(row, 1, (size_t)n, out) != (size_t)n) return false;
    }
    return true;
}

/* ---- BMP (1-bit, top-down) ---- */

static bool w8(FILE *out, uint8_t v)       { return fputc(v, out) != EOF; }
static bool w16(FILE *out, uint16_t v)
{
    return w8(out, (uint8_t)(v & 0xff)) && w8(out, (uint8_t)(v >> 8));
}
static bool w32(FILE *out, uint32_t v)
{
    return w8(out, (uint8_t)(v & 0xff)) &&
           w8(out, (uint8_t)((v >> 8) & 0xff)) &&
           w8(out, (uint8_t)((v >> 16) & 0xff)) &&
           w8(out, (uint8_t)((v >> 24) & 0xff));
}

bool frame_export_bmp(FILE *out, const uint8_t *fb, int w, int h)
{
    if (!out || !fb || w <= 0 || h <= 0) return false;

    int row_bytes = (w + 7) / 8;
    int row_stride = (row_bytes + 3) & ~3;      /* BMP rows pad to 4 bytes */
    uint32_t pixel_offset = 14 + 40 + 8;        /* file hdr + info hdr + palette */
    uint32_t pixel_size = (uint32_t)row_stride * (uint32_t)h;

    /* ---- BITMAPFILEHEADER (14 bytes) ---- */
    if (!w8(out, 'B') || !w8(out, 'M')) return false;
    if (!w32(out, pixel_offset + pixel_size)) return false;  /* bfSize */
    if (!w16(out, 0) || !w16(out, 0)) return false;          /* reserved */
    if (!w32(out, pixel_offset)) return false;               /* bfOffBits */

    /* ---- BITMAPINFOHEADER (40 bytes) ---- */
    if (!w32(out, 40)) return false;                         /* biSize */
    if (!w32(out, (uint32_t)w)) return false;                /* biWidth */
    if (!w32(out, (uint32_t)(-h))) return false;             /* biHeight: top-down */
    if (!w16(out, 1)) return false;                          /* biPlanes */
    if (!w16(out, 1)) return false;                          /* biBitCount = 1 */
    if (!w32(out, 0)) return false;                          /* biCompression = BI_RGB */
    if (!w32(out, pixel_size)) return false;                 /* biSizeImage */
    if (!w32(out, 2835) || !w32(out, 2835)) return false;    /* 72 DPI both axes */
    if (!w32(out, 2)) return false;                          /* biClrUsed = 2 */
    if (!w32(out, 2)) return false;                          /* biClrImportant */

    /* ---- Palette (8 bytes): index 0 = white/paper, 1 = black/ink ---- */
    static const uint8_t palette[8] = { 0xff, 0xff, 0xff, 0x00,   /* white  */
                                        0x00, 0x00, 0x00, 0x00 }; /* black  */
    if (fwrite(palette, 1, sizeof palette, out) != sizeof palette) return false;

    /* ---- Pixel data: MSB-first rows, 4-byte padded ---- */
    uint8_t row[512];
    for (int y = 0; y < h; y++) {
        int n = pack_row(fb, w, y, row);
        if (n > (int)sizeof row) return false;
        if (fwrite(row, 1, (size_t)n, out) != (size_t)n) return false;
        if (row_stride > n) {
            static const uint8_t pad[4] = { 0, 0, 0, 0 };
            if (fwrite(pad, 1, (size_t)(row_stride - n), out) !=
                (size_t)(row_stride - n)) return false;
        }
    }
    return true;
}
