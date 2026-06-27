#include "sdl3_backend.h"
#include "u8g2.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

static SDL_Window   *g_win = NULL;
static SDL_Renderer *g_ren = NULL;
static SDL_Texture  *g_tex = NULL;
static int           g_scale = 3;
static u8g2_t       *g_u8g2 = NULL;

/* ---------------------------------------------------------------------- */
/* u8g2 display callback + setup                                           */
/* ---------------------------------------------------------------------- */

static const u8x8_display_info_t u8x8_sdl3_400x304_info = {
    /* chip_enable_level    */ 0,
    /* chip_disable_level   */ 1,
    /* post_chip_enable_wait_ns */ 0,
    /* pre_chip_disable_wait_ns */ 0,
    /* reset_pulse_width_ms */ 0,
    /* post_reset_wait_ms   */ 0,
    /* sda_setup_time_ns    */ 0,
    /* sck_pulse_width_ns   */ 0,
    /* sck_clock_hz         */ 4000000UL,
    /* spi_mode             */ 1,
    /* i2c_bus_clock_100kHz */ 0,
    /* data_setup_time_ns   */ 0,
    /* write_pulse_width_ns */ 0,
    /* tile_width           */ DISP_W / 8,   /* 50 */
    /* tile_hight           */ BUF_H  / 8,   /* 38 */
    /* default_x_offset     */ 0,
    /* flipmode_x_offset    */ 0,
    /* pixel_width          */ DISP_W,
    /* pixel_height         */ BUF_H,
};

static uint8_t u8x8_d_sdl3_400x304(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)arg_int; (void)arg_ptr;
    switch (msg) {
    case U8X8_MSG_DISPLAY_SETUP_MEMORY:
        u8x8_d_helper_display_setup_memory(u8x8, &u8x8_sdl3_400x304_info);
        break;
    case U8X8_MSG_DISPLAY_INIT:
        u8x8_d_helper_display_init(u8x8);
        break;
    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
    case U8X8_MSG_DISPLAY_SET_FLIP_MODE:
    case U8X8_MSG_DISPLAY_SET_CONTRAST:
        break;
    case U8X8_MSG_DISPLAY_DRAW_TILE:
        /* Full-buffer mode: u8g2_SendBuffer writes tiles into the u8g2
         * buffer itself; we read that buffer directly on REFRESH. */
        break;
    case U8X8_MSG_DISPLAY_REFRESH:
        sdl3_present();
        break;
    default:
        return 0;
    }
    return 1;
}

static uint8_t u8x8_d_sdl3_gpio(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8; (void)msg; (void)arg_int; (void)arg_ptr;
    return 1;
}

void sdl3_backend_setup_u8g2(u8g2_t *u8g2)
{
    static uint8_t buf[DISP_W * BUF_H / 8];   /* 15200 bytes, full buffer */
    u8x8_Setup(u8g2_GetU8x8(u8g2),
               u8x8_d_sdl3_400x304,
               u8x8_cad_001,
               u8x8_byte_empty,                /* no-op: we never transfer real bytes */
               u8x8_d_sdl3_gpio);
    u8g2_SetupBuffer(u8g2, buf, BUF_H / 8, u8g2_ll_hvline_vertical_top_lsb, &u8g2_cb_r0);
    g_u8g2 = u8g2;
}

/* ---------------------------------------------------------------------- */
/* SDL3 lifecycle                                                          */
/* ---------------------------------------------------------------------- */

bool sdl3_backend_init(int scale)
{
    g_scale = scale > 0 ? scale : 3;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    if (!SDL_CreateWindowAndRenderer("RLCD Time Scale Monitor",
            DISP_W * g_scale, DISP_H * g_scale,
            SDL_WINDOW_HIGH_PIXEL_DENSITY, &g_win, &g_ren)) {
        fprintf(stderr, "SDL_CreateWindowAndRenderer failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(g_ren, 1);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_XRGB8888,
                              SDL_TEXTUREACCESS_STREAMING, DISP_W, DISP_H);
    if (!g_tex) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }
    /* Nearest-neighbor so the 1-bit framebuffer stays crisp on HiDPI. */
    SDL_SetTextureScaleMode(g_tex, SDL_SCALEMODE_NEAREST);

    int logical_w, logical_h;
    SDL_GetWindowSize(g_win, &logical_w, &logical_h);
    int pixel_w, pixel_h;
    SDL_GetWindowSizeInPixels(g_win, &pixel_w, &pixel_h);
    int out_w, out_h;
    SDL_GetCurrentRenderOutputSize(g_ren, &out_w, &out_h);
    printf("[sdl3] window logical: %dx%d, pixels: %dx%d, render output: %dx%d, scale: %d\n",
           logical_w, logical_h, pixel_w, pixel_h, out_w, out_h, g_scale);

    return true;
}

