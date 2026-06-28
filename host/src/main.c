#include "sdl3_backend.h"
#include "time_model.h"
#include "render_faces.h"
#include "u8g2.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define FRAME_HZ 15
#define FRAME_MS (1000 / FRAME_HZ)   /* ~66 ms */

int main(int argc, char **argv)
{
    int scale = 3;
    const char *pbm_path = NULL;
    const char *png_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--pbm") == 0 && i + 1 < argc) {
            pbm_path = argv[++i];
        } else if (strcmp(argv[i], "--png") == 0 && i + 1 < argc) {
            png_path = argv[++i];
        }
    }

    if (pbm_path || png_path) {
        /* Headless: render one frame and save, no SDL window. */
        u8g2_t u8g2;
        sdl3_backend_setup_u8g2(&u8g2);
        u8g2_InitDisplay(&u8g2);
        u8g2_SetPowerSave(&u8g2, 0);
        clock_model_t model;
        time_model_now(&model);
        render_face(&u8g2, &model);
        u8g2_SendBuffer(&u8g2);
        if (pbm_path) sdl3_backend_save_pbm(pbm_path);
        if (png_path) sdl3_backend_save_png(png_path);
        return 0;
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
                case SDLK_S: sdl3_backend_save_pbm("rlcd_screenshot.pbm"); break;
                default: break;
                }
            }
        }

        /* ---- model + render ---- */
        time_model_now(&model);
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
