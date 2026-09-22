#include "clock_health.h"

void clock_health_select(clock_health_t *h, int64_t now_us)
{
    h->source_synced = false;
    h->source_since_us = now_us;
}
void clock_health_sync(clock_health_t *h, int64_t now_us)
{
    h->ever_synced = h->source_synced = true;
    h->last_sync_us = now_us;
}
uint32_t clock_health_age(const clock_health_t *h, int64_t now_us)
{
    if (!h->ever_synced || now_us <= h->last_sync_us) return 0;
    int64_t age = (now_us - h->last_sync_us) / 1000000;
    return age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
}
bool clock_health_trusted(const clock_health_t *h, int64_t now_us)
{
    return h->ever_synced && clock_health_age(h, now_us) < CLOCK_HOLDOVER_S;
}
bool clock_health_fresh(const clock_health_t *h, int64_t now_us)
{
    return h->source_synced && clock_health_trusted(h, now_us) &&
           clock_health_age(h, now_us) < CLOCK_FRESH_S;
}
bool clock_health_dhcp_failed(const clock_health_t *h, int64_t now_us)
{
    int64_t since = h->source_synced ? h->last_sync_us : h->source_since_us;
    int64_t timeout = h->source_synced ? (int64_t)CLOCK_FRESH_S * 1000000 : CLOCK_DHCP_TRIAL_US;
    return now_us - since >= timeout;
}
uint32_t clock_frame_wait_ms(int64_t wall_start_ms, int64_t mono_start_ms,
                             int64_t wall_now_ms, int64_t mono_now_ms)
{
    int64_t elapsed = mono_now_ms - mono_start_ms;
    int64_t target = (wall_start_ms / 1000 + 1) * 1000;
    if (wall_now_ms >= target || wall_now_ms < wall_start_ms || elapsed >= 1000)
        return 0;
    int64_t wait = target - wall_now_ms;
    if (wait > 1000 - elapsed) wait = 1000 - elapsed;
    return (uint32_t)wait;
}
