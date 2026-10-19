#include "weather_model.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "glass_astro.h"
#include "mini_json.h"

#include "generated/wmo_table.h"

namespace {

const char* kWeekdays[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
const char* kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/** Days since 1970-01-01 for a proleptic Gregorian date (Hinnant's algorithm). */
int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t z, int* y, unsigned* m, unsigned* d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = static_cast<int>(yy + (*m <= 2));
}

/** kWeekdays starts on Monday; 1970-01-01 (day 0) was a Thursday. */
int weekday_index(int64_t days) {
    const int raw = static_cast<int>((days % 7 + 7) % 7);
    return (raw + 3) % 7;
}

const char* wind_letter(float deg) {
    static const char* kDirs[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int index = static_cast<int>(fmodf(deg + 22.5f, 360.0f) / 45.0f);
    if (index < 0) index += 8;
    if (index > 7) index = 7;
    return kDirs[index];
}

void offset_label(int32_t offset_sec, char* out, size_t size) {
    const int total = offset_sec < 0 ? -offset_sec : offset_sec;
    snprintf(out, size, "%c%02d%02d", offset_sec < 0 ? '-' : '+', total / 3600,
             (total % 3600) / 60);
}

}  // namespace

extern "C" void weather_wmo(int code, const char** icon, const char** scene,
                            const char** label, const char** precip,
                            int* cloud_pct) {
    for (int i = 0; i < kWmoTableSize; ++i) {
        if (kWmoTable[i].code != code) continue;
        if (icon != nullptr) *icon = kWmoTable[i].icon;
        if (scene != nullptr) *scene = kWmoTable[i].scene;
        if (label != nullptr) *label = kWmoTable[i].label;
        if (precip != nullptr) *precip = kWmoTable[i].precip;
        if (cloud_pct != nullptr) {
            *cloud_pct = kWmoTable[i].cloud_pct == 255 ? -1
                                                       : kWmoTable[i].cloud_pct;
        }
        return;
    }
    if (icon != nullptr) *icon = "unknown";
    if (scene != nullptr) *scene = "cloud";
    if (label != nullptr) *label = "Unknown";
    if (precip != nullptr) *precip = "none";
    if (cloud_pct != nullptr) *cloud_pct = -1;
}

extern "C" int64_t weather_local_stamp_to_utc(const char* stamp, int32_t offset_sec) {
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    int hh = 0;
    int mm = 0;
    if (stamp == nullptr || strlen(stamp) < 16) return 0;
    if (sscanf(stamp, "%4d-%2u-%2uT%2d:%2d", &y, &m, &d, &hh, &mm) != 5) return 0;
    return days_from_civil(y, m, d) * 86400LL + hh * 3600LL + mm * 60LL - offset_sec;
}

extern "C" bool weather_parse_open_meteo(const char* json, size_t len,
                                         weather_snapshot_t* out) {
    memset(out, 0, sizeof(*out));
    size_t current_len = 0;
    const char* current = json_find(json, len, "current", &current_len);
    if (current == nullptr) return false;

    double value = 0.0;
    long ivalue = 0;
    char stamp[32] = {0};
    if (json_string(current, current_len, "time", stamp, sizeof(stamp))) {
        if (!json_int(json, len, "utc_offset_seconds", &ivalue)) ivalue = 0;
        out->utc_offset_sec = static_cast<int32_t>(ivalue);
        out->observed_utc = weather_local_stamp_to_utc(stamp, out->utc_offset_sec);
    }
    if (out->observed_utc == 0) return false;

    out->temp_c = json_number(current, current_len, "temperature_2m", &value) ? value : 0.0f;
    out->feels_c = json_number(current, current_len, "apparent_temperature", &value) ? value
                                                                                    : out->temp_c;
    out->humidity_pct =
        json_number(current, current_len, "relative_humidity_2m", &value) ? value : 0.0f;
    out->wind_kph = json_number(current, current_len, "wind_speed_10m", &value) ? value : 0.0f;
    out->wind_dir_deg =
        json_number(current, current_len, "wind_direction_10m", &value) ? value : 0.0f;
    out->precip_mm = json_number(current, current_len, "precipitation", &value) ? value : 0.0f;
    out->uv_index = json_number(current, current_len, "uv_index", &value) ? value : 0.0f;
    out->cloud_cover_pct =
        json_int(current, current_len, "cloud_cover", &ivalue) ? (int)ivalue : -1;
    out->wmo_code = json_int(current, current_len, "weather_code", &ivalue) ? (int)ivalue : -1;
    out->api_is_day = json_int(current, current_len, "is_day", &ivalue) ? ivalue != 0 : true;
    json_string(json, len, "timezone", out->timezone, sizeof(out->timezone));

    size_t daily_len = 0;
    const char* daily = json_find(json, len, "daily", &daily_len);
    if (daily != nullptr) {
        char dates[GLASS_WEATHER_DAYS][32];
        const int ndates =
            json_array_strings(daily, daily_len, "time", dates, GLASS_WEATHER_DAYS);
        double codes[GLASS_WEATHER_DAYS] = {0};
        double highs[GLASS_WEATHER_DAYS] = {0};
        double lows[GLASS_WEATHER_DAYS] = {0};
        double probs[GLASS_WEATHER_DAYS] = {0};
        const int ncodes =
            json_array_numbers(daily, daily_len, "weather_code", codes, GLASS_WEATHER_DAYS);
        const int nhigh = json_array_numbers(daily, daily_len, "temperature_2m_max", highs,
                                             GLASS_WEATHER_DAYS);
        const int nlow = json_array_numbers(daily, daily_len, "temperature_2m_min", lows,
                                            GLASS_WEATHER_DAYS);
        const int nprob = json_array_numbers(daily, daily_len, "precipitation_probability_max",
                                             probs, GLASS_WEATHER_DAYS);
        for (int i = 0; i < GLASS_WEATHER_DAYS; ++i) {
            if (i >= ndates) break;
            snprintf(out->days[i].date, sizeof(out->days[i].date), "%.15s", dates[i]);
            out->days[i].wmo_code = i < ncodes ? (int)codes[i] : -1;
            out->days[i].high_c = i < nhigh ? (float)highs[i] : 0.0f;
            out->days[i].low_c = i < nlow ? (float)lows[i] : 0.0f;
            out->days[i].precip_prob_pct = i < nprob ? (int)probs[i] : 0;
        }
    }

    out->valid = out->wmo_code >= 0;
    return out->valid;
}

extern "C" bool weather_daylight(int64_t epoch_utc, float latitude, float longitude) {
    glass_sky_bodies_t bodies;
    glass_astro_compute(epoch_utc, latitude, longitude, &bodies);
    return bodies.sun.altitude_deg > -0.83f;
}

extern "C" void weather_weekday_name(const char* date, const char* today, char* out,
                                     size_t out_size) {
    if (out_size == 0) return;
    out[0] = '\0';
    if (date == nullptr || strlen(date) < 10) {
        snprintf(out, out_size, "------");
        return;
    }
    if (today != nullptr && strncmp(date, today, 10) == 0) {
        snprintf(out, out_size, "Today");
        return;
    }
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    if (sscanf(date, "%4d-%2u-%2u", &y, &m, &d) != 3) {
        snprintf(out, out_size, "------");
        return;
    }
    const int64_t days = days_from_civil(y, m, d);
    snprintf(out, out_size, "%s", kWeekdays[weekday_index(days)]);
}

extern "C" void weather_format_card(const weather_snapshot_t* s, const char* city,
                                    int64_t now_utc, float latitude,
                                    float longitude, glass_card_data_t* out) {
    memset(out, 0, sizeof(*out));
    const int64_t local = s->observed_utc + s->utc_offset_sec;
    const int64_t days = local / 86400;
    unsigned m = 0;
    unsigned d = 0;
    int y = 0;
    civil_from_days(days, &y, &m, &d);
    const int hh = static_cast<int>((local / 3600) % 24);
    const int mm = static_cast<int>((local / 60) % 60);

    if (city != nullptr && city[0] != '\0') {
        snprintf(out->city, sizeof(out->city), "%s", city);
    } else if (s->timezone[0] != '\0') {
        const char* slash = strrchr(s->timezone, '/');
        // Bounded: the timezone field is wider than the city field, and an
        // unbounded %s here is a -Werror format-truncation failure.
        snprintf(out->city, sizeof(out->city), "%.27s", slash ? slash + 1 : s->timezone);
    } else {
        snprintf(out->city, sizeof(out->city), "Weather");
    }

    snprintf(out->date, sizeof(out->date), "%s %u, %s",
             m >= 1 && m <= 12 ? kMonths[m - 1] : "---", d,
             kWeekdays[weekday_index(days)]);
    char zone[10];
    offset_label(s->utc_offset_sec, zone, sizeof(zone));
    snprintf(out->updated, sizeof(out->updated), "updated %02d:%02d %s", hh, mm, zone);

    const float temp = s->temp_c;
    if (!s->valid) {
        // No observation: the clock above is still honest, but drawing "0°C"
        // and "Clear" from a zeroed snapshot would be a lie on the wall. Blank
        // the weather and let the caller add its own status note.
        snprintf(out->temp, sizeof(out->temp), "--");
        snprintf(out->unit, sizeof(out->unit), "\xc2\xb0");
        snprintf(out->condition, sizeof(out->condition), "No data");
        out->feels[0] = '\0';
        snprintf(out->high, sizeof(out->high), "--\xc2\xb0");
        snprintf(out->low, sizeof(out->low), "--\xc2\xb0");
        snprintf(out->wind, sizeof(out->wind), "-- km/h");
        snprintf(out->humidity, sizeof(out->humidity), "--%%");
        snprintf(out->uv, sizeof(out->uv), "--");
        snprintf(out->icon, sizeof(out->icon), "unknown");
        out->is_day = weather_daylight(now_utc != 0 ? now_utc : s->observed_utc,
                                       latitude, longitude);
        return;
    }
    snprintf(out->temp, sizeof(out->temp), "%ld", lroundf(temp));
    snprintf(out->unit, sizeof(out->unit), "\xc2\xb0" "C");
    snprintf(out->feels, sizeof(out->feels), "feels %ld\xc2\xb0", lroundf(s->feels_c));
    snprintf(out->high, sizeof(out->high), "%ld\xc2\xb0", lroundf(s->days[0].high_c));
    snprintf(out->low, sizeof(out->low), "%ld\xc2\xb0", lroundf(s->days[0].low_c));
    snprintf(out->wind, sizeof(out->wind), "%s %ld km/h", wind_letter(s->wind_dir_deg),
             lroundf(s->wind_kph));
    snprintf(out->humidity, sizeof(out->humidity), "%ld%%", lroundf(s->humidity_pct));
    snprintf(out->uv, sizeof(out->uv), "%.1f", static_cast<double>(s->uv_index));

    const char* icon = "unknown";
    const char* scene = "cloud";
    const char* label = "Unknown";
    const char* precip = "none";
    weather_wmo(s->wmo_code, &icon, &scene, &label, &precip, nullptr);
    snprintf(out->icon, sizeof(out->icon), "%s", icon);
    snprintf(out->condition, sizeof(out->condition), "%s", label);
    out->is_day = weather_daylight(now_utc != 0 ? now_utc : s->observed_utc, latitude,
                                   longitude);

    for (int i = 0; i < GLASS_CARD_DAYS; ++i) {
        const weather_day_t* src = &s->days[i];
        glass_card_day_t* dst = &out->days[i];
        if (src->date[0] == '\0') continue;
        weather_weekday_name(src->date, s->days[0].date, dst->label, sizeof(dst->label));
        const char* d_icon = "unknown";
        const char* d_scene = "cloud";
        const char* d_label = "Unknown";
        const char* d_precip = "none";
        weather_wmo(src->wmo_code, &d_icon, &d_scene, &d_label, &d_precip, nullptr);
        snprintf(dst->icon, sizeof(dst->icon), "%s", d_icon);
        snprintf(dst->high, sizeof(dst->high), "%ld\xc2\xb0", lroundf(src->high_c));
        snprintf(dst->low, sizeof(dst->low), "%ld\xc2\xb0", lroundf(src->low_c));
        if (strcmp(d_precip, "none") == 0) {
            snprintf(dst->precip, sizeof(dst->precip), "%d%% dry", src->precip_prob_pct);
        } else {
            snprintf(dst->precip, sizeof(dst->precip), "%d%% %s", src->precip_prob_pct,
                     strcmp(d_precip, "hail") == 0 ? "hail" : d_precip);
        }
        dst->is_day = out->is_day;
    }

    if (now_utc > s->observed_utc + 2 * 3600) {
        snprintf(out->status, sizeof(out->status), "observation is stale");
    }
}

extern "C" int64_t weather_next_fetch_delay_sec(const weather_snapshot_t* snap,
                                                int64_t now_utc, float latitude,
                                                float longitude, int64_t day_gap,
                                                int64_t night_gap) {
    (void)snap;
    const bool day = weather_daylight(now_utc, latitude, longitude);
    const int64_t gap = day ? day_gap : night_gap;
    return gap < 60 ? 60 : gap;
}