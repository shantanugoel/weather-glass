#include "glass_sky.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

// Cloud density is synthesized at 1/4 scale and bilinearly upsampled: clouds
// are low frequency, and this keeps the whole sky under a few tens of
// milliseconds on the ESP32-S3 instead of a few hundred.
constexpr int kCW = 100;
constexpr int kCH = 75;
uint8_t s_field[kCW * kCH];

struct Rng {
    uint32_t s;
    uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s;
    }
    float f() const { return static_cast<float>(s >> 8) * (1.0f / 16777216.0f); }
    float range(float lo, float hi) const { return lo + f() * (hi - lo); }
};

uint32_t hash2(int x, int y, uint32_t seed) {
    uint32_t h = seed + static_cast<uint32_t>(x) * 374761393u +
                 static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float vnoise(float x, float y, uint32_t seed) {
    const int xi = static_cast<int>(floorf(x));
    const int yi = static_cast<int>(floorf(y));
    const float xf = x - static_cast<float>(xi);
    const float yf = y - static_cast<float>(yi);
    const float u = xf * xf * (3.0f - 2.0f * xf);
    const float v = yf * yf * (3.0f - 2.0f * yf);
    const float a = (hash2(xi, yi, seed) >> 8) * (1.0f / 16777216.0f);
    const float b = (hash2(xi + 1, yi, seed) >> 8) * (1.0f / 16777216.0f);
    const float c = (hash2(xi, yi + 1, seed) >> 8) * (1.0f / 16777216.0f);
    const float d = (hash2(xi + 1, yi + 1, seed) >> 8) * (1.0f / 16777216.0f);
    return (a + (b - a) * u) * (1.0f - v) + (c + (d - c) * u) * v;
}

float fbm(float x, float y, int octaves, uint32_t seed) {
    float sum = 0.0f;
    float amp = 0.5f;
    float total = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += vnoise(x, y, seed + static_cast<uint32_t>(i) * 7919u) * amp;
        total += amp;
        amp *= 0.5f;
        x *= 2.03f;
        y *= 1.97f;
    }
    return sum / total;
}

int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/** Screen position of a body: south-facing view, 120 degrees across the frame. */
void body_screen(float alt, float az, int* x, int* y) {
    float d = az - 180.0f;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    *x = GLASS_W / 2 + static_cast<int>(d * GLASS_W / 120.0f);
    *y = (GLASS_H + 30) - static_cast<int>(alt * (GLASS_H + 30) / 82.0f);
}

void build_cloud_field(uint32_t seed, float cover, float drift) {
    const float thresh = 0.62f - 0.5f * cover;
    for (int j = 0; j < kCH; ++j) {
        for (int i = 0; i < kCW; ++i) {
            // Coarser than it looks on purpose: the glass layer box-blurs
            // whatever is behind it, and detail finer than ~20 px is erased
            // there, so a busy field costs contrast on the open sky and buys
            // nothing under the pills.
            const float x = static_cast<float>(i) * 0.055f + drift;
            const float y = static_cast<float>(j) * 0.075f;
            const float n = fbm(x, y, 3, seed);
            // A second, larger-scale field so an overcast sheet still has
            // mass, instead of looking like one uniform popcorn layer.
            const float big = fbm(x * 0.32f + 11.0f, y * 0.32f + 7.0f, 2,
                                  seed ^ 0x5f3a2u);
            const float v = n * 0.58f + big * 0.42f;
            const float a = clampf((v - thresh) * 3.0f, 0.0f, 1.0f);
            s_field[j * kCW + i] = static_cast<uint8_t>(a * 255.0f);
        }
    }
}

/**
 * Two hill ridges along the bottom edge.
 *
 * Most of this lands under the forecast pills, but it is what those pills
 * frost over: without a ground line the panel is a card floating in noise, and
 * the word "nature" in the brief has nowhere to live.
 */
void draw_ground(glass_canvas_t* c, uint32_t seed, bool is_day, float cover) {
    const int near_tone = is_day ? (cover > 0.6f ? 78 : 58) : 14;
    const int far_tone = near_tone + (is_day ? 40 : 14);
    int far_y[GLASS_W];
    int near_y[GLASS_W];
    for (int i = 0; i < GLASS_W; ++i) {
        const float x = static_cast<float>(i) * 0.011f;
        const float n1 = fbm(x, 3.1f, 3, seed ^ 0x1234u);
        const float n2 = fbm(x * 2.1f + 40.0f, 7.7f, 2, seed ^ 0xabcdu);
        far_y[i] = GLASS_H - 40 - static_cast<int>(n1 * 30.0f);
        near_y[i] = GLASS_H - 14 - static_cast<int>(n2 * 22.0f);
        if (near_y[i] < far_y[i] + 6) near_y[i] = far_y[i] + 6;
    }
    for (int i = 0; i < GLASS_W; ++i) {
        for (int j = far_y[i]; j < GLASS_H; ++j) {
            const bool near = j >= near_y[i];
            const int tone = near ? near_tone : far_tone;
            // A silhouette is solid: weight 0 snaps it to a level instead of
            // dithering an edge that is meant to be hard.
            gc_dot(c, i, j, near ? 255 : 200, static_cast<uint8_t>(tone),
                   near ? 0 : 90);
        }
    }
}

