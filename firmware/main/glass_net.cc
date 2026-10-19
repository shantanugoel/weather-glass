#include "glass_net.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace {

constexpr const char* kTag = "glass_net";
constexpr const char* kHost = "api.open-meteo.com";

/**
 * One request for everything the card draws.
 *
 * `timezone=auto` is what lets the device skip an NTP round trip: the payload
 * carries its own UTC offset and a local stamp, so the RTC can be set from the
 * weather fetch that was happening anyway.
 */
void build_url(char* out, size_t cap, float latitude, float longitude,
               const char* timezone) {
    char zone[64] = "auto";
    if (timezone != nullptr && timezone[0] != '\0') {
        strlcpy(zone, timezone, sizeof(zone));
    }
    snprintf(out, cap,
             "https://%s/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
             "is_day,precipitation,weather_code,cloud_cover,wind_speed_10m,"
             "wind_direction_10m,uv_index"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
             "precipitation_probability_max"
             "&forecast_days=3&timezone=%s&timezone_abbreviation=none",
             kHost, static_cast<double>(latitude), static_cast<double>(longitude),
             zone);
}

}  // namespace

extern "C" bool glass_net_fetch(float latitude, float longitude,
                                const char* timezone, char* json,
                                size_t json_capacity, weather_snapshot_t* snap) {
    char url[512];
    build_url(url, sizeof(url), latitude, longitude, timezone);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 12000;
    // The bundle is verified against, not skipped: this is an unattended
    // battery device that will be reusing this credential for years.
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(kTag, "client init failed");
        return false;
    }

    bool ok = false;
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        // Timing distinguishes the two ways this fails: a fast failure is DNS or
        // routing, a failure at roughly the timeout is the connection stalling,
        // which has a different cause and a different fix.
        ESP_LOGE(kTag, "open failed after %ld ms: %s",
                 static_cast<long>((esp_timer_get_time() - t0) / 1000),
                 esp_err_to_name(err));
        ESP_LOGE(kTag, "url was %s", url);
    } else {
        ESP_LOGI(kTag, "connected in %ld ms",
                 static_cast<long>((esp_timer_get_time() - t0) / 1000));
        // fetch_headers returns the content length, not the status - the
        // status has to be asked for separately. Reading the length as a
        // status code makes a 51-byte error body look like "HTTP 51".
        const int content_len = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        int total = 0;
        while (total < static_cast<int>(json_capacity) - 1) {
            const int n = esp_http_client_read(client, json + total,
                                               static_cast<int>(json_capacity) - total - 1);
            if (n <= 0) break;
            total += n;
        }
        json[total] = '\0';
        if (status != 200) {
            // Open-Meteo says what it did not like. Without the body a bad
            // query string is indistinguishable from a dead network.
            ESP_LOGE(kTag, "HTTP %d from %s (length %d, read %d): %.200s", status,
                     kHost, content_len, total, json);
        } else if (total <= 0) {
            ESP_LOGE(kTag, "empty body from %s", kHost);
        } else if (!weather_parse_open_meteo(json, static_cast<size_t>(total), snap)) {
            ESP_LOGE(kTag, "payload did not parse (%d bytes): %.200s", total, json);
        } else {
            ESP_LOGI(kTag, "fetched %d bytes", total);
            ok = true;
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    return ok;
}
