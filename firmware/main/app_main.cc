/**
 * Weather Glass for the Zectrix Note 4.
 *
 * One wake, one fetch, one refresh, back to sleep. Between refreshes the
 * ESP32-S3 is in deep sleep with the panel unpowered: an e-paper image is
 * bistable, so holding the picture costs nothing and the only thing worth
 * spending milliamp-hours on is the radio.
 *
 * The RTC (PCF8563) is the alarm clock. Its INT line is the EXT1 wake source,
 * so the ESP is not keeping time itself, and the two front buttons share the
 * wake mask so a press re-draws immediately.
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "glass_canvas.h"
#include "glass_card.h"
#include "glass_glass.h"
#include "glass_net.h"
#include "glass_wifi.h"
#include "glass_scene.h"
#include "weather_model.h"
#include "rtc_pcf8563.h"  // ZectrixBoard only forward-declares it
#include "zectrix_board.h"
#include "zectrix_board_config.h"
#include "zectrix_epd.h"

namespace {

constexpr const char* kTag = "glass";

// 20 minutes by day, 2 hours at night: overnight the temperature moves a
// couple of degrees and nobody is looking at the panel. Configurable in
// config.toml, clamped by Kconfig to sane values.
constexpr int64_t kDayGapSec = CONFIG_GLASS_DAY_INTERVAL_MIN * 60;
constexpr int64_t kNightGapSec = CONFIG_GLASS_NIGHT_INTERVAL_MIN * 60;
// A failed fetch is retried sooner than a scheduled refresh, but not so soon
// that a dead access point drains the cell in an afternoon.
constexpr int64_t kRetryGapSec = CONFIG_GLASS_RETRY_INTERVAL_MIN * 60;
constexpr uint32_t kWifiTimeoutMs = 15000;

/**
 * Consecutive fetch failures, kept in RTC slow memory so it survives deep
 * sleep without a flash write.
 *
 * A device behind a dead access point would otherwise wake, join, do TLS and
 * burn a full panel refresh every retry interval, forever. The streak doubles
 * the wait up to kMaxOfflineSec and is cleared by a successful fetch or a
 * power-on.
 */
RTC_NOINIT_ATTR uint32_t g_fail_streak;
constexpr int64_t kMaxOfflineSec = 4 * 3600;

/**
 * Consecutive NTP timeouts, kept in RTC memory for the same reason.
 *
 * This network answers association, DNS and HTTPS but never UDP/123, so after
 * a few attempts there is no point paying three seconds for it on every wake.
 * The RTC carries the clock between those checks, and the Open-Meteo
 * observation stamp still reports any drift large enough to matter.
 */
RTC_NOINIT_ATTR uint32_t g_ntp_fail_streak;

/**
 * Sleep bookkeeping in RTC slow memory, so a wake can tell an alarm that fired
 * on time from a wake line that dragged the chip straight back out.
 *
 * The PCF8563 INT pin is open drain and the buttons are bare switches, so a
 * line that is low when we enter deep sleep means EXT1 - "any low" - fires
 * again at once. Without this the panel blanks and redraws every few seconds
 * for as long as the line stays low, which looks like a display fault and
 * flattens the battery.
 */
RTC_NOINIT_ATTR int64_t g_sleep_started_local;
RTC_NOINIT_ATTR int64_t g_sleep_target_local;
RTC_NOINIT_ATTR uint32_t g_spurious_wakes;
constexpr uint32_t kMaxSpuriousWakes = 5;
/**
 * Waking this soon after an alarm armed minutes ahead was not the alarm.
 *
 * The threshold is deliberately well above the alarm's own granularity: the
 * PCF8563 compares minutes only, so a short requested delay is rounded to the
 * next minute boundary and can legitimately fire after a couple of seconds.
 * Treating that as a stuck wake line would put a healthy board into a backoff
 * it never earned, so only intervals a minute boundary cannot explain count.
 */
constexpr int64_t kSpuriousWakeSec = 20;
constexpr int64_t kSpuriousMinIntendedSec = 5 * 60;

/**
 * Wider than the API's 900 s model grid, so a healthy clock stays quiet and
 * only gross RTC failures (run down, wrong year) get a log line.
 */
constexpr int64_t kObservationGridSec = 1200;

