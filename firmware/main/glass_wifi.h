/**
 * WiFi station, just enough of it for one fetch.
 *
 * The panel is refreshed every 20 minutes by day and every 2 hours by night,
 * and the radio is off in between, so this is a connect-once helper rather than
 * a resilient long-lived client: connect, or come back saying it could not.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bring up the radio and join the configured AP. Returns false on timeout. */
bool glass_wifi_start(uint32_t connect_timeout_ms);

/** True once an address has been obtained (and still held). */
bool glass_wifi_connected(void);

/** Take the radio back down before deep sleep. */
void glass_wifi_stop(void);

#ifdef __cplusplus
}
#endif
