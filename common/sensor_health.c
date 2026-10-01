#include "sensor_health.h"

void sensor_sample_cache_record(sensor_sample_cache_t *cache, int64_t now_us,
                                float temp_c, float rh_pct)
{
    cache->has_sample = true;
    cache->last_good_us = now_us;
    cache->temp_c = temp_c;
    cache->rh_pct = rh_pct;
}

bool sensor_sample_cache_current(const sensor_sample_cache_t *cache, int64_t now_us,
                                 float *temp_c, float *rh_pct)
{
    if (!cache->has_sample || now_us < cache->last_good_us ||
        now_us - cache->last_good_us > SENSOR_SHTC3_MAX_AGE_US) return false;
    *temp_c = cache->temp_c;
    *rh_pct = cache->rh_pct;
    return true;
}