/**
 * What the panel is showing, as a hash of the quantised frame.
 *
 * e-paper is bistable: the picture costs nothing to keep, and the 7 s full
 * 16-gray refresh is the most expensive thing this device does - well over
 * half a normal wake. A 30-minute schedule therefore spends most of its
 * energy redrawing a picture that has not changed: the temperature is often
 * the same for two or three consecutive model steps, and the sky plate is
 * chosen by hour, not by minute.
 *
 * So compare the frame we are about to send with the one the panel was last
 * told to show, and send nothing when they agree. The comparison is on the
 * *quantised* frame rather than the snapshot, so it also covers the clock
 * text, the icon and any layout change: if the pixels differ at all, they
 * differ on the wall, and the refresh happens.
 *
 * RTC slow memory because it has to survive deep sleep without a flash write.
 * A power cycle clears it (see app_main), which is the right answer: after a
 * battery swap or a cold boot we have no idea what is on the glass, so it gets
 * drawn.
 */
RTC_NOINIT_ATTR uint64_t g_frame_hash;
RTC_NOINIT_ATTR int64_t g_frame_shown_utc;
RTC_NOINIT_ATTR uint32_t g_frame_skips;
/**
 * Redraw regardless, however unchanged, after this long. The panel has no
 * ghosting of its own once a frame is written, but a comparison is only as
 * good as the memory behind it, and an occasional honest full refresh costs
 * nothing worth arguing about.
 */
constexpr int64_t kForceRefreshSec = 6 * 3600;
/** Same belt and braces in wake counts: at 30 min, four hours. */
constexpr uint32_t kMaxFrameSkips = 8;

ZectrixBoard g_board;

/**
 * GPIOs that can wake us.
 *
 * Only GPIO0-21 are RTC IOs on the ESP32-S3, and of the Note 4's three
 * buttons only OK (GPIO0) and Down (GPIO18) are in that range - Up is GPIO39,
 * so it cannot wake the board from deep sleep. That is a silicon limit, not a
 * software choice, and it is why the alarm is the primary wake source.
 */
constexpr uint64_t kWakeMask = (1ULL << ZECTRIX_RTC_INT) |
                               (1ULL << ZECTRIX_BUTTON_OK) |
                               (1ULL << ZECTRIX_BUTTON_DOWN);

int32_t kFallbackUtcOffset() { return static_cast<int32_t>(CONFIG_GLASS_UTC_OFFSET_SEC); }

/**
 * Latch the wake pads and enter deep sleep.
 *
 * Two things here are load-bearing. The pins have to be held: deep sleep
 * powers down the GPIO matrix, and the PCF8563 INT line is open drain, so a
 * pad that merely stops being driven reads low and trips the "any low"
 * condition the instant we sleep - the board wakes itself once a second,
 * blanking and redrawing the panel and flattening the battery. And the wake has
 * to be armed with the classic EXT1 call: every pin in kWakeMask is an RTCIO,
 * and the _io variant exists for non-RTCIO pins, whose pads do not keep the
 * pull-up set at init.
 */
void deep_sleep_with_ext1() {
    for (uint64_t mask = kWakeMask; mask != 0; mask &= mask - 1) {
        const gpio_num_t pin = static_cast<gpio_num_t>(__builtin_ctzll(mask));
        gpio_hold_dis(pin);
        gpio_hold_en(pin);
    }
    gpio_deep_sleep_hold_en();

    esp_err_t err = esp_sleep_enable_ext1_wakeup(kWakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        // Should not happen - the mask is RTCIO-only - but a panic here would
        // be indistinguishable from the bug above, so say so and try the
        // non-RTCIO path instead.
        ESP_LOGE(kTag, "ext1 wake arm failed: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(kWakeMask,
                                                        ESP_EXT1_WAKEUP_ANY_LOW));
    }
    esp_deep_sleep_start();
}

/* Kconfig hands these over as strings so they can hold decimal points. */
float Latitude() { return strtof(CONFIG_GLASS_LATITUDE, nullptr); }
float Longitude() { return strtof(CONFIG_GLASS_LONGITUDE, nullptr); }

/** Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
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
    *y = static_cast<int>(yy) + (*m <= 2);
}

/** RTC time is local wall-clock time; treat it as an epoch for arithmetic. */
bool rtc_local_epoch(tm* out_tm, int64_t* out_epoch) {
    tm now = {};
    if (g_board.rtc() == nullptr || !g_board.rtc()->GetTime(now)) return false;
    const int64_t days = days_from_civil(now.tm_year + 1900, now.tm_mon + 1,
                                         now.tm_mday);
    if (days < days_from_civil(2020, 1, 1)) {
        ESP_LOGW(kTag, "RTC is unset (day %" PRId64 ")", days);
        return false;
    }
    if (out_tm != nullptr) *out_tm = now;
    if (out_epoch != nullptr) {
        *out_epoch = days * 86400 + now.tm_hour * 3600 + now.tm_min * 60 + now.tm_sec;
    }
    return true;
}

