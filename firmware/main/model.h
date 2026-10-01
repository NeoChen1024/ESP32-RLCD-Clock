#ifndef RLCD_FW_MODEL_H
#define RLCD_FW_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "leap_table.h"
#include "time_model.h"
#include "tz_rule.h"

/*
 * Device-side platform glue for the shared time model.
 *
 * time_model_now() fills clock_model_t from trusted SNTP/RTC-seeded time,
 * Wi-Fi and sensor state, the effective POSIX TZ rule and the TAI−UTC table.
 * The TZ rule and leap table are RAM state, copied in by config/leap loaders
 * or the CLI and read by the render task under a short critical section.
 *
 * time_model.c itself is the shared pure-computation core, compiled verbatim
 * from common/ by both host and target (see docs/architecture.md).
 */

/* Default when no usable config selects a rule: UTC+8, no daylight time. */
#define MODEL_TZ_DEFAULT "<+08>-8"

/* Clear the CLI override and use the selected config rule (or the default). */
void model_tz_set_default(void);
/* NULL selects MODEL_TZ_DEFAULT. */
void model_tz_set_config(const tz_rule_t *rule);
/* RAM-only CLI override until reset or reboot. */
void model_tz_set(const tz_rule_t *rule);
/* Effective rule; *cli_override may be NULL. */
void model_tz_get(tz_rule_t *out, bool *cli_override);

/* Install a verified table from volume ("sd"/"flash"), or NULL for the
 * built-in TAI−UTC value. */
void model_leap_set(const leap_table_t *table, const char *volume);

typedef struct {
    bool loaded;            /* false: built-in TAI_MINUS_UTC_BUILTIN_S */
    char volume[8];
    int64_t updated_unix_s;
    int64_t expires_unix_s;
} model_leap_info_t;

void model_leap_info(model_leap_info_t *out);
int model_tai_minus_utc(int64_t unix_s);

#endif
