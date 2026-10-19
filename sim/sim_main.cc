/**
 * Host build of the firmware compositor.
 *
 * Compiles the exact glass_gfx and glass_app sources the device runs and
 * writes PNGs, so the layout, the sky and the 16-level dither can be reviewed
 * on a screen before anything is flashed. Also writes the packed 4bpp buffer
 * so a frame can be pushed to the panel as a wallpaper test.
 *
 *   ./out/glass-sim out              every scenario
 *   ./out/glass-sim out rain         one scenario
 *   ./out/glass-sim out --astro      astronomy sanity table
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glass_astro.h"
#include "glass_canvas.h"
#include "glass_card.h"
#include "glass_glass.h"
#include "glass_scene.h"
#include "glass_sky.h"
#include "weather_model.h"

namespace {

// ---------------------------------------------------------------------------
// Minimal PNG writer (grayscale, stored deflate blocks, no dependencies)
// ---------------------------------------------------------------------------

uint32_t crc32_of(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void put_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

/** CRC over the type bytes followed by the payload, as PNG requires. */
bool write_chunk(FILE* f, const char* type, const uint8_t* data, size_t len) {
    uint8_t* joined = static_cast<uint8_t*>(malloc(len + 4));
    memcpy(joined, type, 4);
    if (len > 0) memcpy(joined + 4, data, len);
    const uint32_t crc = crc32_of(joined, len + 4);
    free(joined);

    uint8_t header[8];
    put_be32(header, static_cast<uint32_t>(len));
    memcpy(header + 4, type, 4);
    if (fwrite(header, 1, 8, f) != 8) return false;
    if (len > 0 && fwrite(data, 1, len, f) != len) return false;
    uint8_t crc_be[4];
    put_be32(crc_be, crc);
    return fwrite(crc_be, 1, 4, f) == 4;
}