/** Break a local wall-clock epoch into the fields the PCF8563 wants. */
bool tm_from_local_epoch(int64_t local_epoch, tm* out) {
    unsigned m = 0;
    unsigned d = 0;
    int y = 0;
    civil_from_days(static_cast<int64_t>(floor(static_cast<double>(local_epoch) / 86400.0)),
                    &y, &m, &d);
    const int64_t secs = local_epoch - days_from_civil(y, m, d) * 86400;
    if (secs < 0 || secs >= 86400) return false;
    tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = static_cast<int>(m) - 1;
    t.tm_mday = static_cast<int>(d);
    t.tm_hour = static_cast<int>(secs / 3600);
    t.tm_min = static_cast<int>((secs / 60) % 60);
    t.tm_sec = static_cast<int>(secs % 60);
    // The PCF8563 wants tm_wday too; 1970-01-01 was a Thursday.
    t.tm_wday = static_cast<int>((days_from_civil(y, m, d) + 4) % 7);
    *out = t;
    return true;
}

bool sync_clock_via_ntp(uint32_t timeout_ms);
bool clock_looks_valid();

/**
 * Cross-check the system clock against the observation stamp. Log only.
 *
 * Open-Meteo's `current.time` is the start of the model step it came from -
 * the payload says so itself with `interval: 900` - so it is accurate to about
 * fifteen minutes and no better. It will happily agree with a clock that is
 * fourteen minutes slow, which is exactly why the RTC is disciplined by NTP
 * instead. What this check is good for is the gross cases: an RTC that has run
 * down, or rolled into the wrong year, shows up here.
 */
void log_observation_drift(const weather_snapshot_t& snap) {
    const int64_t drift = snap.observed_utc - static_cast<int64_t>(time(nullptr));
    if (clock_looks_valid() && llabs(drift) <= kObservationGridSec) {
        return;
    }
    ESP_LOGW(kTag, "clock disagrees with the observation stamp by %+lld s",
             static_cast<long long>(drift));
}

/** Arm the PCF8563 alarm `delay` from now and let EXT1 put us to sleep. */
void sleep_for(int64_t delay_sec) {
    tm now_tm = {};
    int64_t now_epoch = 0;
    if (!rtc_local_epoch(&now_tm, &now_epoch)) {
        // No usable RTC: fall back to the ESP's own timer so a board with a
        // dead RTC still refreshes instead of freezing on one frame forever.
        ESP_LOGW(kTag, "no RTC; using internal timer for the %lld s sleep",
                 static_cast<long long>(delay_sec));
        if (g_board.rtc() != nullptr) {
            g_board.rtc()->StopCountdownTimer();
            g_board.rtc()->DisableAlarm();
        }
        // No RTC alarm to drive INT, so let the pins themselves arm the wake.
        ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(
            static_cast<uint64_t>(delay_sec) * 1000000ULL));
        deep_sleep_with_ext1();
        return;
    }

    int64_t target = now_epoch + delay_sec;
    tm alarm = now_tm;
    bool armed = false;
    if (g_board.rtc() != nullptr) {
        g_board.rtc()->StopCountdownTimer();
        // Disarm before re-arming. The alarm has no seconds field, so a target
        // still matching the clock's current minute re-asserts AF - and INT -
        // the moment AIE is set, and the board wakes itself back up immediately.
        g_board.rtc()->DisableAlarm();
        if (!tm_from_local_epoch(target, &alarm)) alarm = now_tm;
        if (alarm.tm_min == now_tm.tm_min && alarm.tm_hour == now_tm.tm_hour &&
            alarm.tm_mday == now_tm.tm_mday) {
            target += 60;
            if (!tm_from_local_epoch(target, &alarm)) alarm = now_tm;
        }
        armed = g_board.rtc()->SetAlarm(alarm);
    }
    const int64_t waited = target - now_epoch;
    g_sleep_started_local = now_epoch;
    g_sleep_target_local = target;
    if (armed) {
        ESP_LOGI(kTag, "sleeping %lld s, RTC alarm at %02d:%02d:%02d",
                 static_cast<long long>(waited), alarm.tm_hour, alarm.tm_min,
                 alarm.tm_sec);
    } else {
        ESP_LOGE(kTag, "RTC alarm failed; internal timer will wake us");
        ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(
            static_cast<uint64_t>(delay_sec) * 1000000ULL));
    }

    // Belt and braces: the schedule must never depend solely on an open-drain
    // interrupt line. If INT is stuck high the RTC alarm can never reach us and
    // the board sleeps forever; the ESP's own timer still fires on time. If INT
    // is stuck low, EXT1 wakes us early and the wake-storm guard absorbs it.
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(
        static_cast<uint64_t>(waited) * 1000000ULL));

    // INT is active low when the alarm fires, and the buttons are active low
    // with pull-ups, so "any of them low" is exactly the wake condition.
    deep_sleep_with_ext1();
}

