/**
 * Weather model: parse Open-Meteo, map WMO codes, format the card, schedule
 * the next fetch.
 *
 * This is the layer the device and the host simulator share, so a layout bug
 * shows up in a PNG before it shows up on the panel.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "glass_card.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GLASS_WEATHER_DAYS 3

typedef struct {
    char date[16];
    int wmo_code;
    float high_c;
    float low_c;
    int precip_prob_pct;
} weather_day_t;

typedef struct {
    /** Observation time in UTC seconds since the epoch. */
    int64_t observed_utc;
    float temp_c;
    float feels_c;
    float humidity_pct;
    float wind_kph;
    float wind_dir_deg;
    float uv_index;
    float precip_mm;
    int cloud_cover_pct;
    int wmo_code;
    bool api_is_day;
    int32_t utc_offset_sec;
    char timezone[32];
    weather_day_t days[GLASS_WEATHER_DAYS];
    /** True when the payload parsed well enough to display. */
    bool valid;
} weather_snapshot_t;

/**
 * WMO lookup: icon kind, sky scene, short label, precipitation word, and a
 * cloud-cover estimate for codes where the API did not supply one.
 * `cloud_pct` comes back as -1 when the caller's value should be kept.
 */
void weather_wmo(int code, const char** icon, const char** scene,
                 const char** label, const char** precip, int* cloud_pct);

/** Parse an Open-Meteo /v1/forecast response. */
bool weather_parse_open_meteo(const char* json, size_t len, weather_snapshot_t* out);

/** Day or night from the sun's real altitude, which is what the sky draws. */
bool weather_daylight(int64_t epoch_utc, float latitude, float longitude);

/** Fill a card from a snapshot. `city` may be NULL to use the timezone name. */
void weather_format_card(const weather_snapshot_t* snap, const char* city,
                         int64_t now_utc, float latitude, float longitude,
                         glass_card_data_t* out);

/**
 * Seconds until the next fetch: `day_gap` while the sun is up, `night_gap`
 * when it is down, clamped to at least 60 s.
 */
int64_t weather_next_fetch_delay_sec(const weather_snapshot_t* snap,
                                     int64_t now_utc, float latitude,
                                     float longitude, int64_t day_gap,
                                     int64_t night_gap);

/** UTC seconds for a "YYYY-MM-DDTHH:MM" local stamp minus an offset. */
int64_t weather_local_stamp_to_utc(const char* stamp, int32_t offset_sec);

/** Weekday name for a "YYYY-MM-DD" date, or "------" when unparseable. */
void weather_weekday_name(const char* date, const char* today, char* out,
                          size_t out_size);

#ifdef __cplusplus
}
#endif