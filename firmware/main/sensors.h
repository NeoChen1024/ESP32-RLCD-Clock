#ifndef RLCD_FW_SENSORS_H
#define RLCD_FW_SENSORS_H

#include <stdbool.h>
#include "driver/i2c_master.h"

/*
 * Sensor front-end: SHTC3 temp/humidity over I2C + battery voltage via ADC.
 * One I2C master bus is shared with the PCF85063A RTC.
 */

/* Initialize I2C bus + SHTC3 + battery ADC. Call once at boot. */
bool sensors_start(void);
/* Shared I2C bus for the SHTC3 and PCF85063A. NULL if bus init failed. */
i2c_master_bus_handle_t sensors_i2c_bus(void);

/* Read temperature [°C] and humidity [%RH]. Returns false on I2C failure
 * (e.g. sensor absent); outputs are unchanged on failure. */
bool sensors_read_temp_humi(float *temp_c, float *rh_pct);

/* Read battery voltage [V] (already scaled by the divider). */
float sensors_read_batt_v(void);

#endif