/**
 * What actually woke us, as far as the hardware can say.
 *
 * The pin levels matter as much as the cause bits: an INT that still reads low
 * at boot is a wake source that has not been acknowledged, and will fire again
 * the next time we sleep.
 */
void log_wake_detail() {
    ESP_LOGI(kTag, "wake: causes=0x%08" PRIx32 " ext1=0x%016" PRIx64
                   " int=%d ok=%d down=%d up=%d",
             esp_sleep_get_wakeup_causes(),
             static_cast<uint64_t>(esp_sleep_get_ext1_wakeup_status()),
             gpio_get_level(ZECTRIX_RTC_INT), gpio_get_level(ZECTRIX_BUTTON_OK),
             gpio_get_level(ZECTRIX_BUTTON_DOWN), gpio_get_level(ZECTRIX_BUTTON_UP));
}

const char* wake_cause_name(void) {
    // IDF 6.1 returns a bitmap of causes; the old single-cause getter is
    // deprecated and can lose a source when the alarm and a button fire in
    // the same tick, which is exactly the case worth reporting correctly.
    const uint32_t causes = esp_sleep_get_wakeup_causes();
    if ((causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) != 0) {
        const uint64_t status = esp_sleep_get_ext1_wakeup_status();
        if ((status & (1ULL << ZECTRIX_RTC_INT)) != 0) return "rtc-alarm";
        if ((status & (1ULL << ZECTRIX_BUTTON_OK)) != 0) return "button-ok";
        if ((status & (1ULL << ZECTRIX_BUTTON_DOWN)) != 0) return "button-down";
        return "ext1";
    }
    if ((causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) != 0) return "timer";
    if ((causes & BIT(ESP_SLEEP_WAKEUP_UNDEFINED)) != 0) {
        return esp_reset_reason() == ESP_RST_POWERON ? "power-on" : "reset";
    }
    return "other";
}

/** True once the clock is past 2024, i.e. plausible enough for TLS. */
bool clock_looks_valid() { return time(nullptr) > 1704067200; }

void set_system_clock(int64_t utc_sec, const char* whence) {
    const timeval tv = {static_cast<time_t>(utc_sec), 0};
    if (settimeofday(&tv, nullptr) != 0) {
        ESP_LOGW(kTag, "settimeofday failed for %s", whence);
        return;
    }
    char stamp[32] = {};
    const time_t t = static_cast<time_t>(utc_sec);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S UTC", gmtime(&t));
    ESP_LOGI(kTag, "system clock from %s: %s", whence, stamp);
}

/**
 * Put the RTC's time into the system clock.
 *
 * Not cosmetic. mbedTLS checks the peer certificate against the system clock,
 * so with the clock still at 1970 every TLS handshake with Open-Meteo fails
 * and the fetch returns HTTP 0 - a symptom that points at the network and is
 * nothing of the sort. The PCF8563 keeps time through deep sleep, so it is the
 * cheapest trustworthy source available.
 */
bool sync_clock_from_rtc() {
    int64_t local = 0;
    if (!rtc_local_epoch(nullptr, &local)) return false;
    set_system_clock(local - kFallbackUtcOffset(), "RTC");
    return true;
}

/**
 * Last resort for a board whose RTC never got set: ask an NTP server.
 *
 * Only paid for when the clock is actually wrong; a board that has fetched
 * once has a correct RTC and skips this entirely.
 */
