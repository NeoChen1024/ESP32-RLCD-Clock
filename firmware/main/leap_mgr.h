#ifndef RLCD_LEAP_MGR_H
#define RLCD_LEAP_MGR_H
#include <stdbool.h>

/* Managed leap table file, relative to each storage volume root. */
#define LEAP_MGR_RELATIVE "time/leap-seconds.list"

/* Select the verified table with the latest "#$" update time (SD wins a
 * tie) and install it in the model; with none, revert to the built-in
 * TAI−UTC. Storage owner must already hold the shared mutex. */
bool leap_mgr_reload_locked(void);
#endif
