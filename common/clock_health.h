#ifndef RLCD_CLOCK_HEALTH_H
#define RLCD_CLOCK_HEALTH_H
#include <stdbool.h>
#include <stdint.h>

/* Product policy, not an accuracy guarantee. Default SNTP poll is one hour. */
#define CLOCK_FRESH_S (2U * 60U * 60U)
#define CLOCK_HOLDOVER_S (24U * 60U * 60U)
#define CLOCK_DHCP_TRIAL_US (18LL * 1000000)
#define CLOCK_DHCP_RETRY_US (5LL * 60 * 1000000)
/* Wall time outside [build, build + 10 years] means something is broken. */
#define CLOCK_PLAUSIBLE_SPAN_S (10LL * 36525 * 86400 / 100)
/* RTC_HOLD cross-check: system clock and PCF85063A may each drift; allow
 * the larger of 60 s or 50 ppm of the time since the last sync. */
#define CLOCK_RTC_TOLERANCE_MIN_S 60
#define CLOCK_RTC_TOLERANCE_PPM 50

/*
 * Time state, the single source for display masking and event scheduling:
 *   INVALID   no usable time this boot, or wall time outside the plausible
 *             build window, or (after 24 h) an RTC that fails its checks
 *   TRUSTED   synchronized within the last 24 h (an RTC boot counts when its
 *             persisted last-sync checkpoint is younger than 24 h)
 *   RTC_HOLD  older than 24 h, but the PCF85063A is running, valid and agrees
 *             with the system clock within the tolerance above
 */
typedef enum { CLOCK_INVALID = 0, CLOCK_TRUSTED, CLOCK_RTC_HOLD } clock_state_t;

typedef struct {
    bool ever_synced;
    bool rtc_hold;          /* booted from a valid RTC older than 24 h */
    bool source_synced;
    int64_t last_sync_us;
    int64_t source_since_us;
} clock_health_t;

void clock_health_select(clock_health_t *h, int64_t now_us);
void clock_health_sync(clock_health_t *h, int64_t now_us);
/* Seed from an RTC whose persisted last-sync age was validated at boot.
 * The source is not fresh until an actual SNTP callback occurs. */
bool clock_health_seed_rtc(clock_health_t *h, int64_t now_us, uint32_t age_s);
/* Seed from a valid RTC whose checkpoint is missing or older than 24 h:
 * usable only as RTC_HOLD while the RTC cross-check passes. */
void clock_health_seed_rtc_hold(clock_health_t *h);
uint32_t clock_health_age(const clock_health_t *h, int64_t now_us);
bool clock_health_trusted(const clock_health_t *h, int64_t now_us);
bool clock_health_fresh(const clock_health_t *h, int64_t now_us);
bool clock_health_dhcp_failed(const clock_health_t *h, int64_t now_us);
/* rtc_ok: the latest RTC cross-check passed. */
clock_state_t clock_health_state(const clock_health_t *h, int64_t now_us, int64_t wall_s, bool rtc_ok);
const char *clock_state_name(clock_state_t s);

/* Build time (UTC seconds) from RLCD_BUILD_EPOCH, set by the build system. */
int64_t clock_build_epoch(void);
bool clock_wall_plausible(int64_t wall_s, int64_t build_s);
/* since_sync_s: seconds since both clocks were last set from SNTP. */
bool clock_rtc_agrees(int64_t system_s, int64_t rtc_s, uint64_t since_sync_s);
/* Wall-clock boundary wait, bounded by monotonic time across clock steps.
 * Returns zero when the caller should render, otherwise milliseconds to wait. */
uint32_t clock_frame_wait_ms(int64_t wall_start_ms, int64_t mono_start_ms,
                             int64_t wall_now_ms, int64_t mono_now_ms);
#endif
