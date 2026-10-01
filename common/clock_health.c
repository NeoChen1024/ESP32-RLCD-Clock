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
void clock_health_seed_rtc_hold(clock_health_t *h)
{
    if (!h->ever_synced) h->rtc_hold = true;
}
bool clock_health_seed_rtc(clock_health_t *h, int64_t now_us, uint32_t age_s)
{
    if (h->ever_synced || age_s >= CLOCK_HOLDOVER_S) return false;
    h->ever_synced = true;
    h->source_synced = false;
    h->last_sync_us = now_us - (int64_t)age_s * 1000000;
    return true;
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
clock_state_t clock_health_state(const clock_health_t *h, int64_t now_us, int64_t wall_s, bool rtc_ok)
{
    if (!clock_wall_plausible(wall_s, clock_build_epoch())) return CLOCK_INVALID;
    if (clock_health_trusted(h, now_us)) return CLOCK_TRUSTED;
    return (h->ever_synced || h->rtc_hold) && rtc_ok ? CLOCK_RTC_HOLD : CLOCK_INVALID;
}
const char *clock_state_name(clock_state_t s)
{
    return s == CLOCK_TRUSTED ? "trusted" : s == CLOCK_RTC_HOLD ? "rtc_hold" : "invalid";
}

#ifndef RLCD_BUILD_EPOCH
#error "RLCD_BUILD_EPOCH (UTC seconds) must be defined by the build"
#endif
int64_t clock_build_epoch(void) { return (int64_t)RLCD_BUILD_EPOCH; }
bool clock_wall_plausible(int64_t wall_s, int64_t build_s)
{
    return wall_s >= build_s && wall_s - build_s <= CLOCK_PLAUSIBLE_SPAN_S;
}
bool clock_rtc_agrees(int64_t system_s, int64_t rtc_s, uint64_t since_sync_s)
{
    uint64_t diff = system_s > rtc_s ? (uint64_t)(system_s - rtc_s) : (uint64_t)(rtc_s - system_s);
    uint64_t allowed = since_sync_s / (1000000 / CLOCK_RTC_TOLERANCE_PPM);
    if (allowed < CLOCK_RTC_TOLERANCE_MIN_S) allowed = CLOCK_RTC_TOLERANCE_MIN_S;
    return diff <= allowed;
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