static const uint8_t *fb_ptr(void)
{
    return g_u8g2 ? (const uint8_t *)u8g2_GetBufferPtr(g_u8g2) : NULL;
}

/* u8g2 vertical_top_lsb layout: byte at (y/8)*W + x, bit0 = top pixel of 8. */
static int fb_pixel(const uint8_t *buf, int x, int y)
{
    return (buf[(y / 8) * DISP_W + x] >> (y & 7)) & 1;
}

void sdl3_present(void)
{
    if (!g_tex) return;
    const uint8_t *buf = fb_ptr();
    if (!buf) return;
    uint32_t *pixels;
    int pitch;
    if (!SDL_LockTexture(g_tex, NULL, (void **)&pixels, &pitch)) return;
    int pitch_px = pitch / 4;
    /* Reflective monochrome: set pixel = ink (black), clear = paper (white). */
    const uint32_t ink   = 0xFF000000;  /* black, opaque */
    const uint32_t paper = 0xFFFFFFFF;  /* white */
    for (int y = 0; y < DISP_H; y++) {
        for (int x = 0; x < DISP_W; x++) {
            pixels[y * pitch_px + x] = fb_pixel(buf, x, y) ? ink : paper;
        }
    }
    SDL_UnlockTexture(g_tex);

    /* Render into a centered, aspect-preserving 4:3 destination rectangle.
     * SDL_GetCurrentRenderOutputSize gives the real backbuffer pixels (which
     * on HiDPI differ from the window's logical size), so the framebuffer is
     * never stretched to a non-4:3 output. */
    int ow, oh;
    SDL_GetCurrentRenderOutputSize(g_ren, &ow, &oh);
    SDL_FRect dst;
    float ar = (float)DISP_W / (float)DISP_H;
    if ((float)ow / (float)oh > ar) {
        dst.h = (float)oh;
        dst.w = dst.h * ar;
        dst.x = ((float)ow - dst.w) * 0.5f;
        dst.y = 0.0f;
    } else {
        dst.w = (float)ow;
        dst.h = dst.w / ar;
        dst.x = 0.0f;
        dst.y = ((float)oh - dst.h) * 0.5f;
    }

    SDL_SetRenderDrawColor(g_ren, 32, 32, 32, 255);   /* letterbox bars */
    SDL_RenderClear(g_ren);
    SDL_RenderTexture(g_ren, g_tex, NULL, &dst);
    SDL_RenderPresent(g_ren);
}

bool sdl3_backend_pump_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) return false;
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) return false;
    }
    return true;
}

void sdl3_backend_shutdown(void)
{
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    SDL_Quit();
}

/* ---------------------------------------------------------------------- */
/* PBM export                                                              */
/* ---------------------------------------------------------------------- */

void sdl3_backend_save_pbm(const char *path)
{
    const uint8_t *buf = fb_ptr();
    if (!buf) { fprintf(stderr, "no framebuffer\n"); return; }
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P4\n%d %d\n", DISP_W, DISP_H);
    /* PBM: 1 = black(ink), 0 = white; MSB-first, row-major. */
    for (int y = 0; y < DISP_H; y++) {
        for (int x = 0; x < DISP_W; x += 8) {
            uint8_t b = 0;
            for (int i = 0; i < 8; i++) {
                if (fb_pixel(buf, x + i, y)) b |= (uint8_t)(1 << (7 - i));
            }
            fputc(b, f);
        }
    }
    fclose(f);
    printf("saved %s (%dx%d)\n", path, DISP_W, DISP_H);
}
