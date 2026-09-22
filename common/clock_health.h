#ifndef RLCD_CLOCK_HEALTH_H
#define RLCD_CLOCK_HEALTH_H
#include <stdbool.h>
#include <stdint.h>

/* Product policy, not an accuracy guarantee. Default SNTP poll is one hour. */
#define CLOCK_FRESH_S (2U * 60U * 60U)
#define CLOCK_HOLDOVER_S (24U * 60U * 60U)
#define CLOCK_DHCP_TRIAL_US (18LL * 1000000)
#define CLOCK_DHCP_RETRY_US (5LL * 60 * 1000000)

typedef struct {
    bool ever_synced;
    bool source_synced;
    int64_t last_sync_us;
    int64_t source_since_us;
} clock_health_t;

void clock_health_select(clock_health_t *h, int64_t now_us);
void clock_health_sync(clock_health_t *h, int64_t now_us);
uint32_t clock_health_age(const clock_health_t *h, int64_t now_us);
bool clock_health_trusted(const clock_health_t *h, int64_t now_us);
bool clock_health_fresh(const clock_health_t *h, int64_t now_us);
bool clock_health_dhcp_failed(const clock_health_t *h, int64_t now_us);
/* Wall-clock boundary wait, bounded by monotonic time across clock steps.
 * Returns zero when the caller should render, otherwise milliseconds to wait. */
uint32_t clock_frame_wait_ms(int64_t wall_start_ms, int64_t mono_start_ms,
                             int64_t wall_now_ms, int64_t mono_now_ms);
#endif
