#ifndef RLCD_RTC_MGR_H
#define RLCD_RTC_MGR_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool present;
    bool oscillator_stopped;
    bool marker_valid;
    bool calendar_valid;
    bool anchor_valid;
    bool eligible;
    bool boot_used;
    int64_t utc_sec;
    int64_t last_sync_sec;
    /* Latest RTC_HOLD cross-check (clock_rtc_agrees). */
    bool hold_ok;
    int64_t hold_diff_s;        /* system minus RTC at that check */
    uint64_t hold_since_s;      /* seconds since the clocks were last aligned */
} rtc_mgr_status_t;

/* Start after sensors_start() provides the shared I2C bus and after
 * sntp_mgr_start(), but before display rendering begins. */
bool rtc_mgr_start(void);
/* Nonblocking notification from the lwIP SNTP callback. */
void rtc_mgr_on_sync(int64_t utc_sec);
rtc_mgr_status_t rtc_mgr_status(void);
/* The RTC is running, valid, inside the build window and agrees with the
 * system clock (checked every minute). Safe from any task. */
bool rtc_mgr_hold_ok(void);
#endif