float sample_field(float fx, float fy) {
    float x = clampf(fx, 0.0f, static_cast<float>(kCW - 1) - 0.001f);
    float y = clampf(fy, 0.0f, static_cast<float>(kCH - 1) - 0.001f);
    const int xi = static_cast<int>(x);
    const int yi = static_cast<int>(y);
    const float xf = x - xi;
    const float yf = y - yi;
    const int r0 = yi * kCW;
    const int r1 = (yi + 1) * kCW;
    const float a = s_field[r0 + xi];
    const float b = s_field[r0 + xi + 1];
    const float c = s_field[r1 + xi];
    const float d = s_field[r1 + xi + 1];
    return (a + (b - a) * xf) * (1.0f - yf) + (c + (d - c) * xf) * yf;
}

void draw_stars(glass_canvas_t* c, Rng rng, int count, int band) {
    for (int i = 0; i < count; ++i) {
        const int x = static_cast<int>(rng.f() * GLASS_W);
        // Bias stars toward the upper two thirds so the horizon stays clean.
        const int y = static_cast<int>(powf(rng.f(), 0.65f) * band);
        const int brightness = 110 + static_cast<int>(rng.f() * 145.0f);
        gc_dot(c, x, y, brightness, 255, 255);
        if (brightness > 235) {
            gc_dot(c, x - 1, y, 70, 255, 255);
            gc_dot(c, x + 1, y, 70, 255, 255);
            gc_dot(c, x, y - 1, 70, 255, 255);
            gc_dot(c, x, y + 1, 70, 255, 255);
        }
    }
}

/** Waxing/gibbous terminator: lit where u >= cos(D) * sqrt(1 - v^2). */
void draw_moon(glass_canvas_t* c, int cx, int cy, int radius, float phase,
               bool waxing, float glow) {
    const float angle = acosf(clampf(1.0f - 2.0f * phase, -1.0f, 1.0f));
    const float cos_d = cosf(angle);
    const int r = radius;
    for (int j = -r; j <= r; ++j) {
        for (int i = -r; i <= r; ++i) {
            const float u = static_cast<float>(i) / r;
            const float v = static_cast<float>(j) / r;
            const float d2 = u * u + v * v;
            if (d2 > 1.0f) continue;
            const float edge = sqrtf(1.0f - v * v);
            float terminator = cos_d * edge;
            if (!waxing) terminator = -terminator;
            const float lit = waxing ? (u - terminator) : (terminator - u);
            if (lit < 0.0f) continue;
            const int cov = static_cast<int>(clampf(lit * 220.0f, 0.0f, 255.0f));
            // Limb darkening keeps the disc from looking like a flat sticker.
            const int shade = static_cast<int>(255.0f - 55.0f * d2);
            gc_dot(c, cx + i, cy + j, cov * shade / 255, 255, 200);
        }
    }
    gc_radial(c, cx, cy, r * 4, static_cast<int>(26 * glow), 255);
}

void draw_precip(glass_canvas_t* c, Rng rng, const char* kind, float amount,
                 float tilt) {
    const int count = static_cast<int>(30.0f + clampf(amount, 0.0f, 3.0f) * 90.0f);
    for (int i = 0; i < count; ++i) {
        const int x = static_cast<int>(rng.f() * (GLASS_W + 60.0f)) - 30;
        const int y = static_cast<int>(rng.f() * GLASS_H);
        // Streaks read against either a dark or a bright sky by flipping sign.
        const int local = gc_get(c, x, y);
        const int delta = (local < 128) ? 46 : -46;
        if (strcmp(kind, "snow") == 0) {
            const int radius = 1 + static_cast<int>(rng.f() * 2.0f);
            gc_radial(c, x, y, radius, delta, 255);
        } else if (strcmp(kind, "hail") == 0) {
            gc_dot(c, x, y, 200, local + delta > 255 ? 255 : local + delta, 255);
            gc_line(c, x, y, x + static_cast<int>(tilt * 6.0f), y + 5,
                    local + delta > 255 ? 255 : (local + delta < 0 ? 0 : local + delta),
                    1, 255);
        } else {
            const int len = 12 + static_cast<int>(rng.f() * 16.0f);
            const int x2 = x + static_cast<int>(tilt * len * 2.4f);
            const int v = local + delta > 255 ? 255 : (local + delta < 0 ? 0 : local + delta);
            gc_line(c, x, y, x2, y + len, static_cast<uint8_t>(v), 1, 255);
        }
    }
}