bool sync_clock_via_ntp(uint32_t timeout_ms) {
    if (!glass_wifi_connected()) return false;
    if (g_ntp_fail_streak >= 3) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ESP_LOGW(kTag, "NTP unreachable from this network, trusting the "
                           "RTC (a power cycle re-enables it)");
        }
        return false;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(kTag, "SNTP init failed");
        return false;
    }
    const bool ok = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) == ESP_OK;
    esp_netif_sntp_deinit();
    if (!ok) {
        if (g_ntp_fail_streak < 8) ++g_ntp_fail_streak;
        ESP_LOGW(kTag, "SNTP got no answer in %" PRIu32 " ms (%" PRIu32
                       " consecutive)", timeout_ms, g_ntp_fail_streak);
        return false;
    }
    g_ntp_fail_streak = 0;
    ESP_LOGI(kTag, "system clock from NTP: %lld", static_cast<long long>(time(nullptr)));

    // Give the RTC the time it was missing, so the next wake does not pay for
    // NTP either.
    if (g_board.rtc() != nullptr) {
        const int64_t local = static_cast<int64_t>(time(nullptr)) + kFallbackUtcOffset();
        tm target = {};
        if (tm_from_local_epoch(local, &target) && g_board.rtc()->SetTime(target)) {
            ESP_LOGI(kTag, "RTC set from NTP: %04d-%02d-%02d %02d:%02d local",
                     target.tm_year + 1900, target.tm_mon + 1, target.tm_mday,
                     target.tm_hour, target.tm_min);
        }
    }
    return true;
}

/** Panels live in PSRAM: five full planes do not fit in the 512 KB of SRAM. */
uint8_t* psram_plane(size_t bytes) {
    uint8_t* p = static_cast<uint8_t*>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (p == nullptr) {
        ESP_LOGE(kTag, "PSRAM allocation of %u bytes failed", static_cast<unsigned>(bytes));
    }
    return p;
}

/** FNV-1a over the finished frame; 60 KB takes about a millisecond. */
uint64_t fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 0x100000001b3ULL;
    }
    return h;
}

void draw(zectrix_epd_handle_t epd, const weather_snapshot_t& snap,
          const glass_card_data_t& card, const char* status,
          bool force_full = false) {
    uint8_t* px = psram_plane(GLASS_PIXELS);
    uint8_t* weight = psram_plane(GLASS_PIXELS);
    uint8_t* scratch = psram_plane(GLASS_SCRATCH_BYTES);
    uint8_t* frame = psram_plane(ZECTRIX_EPD_4BPP_FRAME_BYTES);
    uint8_t* workspace = psram_plane(380 * 110);
    if (px == nullptr || weight == nullptr || scratch == nullptr ||
        frame == nullptr || workspace == nullptr) {
        ESP_LOGE(kTag, "out of memory for a frame");
        return;
    }

    glass_canvas_t canvas;
    glass_canvas_init(&canvas, px, weight, scratch);
    glass_style_t style;
    glass_style_default(&style);

    const float latitude = Latitude();
    const float longitude = Longitude();
    const int64_t now = snap.observed_utc;
    const char* icon = "unknown";
    const char* scene = "cloud";
    const char* label = "Unknown";
    const char* precip = "none";
    int cloud_hint = -1;
    weather_wmo(snap.wmo_code, &icon, &scene, &label, &precip, &cloud_hint);

    // The plate is the artwork - sky, slabs, rims and shadows are all in the
    // photograph - so the device path no longer synthesises a sky. The only
    // astronomy it still needs is day versus night, which chooses the plate.
    const bool is_day = weather_daylight(now, latitude, longitude);

    glass_diag_reset();
    glass_render_frame(&canvas, scene, is_day, &card, &style, workspace);
    glass_diag_t diag = {};
    glass_diag_get(&diag);
    if (diag.missing_glyphs || diag.icon_fallbacks || diag.clipped_text) {
        char messages[GLASS_DIAG_MAX][GLASS_DIAG_LEN];
        const int n = glass_diag_messages(messages);
        for (int i = 0; i < n; ++i) ESP_LOGW(kTag, "layout: %s", messages[i]);
    }

    // Ordered dither, not error diffusion: a still image is re-read for weeks,
    // and FS worms are visible on paper where Bayer reads as texture.
    glass_quantize(&canvas, frame, ZECTRIX_EPD_4BPP_FRAME_BYTES,
                   GLASS_DITHER_BAYER8, 255);

    // Decided before the rail is switched on: the saving is the refresh and
    // the power-on together, so weighing it up afterwards would be pointless.
    const uint64_t hash = fnv1a64(frame, ZECTRIX_EPD_4BPP_FRAME_BYTES);
    const int64_t wall = time(nullptr);
    const bool clock_ok = wall > 1704067200;  // 2024: see clock_looks_valid()
    const bool identical = hash != 0 && hash == g_frame_hash;
    // A clock that has not been set, or has run backwards past the last
    // refresh, cannot vouch for how old the picture is: refresh.
    const bool fresh = clock_ok && g_frame_shown_utc > 0 &&
                       wall >= g_frame_shown_utc &&
                       wall - g_frame_shown_utc <= kForceRefreshSec &&
                       g_frame_skips < kMaxFrameSkips;
    if (identical && fresh && !force_full) {
        ++g_frame_skips;
        ESP_LOGI(kTag, "panel: %s%s %s scene=%s unchanged, refresh skipped (%" PRIu32
                       " running)",
                 card.temp, card.unit, status != nullptr ? status : card.condition,
                 scene, g_frame_skips);
        return;
    }
    if (identical && !force_full) {
        ESP_LOGI(kTag, "panel: frame unchanged but %lld s / %" PRIu32
                       " skips since the last refresh; drawing anyway",
                 clock_ok ? static_cast<long long>(wall - g_frame_shown_utc)
                          : -1LL,
                 g_frame_skips);
    }

    ESP_ERROR_CHECK(zectrix_epd_power_on(epd));
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = zectrix_epd_refresh_full_4bpp(
        epd, frame, ZECTRIX_EPD_4BPP_FRAME_BYTES);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "4bpp refresh failed: %s", esp_err_to_name(err));
    } else {
        // The whole point of the device is the thing on the wall, and it is
        // the one step with no feedback of its own; the elapsed time is also
        // what tells us a slow full refresh is eating the battery budget.
        ESP_LOGI(kTag, "panel: %s%s %s scene=%s in %ld ms", card.temp, card.unit,
                 status != nullptr ? status : card.condition, scene,
                 static_cast<long>((esp_timer_get_time() - t0) / 1000));
        // Only a refresh that reached the panel may be claimed as what the
        // panel is showing, or a failed draw would suppress its own retry.
        g_frame_hash = hash;
        g_frame_shown_utc = wall;
        g_frame_skips = 0;
    }
    ESP_ERROR_CHECK(zectrix_epd_power_off(epd));
    (void)status;
}

}  // namespace

