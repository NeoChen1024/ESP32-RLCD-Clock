#ifndef RLCD_FW_MODEL_H
#define RLCD_FW_MODEL_H

#include "time_model.h"

/*
 * Device-side platform glue for the shared time model.
 *
 * Provides the two platform hooks the shared code needs:
 *   - time_model_now(): fill clock_model_t from SNTP time + Wi-Fi state
 *   - tz_offset_minutes(): CLI-configurable offset (default UTC+8)
 *
 * time_model.c itself is the shared pure-computation core, compiled verbatim
 * from host/src/ (design notes §9).
 */

/* Clear CLI override and use the selected config offset (or UTC+8). */
void model_tz_set_default(void);
void model_tz_set_config(int minutes);

/* Set TZ offset in minutes east of UTC (e.g. +480 = UTC+8). */
void model_tz_set(int minutes);

/* Get current TZ offset in minutes. */
int model_tz_get(void);

#endif
