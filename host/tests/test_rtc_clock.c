#include "rtc_clock.h"
#include "clock_health.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void roundtrip(int64_t epoch)
{
    uint8_t registers[RTC_CLOCK_REGISTER_COUNT];
    int64_t decoded = 0;
    assert(rtc_clock_encode(epoch, registers));
    assert(rtc_clock_decode(registers, &decoded));
    assert(decoded == epoch);
}
int main(void)
{
    roundtrip(946684800);  /* Saturday, 2000-01-01 */
    roundtrip(1709164799); /* 2024-02-28 23:59:59 */
    roundtrip(1709164800); /* leap day */
    roundtrip(4102444799); /* last second of 2099 */
    uint8_t r[RTC_CLOCK_REGISTER_COUNT]; int64_t epoch;
    assert(!rtc_clock_encode(4102444800LL, r));
    assert(rtc_clock_encode(1709164800, r));
    r[RTC_CLOCK_SECONDS] |= RTC_CLOCK_OS_FLAG;
    assert(!rtc_clock_decode(r, &epoch));
    r[RTC_CLOCK_SECONDS] &= ~RTC_CLOCK_OS_FLAG;
    r[RTC_CLOCK_MONTH] = 0x13; assert(!rtc_clock_decode(r, &epoch));
    r[RTC_CLOCK_MONTH] = 0x02;
    r[RTC_CLOCK_DAY] = 0x30; assert(!rtc_clock_decode(r, &epoch));
    r[RTC_CLOCK_DAY] = 0x29;
    r[RTC_CLOCK_WEEKDAY] = 0x07; assert(!rtc_clock_decode(r, &epoch));
    assert(!rtc_clock_eligible(1709164800, 0));
    assert(!rtc_clock_eligible(1709164800, 1709164801));
    assert(rtc_clock_eligible(1709164800, 1709164800 - CLOCK_HOLDOVER_S + 1));
    assert(!rtc_clock_eligible(1709164800, 1709164800 - CLOCK_HOLDOVER_S));
    puts("PCF85063 calendar and 24-hour boot trust policy OK");
}