/**
 * True when `pin` is being held down as the app comes up.
 *
 * Only meaningful in the first instants after a boot, which is exactly when
 * recovery mode has to be decided: the buttons are momentary, so the operator
 * has to be holding it anyway. Pull-ups are configured here rather than relying
 * on the board's input setup so the reading does not depend on call order.
 */
bool button_held_at_boot(gpio_num_t pin) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << static_cast<int>(pin);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io));
    vTaskDelay(pdMS_TO_TICKS(50));  // let the pull-up settle the pad
    return gpio_get_level(pin) == 0;
}

/**
 * Join, fetch, draw and report the battery: one update of the panel.
 *
 * Returns true when a real observation reached the display. The snapshot and
 * the clock reading are handed back because the sleep decision needs both, and
 * because recovery mode has to be able to ask for exactly what a wake does.
 */
bool update_panel(zectrix_epd_handle_t epd, bool clock_from_rtc,
                  weather_snapshot_t* snap, int64_t* now_utc_out) {
    
    bool fetched = false;
    if (glass_wifi_start(kWifiTimeoutMs)) {
        if (!clock_from_rtc) ESP_LOGW(kTag, "RTC gave no time; relying on NTP");
        // NTP on every wake, not only when the clock looks broken. The API's
        // own stamp is floored to a 15-minute grid, so it cannot tell a clock
        // that is fourteen minutes slow from a correct one, and this thing is a
        // wall clock first. One UDP round trip is noise beside TLS and a 7.7 s
        // refresh, and it re-disciplines the RTC for the stretches when the
        // network is down.
        sync_clock_via_ntp(3000);
        static char json[GLASS_JSON_CAPACITY];
        fetched = glass_net_fetch(Latitude(), Longitude(),
                                  CONFIG_GLASS_TIMEZONE, json, sizeof(json), snap);
        if (fetched) {
            log_observation_drift(*snap);
        } else {
            ESP_LOGW(kTag, "fetch failed; drawing the RTC clock alone");
        }
        glass_wifi_stop();
    }

    // Without a fetch, fall back to whatever the RTC says so the clock keeps
    // its own time and the panel never shows a frozen "updated 00:00".
    int64_t now_utc = 0;
    tm rtc_tm = {};
    int64_t rtc_local = 0;
    if (rtc_local_epoch(&rtc_tm, &rtc_local)) {
        now_utc = rtc_local - kFallbackUtcOffset();
        if (!fetched) {
            snap->observed_utc = now_utc;
            snap->utc_offset_sec = kFallbackUtcOffset();
            snprintf(snap->timezone, sizeof(snap->timezone), "%s",
                     CONFIG_GLASS_TIMEZONE);
            // No observation to draw a sky from: pick a neutral overcast-ish
            // sky for the time of day rather than claiming "clear, 0% cloud".
            snap->wmo_code = -1;
            snap->cloud_cover_pct = 40;
        }
    } else if (fetched) {
        now_utc = snap->observed_utc;
    }

    glass_card_data_t card = {};
    weather_format_card(snap, CONFIG_GLASS_CITY, now_utc, Latitude(), Longitude(),
                        &card);
    if (!fetched) {
        snprintf(card.status, sizeof(card.status), "offline");
    }
    draw(epd, *snap, card, fetched ? nullptr : "offline");

    const ZectrixPowerSnapshot power = g_board.ReadPowerSnapshot();
    if (power.battery_valid) {
        ESP_LOGI(kTag, "battery %u mV (%u%%)", power.battery_mv,
                 power.battery_percent);
    }
    *now_utc_out = now_utc;
    return fetched;
}

