/*
 * Byte-level tests for the shared frame exporters (frame_export.{h,c}).
 *
 * The purpose is to lock the on-disk format contract that host and ESP32
 * target must share: P4 PBM (MSB-first, row-major, 1 = ink) and 1-bit
 * top-down BMP. Any change to the byte layout is a breaking change.
 */

#include "frame_export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

/* Build a vertical_top_lsb u8g2 buffer (byte at (y/8)*w + x, bit0 = top). */
static void put_pixel(uint8_t *fb, int w, int x, int y, int ink)
{
    if (ink) fb[(y / 8) * w + x] |= (uint8_t)(1u << (y & 7));
    else     fb[(y / 8) * w + x] &= (uint8_t)~(1u << (y & 7));
}

/* Encode into a caller buffer; returns bytes written (0 on failure). */
static size_t encode_pbm(uint8_t *dst, size_t cap, const uint8_t *fb, int w, int h)
{
    FILE *f = fmemopen(dst, cap, "wb");
    if (!f) return 0;
    bool ok = frame_export_pbm(f, fb, w, h);
    size_t n = (size_t)ftell(f);
    fclose(f);
    return ok ? n : 0;
}

static size_t encode_bmp(uint8_t *dst, size_t cap, const uint8_t *fb, int w, int h)
{
    FILE *f = fmemopen(dst, cap, "wb");
    if (!f) return 0;
    bool ok = frame_export_bmp(f, fb, w, h);
    size_t n = (size_t)ftell(f);
    fclose(f);
    return ok ? n : 0;
}

static void test_pbm_layout(void)
{
    /* w=10, h=16: 2 output bytes per row; buffer has 2 row groups (20 bytes). */
    enum { W = 10, H = 16 };
    uint8_t fb[(H / 8) * W];
    memset(fb, 0, sizeof fb);
    put_pixel(fb, W, 0, 0, 1);
    put_pixel(fb, W, 7, 0, 1);
    put_pixel(fb, W, 8, 1, 1);
    put_pixel(fb, W, 9, 1, 1);
    put_pixel(fb, W, 0, 15, 1);
    put_pixel(fb, W, 9, 15, 1);

    uint8_t out[64];
    size_t n = encode_pbm(out, sizeof out, fb, W, H);
    CHECK(n == 9 + H * 2);   /* "P4\n10 16\n" (9) + 16 rows x 2 bytes */

    static const uint8_t hdr[9] = { 'P', '4', '\n', '1', '0', ' ', '1', '6', '\n' };
    CHECK(memcmp(out, hdr, 9) == 0);

    /* row 0:  bits 0 and 7 set -> 0x81, 0x00 */
    CHECK(out[9 + 0] == 0x81 && out[9 + 1] == 0x00);
    /* row 1:  bits 8 and 9 set -> 0x00, 0xC0 */
    CHECK(out[9 + 2] == 0x00 && out[9 + 3] == 0xC0);
    /* row 15: bits 0 and 9 set -> 0x80, 0x40 */
    CHECK(out[9 + 30] == 0x80 && out[9 + 31] == 0x40);
}

static void test_bmp_layout(void)
{
    /* Same buffer as the PBM test. w=10 -> row_bytes=2, row_stride=4. */
    enum { W = 10, H = 16 };
    uint8_t fb[(H / 8) * W];
    memset(fb, 0, sizeof fb);
    put_pixel(fb, W, 0, 0, 1);
    put_pixel(fb, W, 7, 0, 1);
    put_pixel(fb, W, 8, 1, 1);
    put_pixel(fb, W, 9, 1, 1);
    put_pixel(fb, W, 0, 15, 1);
    put_pixel(fb, W, 9, 15, 1);

    uint8_t out[256];
    size_t n = encode_bmp(out, sizeof out, fb, W, H);
    uint32_t pixel_offset = 14 + 40 + 8;
    uint32_t row_stride = 4;
    CHECK(n == pixel_offset + row_stride * H);   /* 62 + 64 = 126 */

    /* BITMAPFILEHEADER */
    CHECK(out[0] == 'B' && out[1] == 'M');
    CHECK(out[2] == (n & 0xff) && out[3] == ((n >> 8) & 0xff));
    CHECK(out[10] == pixel_offset && out[11] == 0 && out[12] == 0 && out[13] == 0);

    /* BITMAPINFOHEADER */
    CHECK(out[14] == 40);
    CHECK(out[18] == W && out[19] == 0 && out[20] == 0 && out[21] == 0); /* width */
    CHECK(out[22] == (uint8_t)(-H) && out[23] == 0xff);                  /* -H top-down */
    CHECK(out[26] == 1 && out[27] == 0);                                 /* planes */
    CHECK(out[28] == 1 && out[29] == 0);                                 /* 1 bpp */

    /* Palette: white at index 0, black at index 1 */
    CHECK(out[54] == 0xff && out[55] == 0xff && out[56] == 0xff);
    CHECK(out[58] == 0x00 && out[59] == 0x00 && out[60] == 0x00);

    /* Pixel rows: MSB-first, padded to 4 bytes. */
    CHECK(out[62] == 0x81 && out[63] == 0x00 && out[64] == 0x00 && out[65] == 0x00);
    CHECK(out[66] == 0x00 && out[67] == 0xC0 && out[68] == 0x00 && out[69] == 0x00);
    CHECK(out[62 + 15 * 4] == 0x80 && out[62 + 15 * 4 + 1] == 0x40);
}

static void test_full_frame_size(void)
{
    /* Real panel geometry: 400x300 -> 50 bytes/row, 4-byte aligned already. */
    enum { W = 400, H = 300 };
    uint8_t fb[(H / 8) * W];
    memset(fb, 0, sizeof fb);

    uint8_t *out = malloc(64 + (size_t)((W + 7) / 8) * H);
    CHECK(out != NULL);
    size_t n = encode_pbm(out, 64 + (size_t)((W + 7) / 8) * H, fb, W, H);
    CHECK(n == 11 + (size_t)((W + 7) / 8) * H);   /* "P4\n400 300\n" + 15000 */
    CHECK(memcmp(out, "P4\n400 300\n", 11) == 0);

    n = encode_bmp(out, 64 + (size_t)((W + 7) / 8) * H, fb, W, H);
    uint32_t pixel_offset = 14 + 40 + 8;
    uint32_t stride = (uint32_t)(((W + 7) / 8 + 3) & ~3);
    CHECK(n == pixel_offset + stride * H);        /* 62 + 52*300 = 15662 */
    CHECK(out[0] == 'B' && out[1] == 'M');
    CHECK(out[18] == W % 256 && out[22] == (uint8_t)(-H));

    free(out);
}

int main(void)
{
    test_pbm_layout();
    test_bmp_layout();
    test_full_frame_size();

    if (failures) {
        fprintf(stderr, "test_frame_export: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_frame_export: all checks passed\n");
    return 0;
}
