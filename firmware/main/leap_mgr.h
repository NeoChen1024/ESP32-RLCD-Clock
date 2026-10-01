#ifndef RLCD_LEAP_MGR_H
#define RLCD_LEAP_MGR_H
#include <stdbool.h>

/* Managed leap table file, relative to each storage volume root. */
#define LEAP_MGR_RELATIVE "time/leap-seconds.list"

/* Install the active volume's verified table in the model; without one,
 * revert to the built-in TAI−UTC. Storage owner holds the shared mutex. */
bool leap_mgr_reload_locked(void);
#endif