bool write_png(const char* path, int w, int h, const uint8_t* gray) {
    FILE* f = fopen(path, "wb");
    if (f == nullptr) return false;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13];
    put_be32(ihdr, static_cast<uint32_t>(w));
    put_be32(ihdr + 4, static_cast<uint32_t>(h));
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 0;   // grayscale
    ihdr[10] = 0;  // no interlace
    ihdr[11] = 0;
    ihdr[12] = 0;
    if (!write_chunk(f, "IHDR", ihdr, sizeof(ihdr))) {
        fclose(f);
        return false;
    }

    const size_t raw_len = static_cast<size_t>(h) * (w + 1);
    uint8_t* raw = static_cast<uint8_t*>(malloc(raw_len));
    for (int j = 0; j < h; ++j) {
        raw[static_cast<size_t>(j) * (w + 1)] = 0;
        memcpy(raw + static_cast<size_t>(j) * (w + 1) + 1, gray + static_cast<size_t>(j) * w, w);
    }
    // zlib stream of stored blocks.
    const size_t blocks = (raw_len + 65534) / 65535;
    uint8_t* z = static_cast<uint8_t*>(malloc(raw_len + blocks * 5 + 6 + 4));
    size_t zp = 0;
    z[zp++] = 0x78;
    z[zp++] = 0x01;
    size_t done = 0;
    while (done < raw_len) {
        const size_t take = (raw_len - done) > 65535 ? 65535 : (raw_len - done);
        const bool last = (done + take) >= raw_len;
        z[zp++] = last ? 1 : 0;
        z[zp++] = static_cast<uint8_t>(take);
        z[zp++] = static_cast<uint8_t>(take >> 8);
        z[zp++] = static_cast<uint8_t>(~take);
        z[zp++] = static_cast<uint8_t>((~take) >> 8);
        memcpy(z + zp, raw + done, take);
        zp += take;
        done += take;
    }
    // Adler-32, big endian, as zlib wants it. s1 starts at 1, not 0 - the
    // off-by-one here silently produces PNGs every decoder rejects.
    uint32_t s1 = 1;
    uint32_t s2 = 0;
    for (size_t i = 0; i < raw_len; ++i) {
        s1 = (s1 + raw[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    const uint32_t adler = (s2 << 16) | s1;
    z[zp++] = static_cast<uint8_t>(adler >> 24);
    z[zp++] = static_cast<uint8_t>(adler >> 16);
    z[zp++] = static_cast<uint8_t>(adler >> 8);
    z[zp++] = static_cast<uint8_t>(adler);
    const bool ok = write_chunk(f, "IDAT", z, zp) && write_chunk(f, "IEND", nullptr, 0);
    free(raw);
    free(z);
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

constexpr float kLat = 12.9698f;   // the coordinates the Hearth hub already uses
constexpr float kLon = 77.7499f;
constexpr int32_t kOffset = 19800;  // +05:30

int64_t local_epoch(int hour, int minute) {
    // Days from 1970-01-01 to 2026-10-19, computed without time.h so the table
    // is identical on every host.
    int64_t days = 0;
    for (int y = 1970; y < 2026; ++y) {
        const bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        days += leap ? 366 : 365;
    }
    const int month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int m = 1; m < 10; ++m) days += month_days[m - 1];
    days += 18;  // the 19th
    return days * 86400LL + hour * 3600LL + minute * 60LL - kOffset;
}

struct Scenario {
    const char* name;
    const char* city;
    int wmo;
    int cloud;
    int humidity;
    float wind;
    float wind_dir;
    float precip;
    float temp;
    float uv;
    int hour;
    int minute;
    int days[3];
};

const Scenario kScenarios[] = {
    {"clear-day", "Bengaluru", 0, 4, 40, 9.0f, 270.0f, 0.0f, 27.0f, 8.0f, 13, 30, {0, 2, 1}},
    {"partly", "Bengaluru", 2, 45, 55, 14.0f, 240.0f, 0.0f, 24.0f, 5.0f, 10, 45, {2, 3, 0}},
    {"overcast", "Bengaluru", 3, 96, 78, 11.0f, 200.0f, 0.0f, 21.0f, 1.0f, 15, 10, {3, 3, 2}},
    {"rain", "Bengaluru", 63, 94, 92, 22.0f, 250.0f, 2.4f, 19.0f, 0.5f, 16, 20, {63, 65, 80}},
    {"thunder", "Bengaluru", 95, 98, 95, 34.0f, 280.0f, 6.0f, 22.0f, 0.0f, 18, 40, {95, 63, 2}},
    {"snow", "Leh", 73, 94, 88, 16.0f, 320.0f, 1.1f, -3.0f, 2.0f, 11, 0, {73, 71, 3}},
    {"fog", "Bengaluru", 45, 72, 97, 4.0f, 180.0f, 0.0f, 15.0f, 0.2f, 7, 10, {45, 2, 1}},
    {"clear-night", "Bengaluru", 0, 3, 62, 6.0f, 90.0f, 0.0f, 18.0f, 0.0f, 23, 15, {0, 1, 2}},
    {"golden-hour", "Bengaluru", 1, 32, 58, 12.0f, 260.0f, 0.0f, 23.0f, 1.5f, 6, 20, {1, 0, 2}},
    {"drizzle-night", "Bengaluru", 51, 88, 94, 13.0f, 210.0f, 0.4f, 20.0f, 0.0f, 21, 5, {51, 63, 1}},
    // temp below -900 means "the fetch failed": the clock must stay honest and
    // the weather must read as unknown rather than as a zero measurement.
    {"offline", "Bengaluru", -1, -1, -1, 0.0f, 0.0f, 0.0f, -999.0f, 0.0f, 9, 5, {-1, -1, -1}},
};

void build_snapshot(const Scenario& s, weather_snapshot_t* snap) {
    memset(snap, 0, sizeof(*snap));
    snap->observed_utc = local_epoch(s.hour, s.minute);
    snap->temp_c = s.temp;
    snap->feels_c = s.temp - 2.0f;
    snap->humidity_pct = static_cast<float>(s.humidity);
    snap->wind_kph = s.wind;
    snap->wind_dir_deg = s.wind_dir;
    snap->uv_index = s.uv;
    snap->precip_mm = s.precip;
    snap->cloud_cover_pct = s.cloud;
    snap->wmo_code = s.wmo;
    snap->api_is_day = s.hour >= 6 && s.hour < 19;
    snap->utc_offset_sec = kOffset;
    snprintf(snap->timezone, sizeof(snap->timezone), "Asia/Kolkata");
    for (int i = 0; i < 3; ++i) {
        snprintf(snap->days[i].date, sizeof(snap->days[i].date), "2026-10-%02d",
                 19 + i);
        snap->days[i].wmo_code = s.days[i];
        snap->days[i].high_c = s.temp + 2.0f - i * 0.5f;
        snap->days[i].low_c = s.temp - 5.0f + i * 0.5f;
        snap->days[i].precip_prob_pct = (s.days[i] >= 45 && s.days[i] <= 99) ? 40 + i * 20 : 10;
    }
    if (s.temp <= -900.0f) {
        // Mirror what app_main hands the compositor when the fetch failed: a
        // clock, no observation.
        snap->valid = false;
        snap->temp_c = 0.0f;
        snap->feels_c = 0.0f;
        snap->wmo_code = -1;
        snap->cloud_cover_pct = 40;
        snap->precip_mm = 0.0f;
        snap->uv_index = 0.0f;
        for (int i = 0; i < 3; ++i) snap->days[i].date[0] = '\0';
        return;
    }
    snap->valid = true;
}

void astro_table() {
    printf("  local   sun alt   sun az   moon alt  moon az  phase  wax\n");
    for (int hour = 0; hour < 24; hour += 2) {
        const int64_t epoch = local_epoch(hour, 0);
        glass_sky_bodies_t b;
        glass_astro_compute(epoch, kLat, kLon, &b);
        printf("  %02d:00  %+7.2f  %6.1f  %+8.2f  %7.1f  %5.2f  %s\n", hour,
               b.sun.altitude_deg, b.sun.azimuth_deg, b.moon.altitude_deg,
               b.moon.azimuth_deg, b.moon_phase, b.moon_waxing ? "yes" : "no");
    }
    printf("\nExpected at 12.97N: noon sun 77.0 deg at the equinox, 79.5 deg at the\n"
           "June solstice, 53.6 deg at the December solstice; sunrise ~90 deg az.\n"
           "Bengaluru on 2026-10-19: sunrise ~06:10, sunset ~18:05 IST, moon ~67%%\n"
           "waxing (this model keeps only the largest lunar terms).\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <outdir> [scenario|--astro]\n", argv[0]);
        return 2;
    }
    const char* out_dir = argv[1];
    const char* which = argc > 2 ? argv[2] : nullptr;
    if (which != nullptr && strcmp(which, "--astro") == 0) {
        astro_table();
        return 0;
    }
    if (which != nullptr && strcmp(which, "--moon") == 0) {
        // Published lunations for 2026: new Oct 10, full Oct 26 (UTC).
        int64_t epoch = 0;
        // 2026-10-01T00:00Z
        for (int y = 1970; y < 2026; ++y) {
            epoch += ((y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365) * 86400LL;
        }
        const int md[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        for (int m = 1; m < 10; ++m) epoch += md[m - 1] * 86400LL;
        float prev = -1.0f;
        printf("lunation crossings in Oct-Nov 2026 (model vs published):\n");
        for (int64_t t = epoch; t < epoch + 62LL * 86400; t += 600) {
            glass_sky_bodies_t b;
            glass_astro_compute(t, kLat, kLon, &b);
            if (prev >= 0.0f) {
                if (prev > b.moon_phase && prev > 0.995f) {
                    printf("  full  at day %lld  (published: Oct 26)\n",
                           (long long)((t - epoch) / 86400) + 1);
                }
                if (prev < b.moon_phase && prev < 0.005f) {
                    printf("  new   at day %lld  (published: Oct 10, Nov 9)\n",
                           (long long)((t - epoch) / 86400) + 1);
                }
            }
            prev = b.moon_phase;
        }
        glass_sky_bodies_t b;
        glass_astro_compute(epoch + 18LL * 86400 + 18 * 3600, kLat, kLon, &b);
        printf("  phase on Oct 19 18:30Z = %.3f (published ~0.67)\n", b.moon_phase);
        return 0;
    }

    if (which != nullptr && strcmp(which, "--json") == 0) {
        // Parse a real Open-Meteo payload captured with curl, so the query
        // string in glass_net.cc and this parser are checked against the API
        // rather than against a fixture that agrees with it by construction.
        if (argc < 4) {
            fprintf(stderr, "usage: %s <outdir> --json <file>\n", argv[0]);
            return 2;
        }
        FILE* f = fopen(argv[3], "rb");
        if (f == nullptr) {
            fprintf(stderr, "cannot open %s\n", argv[3]);
            return 1;
        }
        static char json[16384];
        const size_t n = fread(json, 1, sizeof(json) - 1, f);
        fclose(f);
        json[n] = '\0';
        weather_snapshot_t snap;
        if (!weather_parse_open_meteo(json, n, &snap)) {
            fprintf(stderr, "parse failed for %.200s\n", json);
            return 1;
        }
        printf("parsed %zu bytes: wmo=%d temp=%.1f cloud=%d%% wind=%.1f@%.0f "
               "uv=%.1f precip=%.1f offset=%d tz=%s observed=%lld\n",
               n, snap.wmo_code, snap.temp_c, snap.cloud_cover_pct, snap.wind_kph,
               snap.wind_dir_deg, snap.uv_index, snap.precip_mm, snap.utc_offset_sec,
               snap.timezone, (long long)snap.observed_utc);
        for (int i = 0; i < GLASS_WEATHER_DAYS; ++i) {
            printf("  day %d: %s wmo=%d %.1f/%.1f %d%%\n", i, snap.days[i].date,
                   snap.days[i].wmo_code, snap.days[i].high_c, snap.days[i].low_c,
                   snap.days[i].precip_prob_pct);
        }
        glass_card_data_t card;
        weather_format_card(&snap, nullptr, snap.observed_utc, kLat, kLon, &card);
        printf("  card: \"%s\" \"%s\" \"%s\" \"%s%s\" \"%s\" \"%s\" | %s %s %s | "
               "H %s L %s\n",
               card.city, card.date, card.updated, card.temp, card.unit,
               card.condition, card.feels, card.wind, card.humidity, card.uv,
               card.high, card.low);
        for (int i = 0; i < GLASS_CARD_DAYS; ++i) {
            printf("  pill %d: \"%s\" %s  %s / %s  %s\n", i, card.days[i].label,
                   card.days[i].icon, card.days[i].high, card.days[i].low,
                   card.days[i].precip);
        }
        return 0;
    }

    static uint8_t px[GLASS_PIXELS];
    static uint8_t weight[GLASS_PIXELS];
    static uint8_t scratch[GLASS_SCRATCH_BYTES];
    static uint8_t workspace[380 * 110];
    static uint8_t frame4[GLASS_PIXELS / 2];
    static uint8_t view[GLASS_PIXELS];
    int failures = 0;

    // "--skies" draws the background alone. The card covers three quarters of
    // the panel, so the sky cannot be judged from a normal render.
    const bool sky_only = (which != nullptr && strcmp(which, "--skies") == 0);
    if (sky_only) which = nullptr;

    glass_canvas_t canvas;
    glass_canvas_init(&canvas, px, weight, scratch);
    glass_style_t style;
    glass_style_default(&style);

    for (const Scenario& s : kScenarios) {
        if (which != nullptr && strcmp(which, s.name) != 0) continue;

        weather_snapshot_t snap;
        build_snapshot(s, &snap);
        const int64_t now = snap.observed_utc;

        glass_card_data_t card;
        weather_format_card(&snap, s.city, now, kLat, kLon, &card);

        const char* icon = "unknown";
        const char* scene = "cloud";
        const char* label = "Unknown";
        const char* precip = "none";
        int cloud_hint = -1;
        weather_wmo(snap.wmo_code, &icon, &scene, &label, &precip, &cloud_hint);

        glass_sky_input_t sky;
        memset(&sky, 0, sizeof(sky));
        sky.latitude = kLat;
        sky.longitude = kLon;
        sky.epoch_utc = now;
        sky.utc_offset_sec = kOffset;
        sky.wmo_code = snap.wmo_code;
        sky.scene = scene;
        sky.cloud_cover_pct = cloud_hint >= 0 ? cloud_hint : snap.cloud_cover_pct;
        sky.humidity_pct = snap.humidity_pct;
        sky.wind_speed_kph = snap.wind_kph;
        sky.wind_dir_deg = snap.wind_dir_deg;
        sky.precip_mm = snap.precip_mm;
        sky.is_day = weather_daylight(now, kLat, kLon);
        sky.seed = glass_sky_seed(now, snap.wmo_code);

        const bool is_day = weather_daylight(now, kLat, kLon);
        glass_diag_reset();
        glass_render_frame(&canvas, scene, is_day, sky_only ? nullptr : &card,
                           &style, workspace);
        if (!sky_only) {
            glass_diag_t diag;
            glass_diag_get(&diag);
            char messages[GLASS_DIAG_MAX][GLASS_DIAG_LEN];
            const int n_messages = glass_diag_messages(messages);
            const bool clean = diag.missing_glyphs == 0 && diag.icon_fallbacks == 0 &&
                               diag.clipped_text == 0;
            if (!clean) printf("%-14s LAYOUT PROBLEMS:\n", s.name);
            for (int m = 0; m < n_messages; ++m) printf("    %s\n", messages[m]);
            if (!clean) ++failures;
        }

        char path[256];
        const char* tag = sky_only ? "_sky" : "";
        snprintf(path, sizeof(path), "%s/%s%s_8bit.png", out_dir, s.name, tag);
        if (!write_png(path, GLASS_W, GLASS_H, px)) {
            fprintf(stderr, "failed to write %s\n", path);
            return 1;
        }
        glass_quantize(&canvas, frame4, sizeof(frame4), GLASS_DITHER_BAYER8, 255);
        for (int i = 0; i < GLASS_PIXELS; ++i) {
            const int nibble = (i & 1) ? (frame4[i / 2] & 0x0F) : (frame4[i / 2] >> 4);
            view[i] = static_cast<uint8_t>(nibble * 17);
        }
        snprintf(path, sizeof(path), "%s/%s%s_16lvl.png", out_dir, s.name, tag);
        if (!write_png(path, GLASS_W, GLASS_H, view)) {
            fprintf(stderr, "failed to write %s\n", path);
            return 1;
        }
        snprintf(path, sizeof(path), "%s/%s%s_4bpp.bin", out_dir, s.name, tag);
        FILE* bin = fopen(path, "wb");
        if (bin != nullptr) {
            fwrite(frame4, 1, sizeof(frame4), bin);
            fclose(bin);
        }
        glass_sky_bodies_t bodies;
        glass_astro_compute(now, kLat, kLon, &bodies);
        printf("%-14s scene=%-8s cloud=%3d%%  sun %+6.1f deg  az %5.1f  %s\n", s.name,
               scene, sky.cloud_cover_pct, bodies.sun.altitude_deg, bodies.sun.azimuth_deg,
               sky.is_day ? "day" : "night");
    }
    if (failures > 0) {
        fprintf(stderr, "\n%d scenario(s) with layout problems\n", failures);
        return 1;
    }
    return 0;
}