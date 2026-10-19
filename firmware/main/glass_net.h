/**
 * Open-Meteo client.
 *
 * Open-Meteo needs no key and no account, and `timezone=auto` makes the
 * response carry its own UTC offset, which is what the RTC gets synced from -
 * so this device does not need an NTP server either.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "weather_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Room for one forecast response; Open-Meteo sends about 1.5 KB for this. */
#define GLASS_JSON_CAPACITY 6144

/**
 * Fetch the current conditions and three forecast days.
 *
 * On success `json` holds the NUL-terminated body and `snap` is filled. A
 * false return leaves `snap` zeroed, which the caller renders as the stale
 * panel rather than a half-drawn one.
 */
bool glass_net_fetch(float latitude, float longitude, const char* timezone,
                     char* json, size_t json_capacity, weather_snapshot_t* snap);

#ifdef __cplusplus
}
#endif
