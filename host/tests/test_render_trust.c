#include "sdl3_backend.h"
#include "render_faces.h"
#include "time_model.h"
#include "display_geometry.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int tz_offset_minutes(void) { return 480; }
int main(int argc, char **argv)
{
    u8g2_t g;
    sdl3_backend_setup_u8g2(&g);
    u8g2_InitDisplay(&g);
    clock_model_t m = {.sync = SYNC_BOOT_UNS, .temp_c = 28.4f, .rh_pct = 61, .batt_v = 3.91f};
    uint8_t first[DISP_W * BUF_H / 8];
    render_face(&g, &m);
    memcpy(first, u8g2_GetBufferPtr(&g), sizeof first);
    m.unix_ms = 1782055035000LL;
    render_face(&g, &m);
    /* Unsafe output must not leak any plausible date/time/ISO/GPS/MJD. */
    assert(!memcmp(first, u8g2_GetBufferPtr(&g), sizeof first));
    if (argc > 1) {
        u8g2_SendBuffer(&g);
        assert(sdl3_backend_save_png(argv[1]));
    }
    m.time_trusted = true;
    render_face(&g, &m);
    assert(memcmp(first, u8g2_GetBufferPtr(&g), sizeof first));
    puts("untrusted civil/UTC/ISO/MJD/GPS fields are masked");
}
