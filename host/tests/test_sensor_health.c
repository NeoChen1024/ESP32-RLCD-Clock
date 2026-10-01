#include "sensor_health.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    sensor_sample_cache_t cache = {0};
    float temp = 0, rh = 0;
    assert(!sensor_sample_cache_current(&cache, 0, &temp, &rh));
    sensor_sample_cache_record(&cache, 1000000, 28.4f, 61.0f);
    assert(sensor_sample_cache_current(&cache, 1000000 + SENSOR_SHTC3_MAX_AGE_US,
                                       &temp, &rh));
    assert(temp == 28.4f && rh == 61.0f);
    assert(!sensor_sample_cache_current(&cache, 1000001 + SENSOR_SHTC3_MAX_AGE_US,
                                        &temp, &rh));
    assert(!sensor_sample_cache_current(&cache, 999999, &temp, &rh));
    sensor_sample_cache_record(&cache, 12000000, 29.1f, 60.0f);
    assert(sensor_sample_cache_current(&cache, 12000000, &temp, &rh));
    assert(temp == 29.1f && rh == 60.0f);
    puts("SHTC3 sample grace, expiry and recovery OK");
}
