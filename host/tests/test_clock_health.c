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
    puts("clock health and stepped-clock scheduling OK");
}
