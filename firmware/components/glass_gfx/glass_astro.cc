#include "glass_astro.h"

#include <math.h>

namespace {

constexpr double kRad = 0.017453292519943295;
constexpr double kDeg = 57.29577951308232;

double norm360(double a) {
    a = fmod(a, 360.0);
    return a < 0.0 ? a + 360.0 : a;
}

// Grayscale sky luminance stops, keyed on sun altitude in degrees. A blue sky
// photographs as mid gray while haze near the horizon reads much lighter, so
// the horizon value is always the brighter one.
struct Stop {
    int alt;
    int zenith;
    int horizon;
};
const Stop kStops[] = {
    {-90, 6,   16},
    {-18, 8,   22},
    {-12, 12,  34},
    {-6,  26,  74},
    {-3,  48,  122},
    {0,   66,  166},
    {4,   86,  206},
    {10,  92,  178},
    {25,  100, 172},
    {45,  108, 178},
    {80,  116, 188},
};

int lerp_stop(double alt, bool horizon) {
    const int n = static_cast<int>(sizeof(kStops) / sizeof(kStops[0]));
    if (alt <= kStops[0].alt) return horizon ? kStops[0].horizon : kStops[0].zenith;
    if (alt >= kStops[n - 1].alt) {
        return horizon ? kStops[n - 1].horizon : kStops[n - 1].zenith;
    }
    for (int i = 0; i + 1 < n; ++i) {
        if (alt >= kStops[i].alt && alt <= kStops[i + 1].alt) {
            const double t = (alt - kStops[i].alt) / (kStops[i + 1].alt - kStops[i].alt);
            const int a = horizon ? kStops[i].horizon : kStops[i].zenith;
            const int b = horizon ? kStops[i + 1].horizon : kStops[i + 1].zenith;
            return static_cast<int>(a + (b - a) * t + 0.5);
        }
    }
    return 128;
}

}  // namespace

extern "C" void glass_astro_compute(int64_t epoch_utc, float latitude,
                                    float longitude, glass_sky_bodies_t* out) {
    const double n = static_cast<double>(epoch_utc) / 86400.0 + 2440587.5 - 2451545.0;
    const double phi = latitude * kRad;
    const double eps = (23.439 - 0.0000004 * n) * kRad;
    const double gmst = 280.46061837 + 360.98564736629 * n;
    const double lst = (gmst + longitude) * kRad;

    // --- Sun: mean longitude + equation of centre, then equatorial ---
    const double sun_mean = 280.460 + 0.9856474 * n;
    const double sun_anom = (357.528 + 0.9856003 * n) * kRad;
    const double sun_ecl =
        norm360(sun_mean + 1.915 * sin(sun_anom) + 0.020 * sin(2.0 * sun_anom));
    const double sun_lambda = sun_ecl * kRad;
    const double sun_ra = atan2(cos(eps) * sin(sun_lambda), cos(sun_lambda));
    const double sun_dec = asin(sin(eps) * sin(sun_lambda));

    const double sun_ha = lst - sun_ra;
    const double sun_alt =
        asin(sin(phi) * sin(sun_dec) + cos(phi) * cos(sun_dec) * cos(sun_ha));
    // Azimuth from south, westward positive; +180 converts to compass bearing.
    const double sun_az_s =
        atan2(sin(sun_ha), cos(sun_ha) * sin(phi) - tan(sun_dec) * cos(phi));

    out->sun.altitude_deg = static_cast<float>(sun_alt * kDeg);
    out->sun.azimuth_deg = static_cast<float>(norm360(sun_az_s * kDeg + 180.0));

    // --- Moon: truncated Meeus series (accurate to a few degrees) ---
    const double moon_mean_lon = 218.316 + 13.176396 * n;
    const double moon_anom = (134.963 + 13.064993 * n) * kRad;
    const double moon_arg_lat = (93.272 + 13.229350 * n) * kRad;
    const double sun_anom_deg = 357.528 + 0.9856003 * n;
    // Mean elongation drives the two largest periodic terms; keeping them
    // matters for phase, where a couple of degrees of longitude is visible.
    const double mean_elong = (moon_mean_lon - sun_mean) * kRad;
    const double moon_ecl = norm360(moon_mean_lon + 6.289 * sin(moon_anom) +
                                    1.274 * sin(2.0 * mean_elong - moon_anom) +
                                    0.658 * sin(2.0 * mean_elong) +
                                    0.214 * sin(2.0 * moon_anom) -
                                    0.186 * sin(sun_anom_deg * kRad) -
                                    0.114 * sin(2.0 * moon_arg_lat));
    const double moon_lat = 5.128 * sin(moon_arg_lat) * kRad;
    const double moon_lambda = moon_ecl * kRad;
    const double moon_ra =
        atan2(sin(moon_lambda) * cos(eps) - tan(moon_lat) * sin(eps),
              cos(moon_lambda));
    const double moon_dec =
        asin(sin(moon_lat) * cos(eps) + cos(moon_lat) * sin(eps) * sin(moon_lambda));

    const double moon_ha = lst - moon_ra;
    const double moon_alt =
        asin(sin(phi) * sin(moon_dec) + cos(phi) * cos(moon_dec) * cos(moon_ha));
    const double moon_az_s =
        atan2(sin(moon_ha), cos(moon_ha) * sin(phi) - tan(moon_dec) * cos(phi));

    out->moon.altitude_deg = static_cast<float>(moon_alt * kDeg);
    out->moon.azimuth_deg = static_cast<float>(norm360(moon_az_s * kDeg + 180.0));

    // Elongation along the ecliptic: 0 = new, 180 = full. Illuminated fraction
    // follows k = (1 - cos D) / 2, and D < 180 is waxing.
    const double elong = norm360(moon_ecl - sun_ecl);
    out->moon_phase = static_cast<float>((1.0 - cos(elong * kRad)) * 0.5);
    out->moon_waxing = elong < 180.0;

    out->sun_up = out->sun.altitude_deg > 0.0f;
    out->civil_twilight = out->sun.altitude_deg > -6.0f;
    out->nautical_twilight = out->sun.altitude_deg > -12.0f;
}

extern "C" void glass_astro_sky_ramp(const glass_sky_bodies_t* bodies, int* zenith,
                                     int* horizon) {
    *zenith = lerp_stop(bodies->sun.altitude_deg, false);
    *horizon = lerp_stop(bodies->sun.altitude_deg, true);
}