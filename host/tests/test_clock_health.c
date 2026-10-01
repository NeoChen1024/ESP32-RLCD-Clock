#include "clock_health.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    clock_health_t h = {0};
    assert(!clock_health_trusted(&h, 0));
    clock_health_select(&h, 1000000);
    assert(!clock_health_dhcp_failed(&h, 18999999));
    assert(clock_health_dhcp_failed(&h, 19000000));
    clock_health_sync(&h, 2000000);
    for (int i = 0; i < 100; ++i) {
        assert(clock_health_trusted(&h, 20000000));
        assert(clock_health_fresh(&h, 20000000));
        assert(!clock_health_dhcp_failed(&h, 20000000));
    }
    clock_health_select(&h, 21000000); /* resync preserves trusted holdover */
    assert(clock_health_trusted(&h, 22000000));
    assert(!clock_health_fresh(&h, 22000000));
    assert(clock_health_age(&h, 22000000) == 20);
    assert(clock_health_dhcp_failed(&h, 39000000));
    clock_health_sync(&h, 40000000);
    assert(clock_health_fresh(&h, 40000000 + (CLOCK_FRESH_S - 1LL) * 1000000));
    assert(!clock_health_fresh(&h, 40000000 + CLOCK_FRESH_S * 1000000LL));
    assert(clock_health_trusted(&h, 40000000 + (CLOCK_HOLDOVER_S - 1LL) * 1000000));
    assert(!clock_health_trusted(&h, 40000000 + CLOCK_HOLDOVER_S * 1000000LL));
    assert(clock_health_age(&h, INT64_MAX) == UINT32_MAX);
    clock_health_t rtc = {0};
    assert(!clock_health_seed_rtc(&rtc, 1000000, CLOCK_HOLDOVER_S));
    assert(clock_health_seed_rtc(&rtc, 1000000, CLOCK_HOLDOVER_S - 1));
    assert(!clock_health_fresh(&rtc, 1000000));
    assert(clock_health_trusted(&rtc, 1000000));
    assert(!clock_health_trusted(&rtc, 2000000));
    assert(!clock_health_seed_rtc(&rtc, 3000000, 0));
    clock_health_sync(&rtc, 4000000);
    assert(clock_health_fresh(&rtc, 4000000));
    /* Early wake, late wake, forward/backward steps, backward step that is
     * still above the start, and monotonic deadline under repeated slews. */
    assert(clock_frame_wait_ms(100020, 50, 100999, 1029) == 1);
    assert(clock_frame_wait_ms(100020, 50, 101001, 1031) == 0);
    assert(clock_frame_wait_ms(100020, 50, 160020, 100) == 0);
    assert(clock_frame_wait_ms(100020, 50, 40020, 100) == 0);
    assert(clock_frame_wait_ms(100020, 50, 100100, 1000) == 50);
    assert(clock_frame_wait_ms(100020, 50, 100200, 1050) == 0);
    /* Time state: build window, 24 h trust, RTC hold. */
    int64_t build = clock_build_epoch(), wall = build + 86400;
    assert(clock_wall_plausible(build, build) && !clock_wall_plausible(build - 1, build));
    assert(clock_wall_plausible(build + CLOCK_PLAUSIBLE_SPAN_S, build));
    assert(!clock_wall_plausible(build + CLOCK_PLAUSIBLE_SPAN_S + 1, build));
    assert(CLOCK_PLAUSIBLE_SPAN_S == 315576000LL);   /* 10 Julian years */
    clock_health_t st = {0};
    assert(clock_health_state(&st, 0, wall, true) == CLOCK_INVALID);    /* never set */
    clock_health_sync(&st, 1000000);
    assert(clock_health_state(&st, 1000000, wall, false) == CLOCK_TRUSTED);
    assert(clock_health_state(&st, 1000000, build - 1, true) == CLOCK_INVALID);
    assert(clock_health_state(&st, 1000000, build + CLOCK_PLAUSIBLE_SPAN_S + 1, true) == CLOCK_INVALID);
    int64_t stale = 1000000 + CLOCK_HOLDOVER_S * 1000000LL;
    assert(clock_health_state(&st, stale, wall, true) == CLOCK_RTC_HOLD);
    assert(clock_health_state(&st, stale, wall, false) == CLOCK_INVALID);
    clock_health_sync(&st, stale);
    assert(clock_health_state(&st, stale, wall, false) == CLOCK_TRUSTED);
    /* Booting from an RTC whose checkpoint is too old (or missing). */
    clock_health_t hold = {0};
    clock_health_seed_rtc_hold(&hold);
    assert(clock_health_state(&hold, 0, wall, true) == CLOCK_RTC_HOLD);
    assert(clock_health_state(&hold, 0, wall, false) == CLOCK_INVALID);
    clock_health_sync(&hold, 5000000);
    assert(clock_health_state(&hold, 5000000, wall, false) == CLOCK_TRUSTED);
    /* RTC agreement: 60 s floor, then 50 ppm of the time since sync. */
    assert(clock_rtc_agrees(wall, wall + 60, 0) && !clock_rtc_agrees(wall, wall + 61, 0));
    assert(clock_rtc_agrees(wall + 60, wall, 3600) && !clock_rtc_agrees(wall + 61, wall, 3600));
    uint64_t month = 30ULL * 86400;   /* 50 ppm of 30 days = 129.6 s */
    assert(clock_rtc_agrees(wall, wall + 129, month) && !clock_rtc_agrees(wall, wall + 130, month));
    puts("clock health, time state and stepped-clock scheduling OK");
}
