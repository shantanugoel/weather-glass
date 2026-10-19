/**
 * Sun and moon position from time and place.
 *
 * Low-precision ephemerides (Astronomical Almanac / Meeus truncated series):
 * good to a fraction of a degree for the sun and a few degrees for the moon,
 * which is far more accuracy than a decorative backdrop needs. Kept in the gfx
 * component so the host simulator and the device agree pixel for pixel.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Degrees above the horizon; negative means below. */
    float altitude_deg;
    /** Degrees clockwise from true north. */
    float azimuth_deg;
} glass_body_pos_t;

typedef struct {
    glass_body_pos_t sun;
    glass_body_pos_t moon;
    /** Illuminated fraction of the lunar disc, 0 = new, 1 = full. */
    float moon_phase;
    /** True when the moon is waxing (lit side drawn to the right). */
    bool moon_waxing;
    /** Civil twilight helper: sun altitude bands. */
    bool sun_up;
    bool civil_twilight;
    bool nautical_twilight;
} glass_sky_bodies_t;

/** Fill a bodies struct for a UTC unix epoch at a lat/lon in degrees. */
void glass_astro_compute(int64_t epoch_utc, float latitude, float longitude,
                         glass_sky_bodies_t* out);

/**
 * Grayscale luminance the sky would have for this sun altitude, ignoring
 * weather. Returns the zenith and horizon values used by the sky renderer.
 */
void glass_astro_sky_ramp(const glass_sky_bodies_t* bodies, int* zenith,
                          int* horizon);

#ifdef __cplusplus
}
#endif