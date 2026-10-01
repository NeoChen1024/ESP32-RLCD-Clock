#ifndef RLCD_SENSOR_HEALTH_H
#define RLCD_SENSOR_HEALTH_H
#include <stdbool.h>
#include <stdint.h>

/* A one-off SHTC3 read failure may reuse the last sample. Ten consecutive
 * seconds without a good read make temperature and humidity unavailable. */
#define SENSOR_SHTC3_MAX_AGE_US (10LL * 1000000)

typedef struct {
    bool has_sample;
    int64_t last_good_us; /* monotonic timestamp, not wall time */
    float temp_c;
    float rh_pct;
} sensor_sample_cache_t;

void sensor_sample_cache_record(sensor_sample_cache_t *cache, int64_t now_us,
                                float temp_c, float rh_pct);
bool sensor_sample_cache_current(const sensor_sample_cache_t *cache, int64_t now_us,
                                 float *temp_c, float *rh_pct);
#endif