void draw_lightning(glass_canvas_t* c, Rng rng, int seed) {
    int x = 70 + (seed % 200);
    int y = 34;
    gc_radial(c, x + 16, 120, 160, 30, 255);
    int previous_x = x;
    int previous_y = y;
    for (int i = 0; i < 7; ++i) {
        x += static_cast<int>(rng.range(-22.0f, 26.0f));
        y += 20 + static_cast<int>(rng.f() * 12.0f);
        gc_line(c, previous_x, previous_y, x, y, 252, 2, 200);
        if (i == 3) {
            // One fork, because a perfectly straight bolt looks synthetic.
            gc_line(c, x, y, x + 26, y + 26, 240, 1, 200);
        }
        previous_x = x;
        previous_y = y;
    }
}

}  // namespace

extern "C" uint32_t glass_sky_seed(int64_t epoch_utc, int wmo_code) {
    // Stable within an hour so repeated renders agree, different next fetch.
    const uint32_t hour = static_cast<uint32_t>(epoch_utc / 3600);
    uint32_t h = hour * 2654435761u ^ static_cast<uint32_t>(wmo_code) * 40503u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return h;
}

extern "C" void glass_sky_bodies(const glass_sky_input_t* in,
                                 glass_sky_bodies_t* out) {
    glass_astro_compute(in->epoch_utc, in->latitude, in->longitude, out);
}

