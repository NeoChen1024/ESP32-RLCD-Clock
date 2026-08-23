#ifndef RLCD_FW_CLI_H
#define RLCD_FW_CLI_H

#include <stdbool.h>

/*
 * Serial CLI (esp_console) for the bring-up firmware.
 *
 * Commands are intentionally stateless where possible: Wi-Fi credentials and
 * settings are NOT persisted (no NVS storage of SSID/password) — this is an
 * early bring-up test firmware.
 *
 * arg parsing: esp_console_split_argv handles double-quoted args with
 * backslash escapes, so SSIDs containing spaces work:
 *     wifi connect "My SSID" mypassword
 */

/* Start the console REPL on the default UART (blocking, runs forever). */
void cli_start(void);

#endif
