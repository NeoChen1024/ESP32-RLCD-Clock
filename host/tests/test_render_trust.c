#include "sdl3_backend.h"
#include "render_faces.h"
#include "time_model.h"
#include "display_geometry.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(int argc, char **argv)
{
    u8g2_t g;
    sdl3_backend_setup_u8g2(&g);
    u8g2_InitDisplay(&g);
    clock_model_t m = {.sync = SYNC_BOOT_UNS, .temp_c = 28.4f, .rh_pct = 61,
                       .batt_v = 3.91f, .temp_humi_valid = true, .batt_valid = true,
                       .tz_offset_min = 480, .tai_minus_utc_s = TAI_MINUS_UTC_BUILTIN_S};
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
    memcpy(first, u8g2_GetBufferPtr(&g), sizeof first);
    m.temp_humi_valid = false;
    m.batt_valid = false;
    render_face(&g, &m);
    assert(memcmp(first, u8g2_GetBufferPtr(&g), sizeof first));
    memcpy(first, u8g2_GetBufferPtr(&g), sizeof first);
    /* Invalid raw values must not leak into the rendered telemetry. */
    m.temp_c = -999.0f; m.rh_pct = 999.0f; m.batt_v = 0.0f;
    render_face(&g, &m);
    assert(!memcmp(first, u8g2_GetBufferPtr(&g), sizeof first));
    puts("untrusted civil/UTC/ISO/MJD/GPS fields are masked");
}
