#ifndef RLCD_FW_SENSORS_H
#define RLCD_FW_SENSORS_H

#include <stdbool.h>

/*
 * Sensor front-end: SHTC3 temp/humidity over I2C + battery voltage via ADC.
 * One I2C master bus is shared with future peripherals (PCF85063 RTC).
 */

/* Initialize I2C bus + SHTC3 + battery ADC. Call once at boot. */
bool sensors_start(void);

/* Read temperature [°C] and humidity [%RH]. Returns false on I2C failure
 * (e.g. sensor absent); outputs are unchanged on failure. */
bool sensors_read_temp_humi(float *temp_c, float *rh_pct);

/* Read battery voltage [V] (already scaled by the divider). */
float sensors_read_batt_v(void);

#endif
