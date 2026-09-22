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
} rtc_mgr_status_t;

/* Start after sensors_start() provides the shared I2C bus and after
 * sntp_mgr_start(), but before display rendering begins. */
bool rtc_mgr_start(void);
/* Nonblocking notification from the lwIP SNTP callback. */
void rtc_mgr_on_sync(int64_t utc_sec);
rtc_mgr_status_t rtc_mgr_status(void);
#endif
