/**
 * Procedural sky background driven by real weather.
 *
 * The backdrop is not a stock photo that merely resembles rain: it is
 * synthesized from the same numbers the card prints. Sun and moon altitude
 * come from your latitude, longitude and the actual time, cloud bands are
 * value-noise biased by the reported cloud cover, precipitation streaks are
 * tilted by the reported wind, and fog flattens contrast the way haze does.
 *
 * Everything is deterministic for a given (weather, time) pair, so the same
 * fetch always produces the same frame, and the frame changes every fetch.
 */
#pragma once

#include <stdint.h>

#include "glass_astro.h"
#include "glass_canvas.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Observer position, degrees. */
    float latitude;
    float longitude;
    /** UTC unix epoch of the observation. */
    int64_t epoch_utc;
    /** Local time offset in seconds, used for the clock only. */
    int32_t utc_offset_sec;

    /** WMO weather interpretation code (Open-Meteo). */
    int wmo_code;
    /** Sky scene name from the WMO table: clear/cloud/rain/snow/thunder/fog. */
    const char* scene;
    /** Cloud cover percent, 0..100. */
    int cloud_cover_pct;
    int humidity_pct;
    float wind_speed_kph;
    float wind_dir_deg;
    float precip_mm;
    bool is_day;

    /** Stable per-fetch seed; the cloud field drifts with this. */
    uint32_t seed;
} glass_sky_input_t;

/** Compute the seed used for a fetch: date + hour + code, order independent. */
uint32_t glass_sky_seed(int64_t epoch_utc, int wmo_code);

/** Render the backdrop into a canvas. Leaves the weight plane at "dither a lot". */
void glass_sky_render(glass_canvas_t* canvas, const glass_sky_input_t* in);

/** Exposed for the simulator's diagnostics. */
void glass_sky_bodies(const glass_sky_input_t* in, glass_sky_bodies_t* out);

#ifdef __cplusplus
}
#endif