extern "C" void glass_sky_render(glass_canvas_t* c, const glass_sky_input_t* in) {
    glass_sky_bodies_t bodies;
    glass_astro_compute(in->epoch_utc, in->latitude, in->longitude, &bodies);

    int zenith = 0;
    int horizon = 0;
    glass_astro_sky_ramp(&bodies, &zenith, &horizon);

    const float cover = clampf(static_cast<float>(in->cloud_cover_pct) / 100.0f,
                               0.0f, 1.0f);
    const bool thunder = in->scene != nullptr && strcmp(in->scene, "thunder") == 0;
    const bool snowing = in->scene != nullptr && strcmp(in->scene, "snow") == 0;
    const bool raining = in->scene != nullptr && strcmp(in->scene, "rain") == 0;
    const bool foggy = in->scene != nullptr && strcmp(in->scene, "fog") == 0;

    // 1. Base sky gradient. The square curve keeps the bright horizon band
    //    thin instead of washing the whole frame.
    for (int j = 0; j < GLASS_H; ++j) {
        const float t = static_cast<float>(j) / static_cast<float>(GLASS_H - 1);
        const float tt = t * t;
        const int v = zenith + static_cast<int>((horizon - zenith) * tt);
        gc_vgradient(c, 0, j, GLASS_W, 1, clampi(v, 0, 255), clampi(v, 0, 255), 255);
    }

    Rng rng{in->seed * 2246822519u + 12345u};

    // 2. Stars, before clouds so cloud cover occludes them naturally.
    if (bodies.sun.altitude_deg < -2.0f) {
        const float darkness = clampf((-2.0f - bodies.sun.altitude_deg) / 10.0f, 0.0f, 1.0f);
        const int count = static_cast<int>(230.0f * darkness * (1.0f - cover * 0.88f));
        draw_stars(c, rng, count, GLASS_H * 2 / 3);
    }

    // 3. The light source: sun by day, moon by night, both placed by real
    //    position, so the glow moves across the panel through the day.
    int sun_x = 0;
    int sun_y = 0;
    body_screen(bodies.sun.altitude_deg, bodies.sun.azimuth_deg, &sun_x, &sun_y);
    if (bodies.sun.altitude_deg > -12.0f) {
        const float lift = clampf((bodies.sun.altitude_deg + 12.0f) / 30.0f, 0.0f, 1.0f);
        gc_radial(c, sun_x, sun_y, 150, static_cast<int>(34 + 40 * lift), 255);
        gc_radial(c, sun_x, sun_y, 46, static_cast<int>(40 + 70 * lift), 255);
        if (bodies.sun.altitude_deg > 1.0f) {
            gc_radial(c, sun_x, sun_y, 15, 120, 255);
        }
    }
    if (!in->is_day && bodies.moon.altitude_deg > -4.0f && cover < 0.85f) {
        int mx = 0;
        int my = 0;
        body_screen(bodies.moon.altitude_deg, bodies.moon.azimuth_deg, &mx, &my);
        const float clarity = clampf((0.85f - cover) / 0.5f, 0.0f, 1.0f);
        draw_moon(c, mx, my, 13, bodies.moon_phase, bodies.moon_waxing, clarity);
    }

    // 4. Clouds.
    if (cover > 0.02f) {
        const float drift = static_cast<float>(in->seed % 4096) * 0.0039f;
        build_cloud_field(in->seed, cover, drift);
        const float sunfac = clampf((bodies.sun.altitude_deg + 9.0f) / 24.0f, 0.0f, 1.0f);
        // Overcast flattens the tonal range; scattered cumulus stay punchy.
        const float range = 1.0f - 0.45f * clampf((cover - 0.7f) / 0.3f, 0.0f, 1.0f);
        const int cloud_dark = static_cast<int>((52.0f + 46.0f * sunfac) * range);
        const int cloud_bright = static_cast<int>((118.0f + 128.0f * sunfac) * range);
        for (int j = 0; j < GLASS_H; ++j) {
            const float fy = static_cast<float>(j) * (kCH - 1) / (GLASS_H - 1);
            const float height = 1.0f - static_cast<float>(j) / static_cast<float>(GLASS_H) * 0.55f;
            for (int i = 0; i < GLASS_W; ++i) {
                const float fx = static_cast<float>(i) * (kCW - 1) / (GLASS_W - 1);
                const float a = sample_field(fx, fy) / 255.0f;
                if (a < 0.01f) continue;
                // Vertical derivative of the density field fakes top lighting
                // on the cloud tops without a real light transport pass.
                const float above = sample_field(fx, fy - 0.9f) / 255.0f;
                const float relief = clampf((a - above) * 2.2f, -1.0f, 1.0f);
                int tone = cloud_dark + static_cast<int>((cloud_bright - cloud_dark) *
                                                         clampf(height + relief * 0.45f, 0.0f, 1.0f));
                const int dx = i - sun_x;
                const int dy = j - sun_y;
                const int d2 = dx * dx + dy * dy;
                if (d2 < 46000) {
                    // Silver lining: cloud nearest the light source brightens.
                    tone += static_cast<int>(38.0f * (1.0f - static_cast<float>(d2) / 46000.0f));
                }
                tone = clampi(tone, 0, 255);
                gc_dot(c, i, j, static_cast<int>(a * 232.0f),
                       static_cast<uint8_t>(tone), 255);
            }
        }
    }

    // 5. Fog and haze flatten contrast, which is exactly what haze does.
    float fog = 0.0f;
    if (foggy) {
        fog = (in->wmo_code == 48) ? 0.9f : 0.78f;
    } else if (in->humidity_pct > 88 && in->wind_speed_kph < 10.0f && !raining &&
               !snowing) {
        fog = clampf((static_cast<float>(in->humidity_pct) - 88.0f) / 12.0f, 0.0f, 0.42f);
    }
    if (fog > 0.01f) {
        for (int j = 0; j < GLASS_H; ++j) {
            const float t = static_cast<float>(j) / static_cast<float>(GLASS_H - 1);
            const int base = zenith + static_cast<int>((horizon - zenith) * t * t);
            const int target = clampi(base + 26, 0, 255);
            const int alpha = static_cast<int>(fog * 210.0f * (0.45f + 0.55f * t));
            gc_blend(c, 0, j, GLASS_W, 1, static_cast<uint8_t>(target), alpha, 255);
        }
    }

    // 6. Precipitation, tilted by the reported wind.
    const float tilt = sinf((in->wind_dir_deg + 180.0f) * 0.017453292519943295f) *
                       clampf(in->wind_speed_kph / 55.0f, 0.0f, 1.0f);
    if (raining || thunder) {
        draw_precip(c, rng, thunder ? "hail" : "rain",
                    in->precip_mm > 0.0f ? in->precip_mm : 0.6f, tilt);
    } else if (snowing) {
        draw_precip(c, rng, "snow", in->precip_mm > 0.0f ? in->precip_mm : 0.5f, tilt);
    }

    // 7. Thunder gets one bolt; a static panel cannot flicker.
    if (thunder) {
        draw_lightning(c, rng, static_cast<int>(in->seed & 0x3fff));
    }

    // 8. Hills, after the weather so rain and snow fall in front of them.
    draw_ground(c, in->seed, in->is_day, cover);

    // 9. Light vignette so the glass pills sit forward.
    for (int j = 0; j < GLASS_H; ++j) {
        const float dy = (static_cast<float>(j) - GLASS_H * 0.5f) / (GLASS_H * 0.62f);
        for (int i = 0; i < GLASS_W; ++i) {
            const float dx = (static_cast<float>(i) - GLASS_W * 0.5f) / (GLASS_W * 0.62f);
            const float d2 = dx * dx + dy * dy;
            if (d2 <= 0.55f) continue;
            const int index = j * GLASS_W + i;
            const int delta = -static_cast<int>(16.0f * (d2 - 0.55f));
            const int v = c->px[index] + delta;
            c->px[index] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}