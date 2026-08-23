#include "sdl3_backend.h"
#include "time_model.h"
#include "render_faces.h"
#include "u8g2.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_HZ 15
#define FRAME_MS (1000 / FRAME_HZ)   /* ~66 ms */

typedef struct {
    sync_state_t sync;
    bool wifi_lost;
    bool low_battery;
} sim_controls_t;

static void usage(FILE *out, const char *program)
{
    fprintf(out,
            "Usage: %s [--scale N] [--pbm PATH] [--bmp PATH] [--png PATH]\n"
            "       %s --help\n",
            program, program);
}

static bool parse_scale(const char *text, int *scale)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || value < 1 || value > 8)
        return false;
    *scale = (int)value;
    return true;
}

static bool sync_is_trusted(sync_state_t sync)
{
    return sync == SYNC_NTP_OK || sync == SYNC_RTC_HOLD || sync == SYNC_WIFI_LOST;
}

static sync_state_t next_sync_state(sync_state_t sync)
{
    switch (sync) {
    case SYNC_BOOT_UNS: return SYNC_SYNCING;
    case SYNC_SYNCING:  return SYNC_NTP_OK;
    case SYNC_NTP_OK:   return SYNC_RTC_HOLD;
    case SYNC_RTC_HOLD:
    case SYNC_WIFI_LOST:
        return SYNC_BOOT_UNS;
    }
    return SYNC_BOOT_UNS;
}

static void apply_sim_controls(clock_model_t *model, const sim_controls_t *controls)
{
    model->sync = controls->wifi_lost ? SYNC_WIFI_LOST : controls->sync;
    model->time_trusted = sync_is_trusted(model->sync);
    model->wifi_rssi_dbm = controls->wifi_lost ? -100 : -57;
    model->batt_v = controls->low_battery ? 3.20f : 3.91f;
}

int main(int argc, char **argv)
{
    int scale = 3;
    const char *pbm_path = NULL;
    const char *bmp_path = NULL;
    const char *png_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(stdout, argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--scale") == 0) {
            if (i + 1 >= argc || !parse_scale(argv[++i], &scale)) {
                fprintf(stderr, "--scale expects an integer from 1 to 8\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--pbm") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--pbm expects a path\n");
                return 2;
            }
            pbm_path = argv[++i];
        } else if (strcmp(argv[i], "--bmp") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--bmp expects a path\n");
                return 2;
            }
            bmp_path = argv[++i];
        } else if (strcmp(argv[i], "--png") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--png expects a path\n");
                return 2;
            }
            png_path = argv[++i];
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(stderr, argv[0]);
            return 2;
        }
    }

    if (pbm_path || bmp_path || png_path) {
        /* Headless: render one frame and save, no SDL window. */
        u8g2_t u8g2;
        sdl3_backend_setup_u8g2(&u8g2);
        u8g2_InitDisplay(&u8g2);
        u8g2_SetPowerSave(&u8g2, 0);
        clock_model_t model;
        time_model_now(&model);
        render_face(&u8g2, &model);
        u8g2_SendBuffer(&u8g2);
        bool ok = true;
        if (pbm_path) ok &= sdl3_backend_save_pbm(pbm_path);
        if (bmp_path) ok &= sdl3_backend_save_bmp(bmp_path);
        if (png_path) ok &= sdl3_backend_save_png(png_path);
        return ok ? 0 : 1;
    }

    if (!sdl3_backend_init(scale)) {
        fprintf(stderr, "backend init failed\n");
        return 1;
    }

    u8g2_t u8g2;
    sdl3_backend_setup_u8g2(&u8g2);
    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0);
    u8g2_ClearBuffer(&u8g2);

    clock_model_t model;
    sim_controls_t controls = {
        .sync = SYNC_NTP_OK,
        .wifi_lost = false,
        .low_battery = false,
    };

    Uint64 perf_freq = SDL_GetPerformanceFrequency();

    for (;;) {
        Uint64 t0 = SDL_GetPerformanceCounter();

        /* ---- input ---- */
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) goto done;
            if (e.type == SDL_EVENT_KEY_DOWN) {
                switch (e.key.key) {
                case SDLK_ESCAPE: goto done;
                case SDLK_S:
                    (void)sdl3_backend_save_pbm("rlcd_screenshot.pbm");
                    break;
                case SDLK_N:
                    controls.sync = next_sync_state(controls.sync);
                    break;
                case SDLK_W:
                    controls.wifi_lost = !controls.wifi_lost;
                    break;
                case SDLK_B:
                    controls.low_battery = !controls.low_battery;
                    break;
                default: break;
                }
            }
        }

        /* ---- model + render ---- */
        time_model_now(&model);
        apply_sim_controls(&model, &controls);
        render_face(&u8g2, &model);
        u8g2_SendBuffer(&u8g2);

        /* ---- 15 Hz frame cap ---- */
        Uint64 t1 = SDL_GetPerformanceCounter();
        Sint64 elapsed_ms = (Sint64)((t1 - t0) * 1000 / (perf_freq ? perf_freq : 1));
        if (elapsed_ms < FRAME_MS) {
            SDL_Delay((Uint32)(FRAME_MS - elapsed_ms));
        }
    }

done:
    sdl3_backend_shutdown();
    return 0;
}
