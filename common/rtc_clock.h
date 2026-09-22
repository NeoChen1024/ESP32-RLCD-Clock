#ifndef RLCD_RTC_CLOCK_H
#define RLCD_RTC_CLOCK_H
#include <stdbool.h>
#include <stdint.h>

/* Offsets within the PCF85063A's contiguous time register block (0x04..0x0a).
 * This is the on-wire layout, not a C struct whose padding could vary. */
enum {
    RTC_CLOCK_SECONDS,
    RTC_CLOCK_MINUTES,
    RTC_CLOCK_HOURS,
    RTC_CLOCK_DAY,
    RTC_CLOCK_WEEKDAY,
    RTC_CLOCK_MONTH,
    RTC_CLOCK_YEAR,
    RTC_CLOCK_REGISTER_COUNT,
};
#define RTC_CLOCK_OS_FLAG 0x80u

/* UTC, 24-hour mode, years 2000..2099. Decode rejects the oscillator-stop
 * flag, invalid BCD/date and bad weekday. */
bool rtc_clock_decode(const uint8_t registers[RTC_CLOCK_REGISTER_COUNT], int64_t *utc_sec);
bool rtc_clock_encode(int64_t utc_sec, uint8_t registers[RTC_CLOCK_REGISTER_COUNT]);
/* A boot read is usable only within the existing 24 h holdover policy. */
bool rtc_clock_eligible(int64_t rtc_sec, int64_t last_sync_sec);
#endif
