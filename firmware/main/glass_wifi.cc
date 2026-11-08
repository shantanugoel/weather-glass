#include "glass_wifi.h"

#include <inttypes.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

namespace {

constexpr const char* kTag = "glass_wifi";
constexpr int kConnectedBit = BIT0;
constexpr int kFailedBit = BIT1;

// Beacon intervals between wakeups while idle. Fetches are device-initiated,
// so the extra inbound latency is invisible and the radio sleeps instead.
constexpr uint8_t kListenInterval = 5;

EventGroupHandle_t g_events = nullptr;
esp_netif_t* g_sta = nullptr;
// The driver is initialized once. `g_started` is only the interface state, so
// stop/start cycles are cheap - and, more importantly, legal: calling
// esp_wifi_init() twice on the same driver aborts, which is what a bench build
// that runs several fetch cycles without a deep sleep in between would do.
bool g_initialized = false;
bool g_started = false;
bool g_got_ip = false;

void OnWifiEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_got_ip = false;
        if (g_events != nullptr) {
            xEventGroupClearBits(g_events, kConnectedBit);
            // The first failure is the one that matters: a wrong password
            // never recovers, and the caller should stop waiting.
            xEventGroupSetBits(g_events, kFailedBit);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        g_got_ip = true;
        if (g_events != nullptr) {
            xEventGroupClearBits(g_events, kFailedBit);
            xEventGroupSetBits(g_events, kConnectedBit);
        }
        ESP_LOGI(kTag, "joined, ip " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

}  // namespace

extern "C" bool glass_wifi_start(uint32_t connect_timeout_ms) {
    const char* ssid = CONFIG_GLASS_WIFI_SSID;
    const char* password = CONFIG_GLASS_WIFI_PASSWORD;
    if (ssid[0] == '\0') {
        ESP_LOGW(kTag, "CONFIG_GLASS_WIFI_SSID is empty; staying offline");
        return false;
    }

    if (g_events == nullptr) {
        g_events = xEventGroupCreate();
    }
    if (!g_initialized) {
        ESP_ERROR_CHECK(esp_netif_init());
        const esp_err_t loop_err = esp_event_loop_create_default();
        if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(loop_err);
        }
        if (g_sta == nullptr) {
            g_sta = esp_netif_create_default_wifi_sta();
        }
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &OnWifiEvent, nullptr, nullptr));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &OnWifiEvent, nullptr, nullptr));

        wifi_config_t wc = {};
        strlcpy(reinterpret_cast<char*>(wc.sta.ssid), ssid, sizeof(wc.sta.ssid));
        strlcpy(reinterpret_cast<char*>(wc.sta.password), password,
                sizeof(wc.sta.password));
        wc.sta.threshold.authmode = password[0] != '\0' ? WIFI_AUTH_WPA2_PSK
                                                        : WIFI_AUTH_OPEN;
        wc.sta.listen_interval = kListenInterval;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        g_initialized = true;
    }

    // Between cycles the event bits are stale by design; the wait below must
    // only see this attempt's connection.
    xEventGroupClearBits(g_events, kConnectedBit | kFailedBit);
    g_got_ip = false;
    if (!g_started) {
        // Power save on: the board is asleep between fetches anyway, and this
        // is what keeps the radio from dominating the wake budget.
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));
        ESP_ERROR_CHECK(esp_wifi_start());
        g_started = true;
    } else {
        ESP_ERROR_CHECK(esp_wifi_connect());
    }

    const TickType_t timeout = pdMS_TO_TICKS(connect_timeout_ms);
    const EventBits_t bits = xEventGroupWaitBits(
        g_events, kConnectedBit | kFailedBit, pdFALSE, pdFALSE, timeout);
    if ((bits & kConnectedBit) != 0) {
        // Power save off for the transfer window. MAX_MODEM lets the STA sleep
        // between beacons, and on a busy 2.4 GHz IoT AP that is enough to stall
        // a TLS handshake past its timeout: a captured failure showed the radio
        // asleep for 17 of the 18 seconds the connection spent failing - after
        // the certificate had already validated. A wake is a few seconds long,
        // so this costs almost nothing next to a panel that says "no data".
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
        return true;
    }
    if ((bits & kFailedBit) != 0) {
        ESP_LOGE(kTag, "association failed for SSID \"%s\"", ssid);
    } else {
        ESP_LOGE(kTag, "no address within %" PRIu32 " ms", connect_timeout_ms);
    }
    return false;
}

extern "C" bool glass_wifi_connected(void) { return g_got_ip; }

extern "C" void glass_wifi_stop(void) {
    if (!g_started) return;
    // Down but not out: the driver stays initialized so a bench session can
    // run another cycle without esp_wifi_init() aborting, and deep sleep
    // resets everything anyway.
    esp_wifi_disconnect();
    esp_wifi_stop();
    g_got_ip = false;
    g_started = false;
}