/**
 * Hold OK as the app comes up and the device refuses to sleep.
 *
 * The Note 4 has a RESET pin-hole and OK is GPIO0, the download-mode strap, so
 * holding OK and pressing RESET reaches the ROM bootloader outright - that is
 * the flash path. This covers the step before it: with the app running but not
 * sleeping, USB-Serial-JTAG stays enumerable and esptool can arrive whenever it
 * likes instead of us racing a fifteen second wake window, which is the only
 * reason reflashing this board has ever been difficult.
 */
void recovery_loop(zectrix_epd_handle_t epd) {
    ESP_LOGW(kTag, "RECOVERY: OK held at boot, staying awake with USB open");
    ESP_LOGI(kTag, "  f=fetch+render  r=render  n=ntp  b=battery  s=sleep  q=reboot");
    weather_snapshot_t snap = {};
    int64_t now_utc = 0;
    bool have = false;
    uint32_t beat = 0;
    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        timeval tv = {0, 200000};
        if (select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv) > 0) {
            char c = 0;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                ESP_LOGI(kTag, "recovery command '%c'", c);
                switch (c) {
                    case 'f':
                        have = update_panel(epd, true, &snap, &now_utc);
                        break;
                    case 'r':
                        if (have) {
                            glass_card_data_t card = {};
                            weather_format_card(&snap, CONFIG_GLASS_CITY, now_utc,
                                                Latitude(), Longitude(), &card);
                            // A keypress is an operator asking to see the glass,
                            // not an optimisation opportunity: always draw it.
                            draw(epd, snap, card, nullptr, /*force_full=*/true);
                        } else {
                            ESP_LOGW(kTag, "nothing to draw yet, press f first");
                        }
                        break;
                    case 'n':
                        sync_clock_via_ntp(3000);
                        break;
                    case 'b': {
                        const ZectrixPowerSnapshot p = g_board.ReadPowerSnapshot();
                        ESP_LOGI(kTag, "battery %u mV (%u%%)", p.battery_mv,
                                 p.battery_percent);
                        break;
                    }
                    case 's':
                        ESP_LOGW(kTag, "leaving recovery, sleeping 60 s");
                        sleep_for(60);
                        break;
                    case 'q':
                        esp_restart();
                        break;
                    default:
                        break;
                }
            }
        }
        // Blink so the mode is visible without a serial cable.
        if (++beat % 5 == 0) g_board.SetPowerLed((beat / 5) % 2 == 0);
        if (beat % 150 == 0) {
            ESP_LOGI(kTag, "recovery alive (%" PRIu32 " s), last fetch %s",
                     beat / 5, have ? "ok" : "never/failed");
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "Weather Glass waking (%s)", wake_cause_name());
    if (esp_reset_reason() == ESP_RST_POWERON || esp_reset_reason() == ESP_RST_UNKNOWN ||
        g_fail_streak > 16) {
        // A fresh power cycle is a human plugging it in: worth a full-speed
        // retry rather than inheriting an outage backoff. The range check
        // covers the garbage RTC slow memory holds on a true cold boot.
        g_fail_streak = 0;
        g_ntp_fail_streak = 0;
        g_spurious_wakes = 0;
        g_sleep_started_local = 0;
        g_sleep_target_local = 0;
        // Nothing is known about the glass after a cold boot, so the first
        // frame is always written rather than compared with RTC garbage.
        g_frame_hash = 0;
        g_frame_shown_utc = 0;
        g_frame_skips = 0;
    }

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);

    ESP_ERROR_CHECK(g_board.Init());
    g_board.SetPowerLed(false);

    // The compiled-in schedule, on every wake. These constants come from
    // config.toml via sdkconfig.defaults.local, and Kconfig only applies a
    // default to a symbol that firmware/sdkconfig does not already answer - so
    // this line is what makes a stale sdkconfig visible instead of silently
    // refreshing the panel once a minute for weeks. tools/gen_config.py
    // invalidates sdkconfig when the fragment changes; this says what won.
    ESP_LOGI(kTag, "schedule: day %lld min, night %lld min, retry %lld min, "
                   "force refresh every %lld s",
             static_cast<long long>(kDayGapSec / 60),
             static_cast<long long>(kNightGapSec / 60),
             static_cast<long long>(kRetryGapSec / 60),
             static_cast<long long>(kForceRefreshSec));

    // Before anything that can open a TLS socket: certificate validation reads
    // the system clock, which comes up at 1970.
    const bool clock_from_rtc = sync_clock_from_rtc();
    log_wake_detail();

    // A wake far too early for the alarm we armed is the wake line, not the
    // alarm. Go back to sleep with the time still owed rather than paying for
    // Wi-Fi, a fetch and a full panel refresh - the panel cannot take that
    // every few seconds, and the battery cannot either. After a bounded number
    // of attempts the cycle runs anyway, so a genuinely stuck line still shows
    // a current forecast instead of freezing on an old frame.
    tm wake_tm = {};
    int64_t wake_local = 0;
    if (g_sleep_target_local < g_sleep_started_local) g_sleep_started_local = 0;  // cold-boot garbage
    if (rtc_local_epoch(&wake_tm, &wake_local) && g_sleep_started_local != 0 &&
        wake_local > g_sleep_started_local) {
        const int64_t intended = g_sleep_target_local - g_sleep_started_local;
        const int64_t slept = wake_local - g_sleep_started_local;
        if (intended >= kSpuriousMinIntendedSec && slept < kSpuriousWakeSec) {
            if (g_spurious_wakes < kMaxSpuriousWakes) {
                ++g_spurious_wakes;
                const int64_t remaining =
                    intended - slept < 30 ? 30 : intended - slept;
                ESP_LOGW(kTag, "woke %" PRId64 " s into a %" PRId64
                               " s alarm; wake line still low (try %" PRIu32
                               "), sleeping %" PRId64 " s more",
                         slept, intended, g_spurious_wakes, remaining);
                sleep_for(remaining);
            }
            ESP_LOGE(kTag,
                     "wake line still low after %" PRIu32 " attempts; running a full "
                     "cycle anyway",
                     g_spurious_wakes);
        }
        g_spurious_wakes = 0;
    }

    zectrix_epd_config_t epd_config;
    zectrix_epd_get_default_config(&epd_config);
    zectrix_epd_handle_t epd = nullptr;
    ESP_ERROR_CHECK(zectrix_epd_new(&epd_config, &epd));

    // Checked before anything that could put us back to sleep: a board being
    // driven into recovery is a board somebody is about to flash.
    if (button_held_at_boot(ZECTRIX_BUTTON_OK)) recovery_loop(epd);

    weather_snapshot_t snap = {};
    int64_t now_utc = 0;
    const bool fetched = update_panel(epd, clock_from_rtc, &snap, &now_utc);

    zectrix_epd_del(epd);

    int64_t delay = 0;
    if (fetched) {
        g_fail_streak = 0;
        delay = weather_next_fetch_delay_sec(&snap, now_utc, Latitude(), Longitude(),
                                            kDayGapSec, kNightGapSec);
    } else {
        // Doubling from the configured retry gap, capped, so an outage costs
        // one wake per four hours instead of one per five minutes.
        if (g_fail_streak < 16) ++g_fail_streak;
        delay = kRetryGapSec;
        for (uint32_t i = 1; i < g_fail_streak && delay < kMaxOfflineSec; ++i) {
            delay *= 2;
        }
        if (delay > kMaxOfflineSec) delay = kMaxOfflineSec;
        ESP_LOGW(kTag, "offline for %" PRIu32 " wake(s); retrying in %lld s",
                 g_fail_streak, static_cast<long long>(delay));
    }
    sleep_for(delay);
}
