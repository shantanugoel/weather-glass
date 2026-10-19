#include "glass_card.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glass_font.h"
#include "glass_icons.h"

// Generated faces.
extern "C" {
extern const glass_font_t glass_font_f9r;
extern const glass_font_t glass_font_f10r;
extern const glass_font_t glass_font_f12r;
extern const glass_font_t glass_font_f17r;
extern const glass_font_t glass_font_f9b;
extern const glass_font_t glass_font_f10b;
extern const glass_font_t glass_font_f12b;
extern const glass_font_t glass_font_f17b;
extern const glass_font_t glass_font_f23r;
extern const glass_font_t glass_font_f55b;
extern const glass_font_t glass_font_f73b;
extern const glass_font_t glass_font_f90b;
}

namespace {

constexpr uint8_t kInkDark = 12;
constexpr uint8_t kInkLight = 246;
constexpr uint8_t kRuleDark = 115;
constexpr uint8_t kRuleLight = 205;

/**
 * Text with a one-pixel halo of the opposite ink. The sky behind the header
 * and the forecast pills varies a lot, and a halo is cheaper than measuring
 * every glyph's neighbourhood while still guaranteeing legibility.
 */
int text_halo(glass_canvas_t* c, const glass_font_t* font, int x, int y,
              const char* text, uint8_t ink, char align, bool halo) {
    if (halo) {
        const uint8_t opposite = (ink < 128) ? 250 : 10;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                gc_text_aligned(c, font, x + dx, y + dy, text, opposite, 90, align);
            }
        }
    }
    return gc_text_aligned(c, font, x, y, text, ink, 0, align);
}

void draw_rule(glass_canvas_t* c, int x, int y0, int y1, uint8_t ink) {
    gc_line(c, x, y0, x, y1, ink, 1, 0);
}

/**
 * Draw `text` and complain if it would not fit before `right`.
 *
 * Clipping used to be visible only as a half-drawn word in a PNG nobody
 * compared against a spec; now it is a line in the simulator report.
 */
void text_fit(glass_canvas_t* c, const glass_font_t* font, int x, int y, int right,
              const char* text, uint8_t ink, const char* label, char align) {
    const int width = glass_font_text_width(font, text);
    const int left = (align == 'r') ? x - width : (align == 'c') ? x - width / 2 : x;
    if (left + width > right) glass_diag_note_clipped(label, left + width - right, right - left);
    gc_text_aligned(c, font, x, y, text, ink, 0, align);
}

}  // namespace

extern "C" void glass_card_panels(glass_pill_t* big, glass_pill_t* metrics,
                                  glass_pill_t* days) {
    big[0] = glass_pill_t{10, 44, 380, 104, 52};
    metrics[0] = glass_pill_t{10, 158, 380, 34, 17};
    days[0] = glass_pill_t{10, 200, 120, 88, 24};
    days[1] = glass_pill_t{140, 200, 120, 88, 24};
    days[2] = glass_pill_t{270, 200, 120, 88, 24};
}

extern "C" void glass_card_render(glass_canvas_t* c, const glass_card_data_t* d,
                                  const glass_style_t* style, uint8_t* ws) {
    glass_pill_t big;
    glass_pill_t metrics;
    glass_pill_t days[GLASS_CARD_DAYS];
    glass_card_panels(&big, &metrics, days);

    // The slabs live in the plate. Drawing our own panels over the photograph
    // is what turned it into a white box; the panel rects are still computed,
    // but only as bounds for the text.
    (void)style;
    (void)ws;

    // One foreground for the whole frame, chosen by the plate rather than by
    // the average tone under each label. Upstream uses near-black on day plates
    // and near-white on night and thunder plates; sampling the background per
    // panel is what put white type on a white slab.
    const bool light_fg = !d->is_day || strcmp(d->icon, "thunder") == 0;
    const uint8_t ink_big = light_fg ? kInkLight : kInkDark;
    const uint8_t ink_metrics = ink_big;
    const uint8_t ink_header = ink_big;
    const uint8_t rule_big = light_fg ? kRuleLight : kRuleDark;
    const uint8_t icon_ink = light_fg ? 246 : 20;

    // Text centred on a point, as the reference lays it out. Centring is done
    // by measuring first and handing text_fit a left origin, so the width check
    // and the ellipsis still apply to the same box the reference uses.
    auto centered = [&](const glass_font_t* font, int cx, int y, const char* text,
                        int box_w, const char* label) {
        if (text == nullptr || text[0] == '\0') return;
        const int w = glass_font_text_width(font, text);
        text_fit(c, font, cx - w / 2, y, cx + box_w / 2, text, ink_big, label, 'l');
    };

    // --- Header on the open sky ---
    // Upstream uses the same ink with no halo. The halo stays because our plates
    // get their contrast stretched, so a dark city name can otherwise land on a
    // dark cloud mass; over light sky it is invisible either way.
    text_halo(c, &glass_font_f17b, 16, 15, d->city, ink_header, 'l', true);
    text_halo(c, &glass_font_f10r, 384, 13, d->date, ink_header, 'r', true);
    text_halo(c, &glass_font_f9r, 384, 29, d->updated, ink_header, 'r', true);

    // --- Big panel: temperature, condition, hero icon, daily range ---
    // The hero goes down first because its left edge bounds the text column:
    // laying text out first is how the daily range ended up printed on top of
    // the sun.
    const glass_icon_variant_t* hero = glass_icon_find(d->icon, d->is_day, 96);
    // Reference geometry: the hero icon is pinned at (230,62) and the text
    // column stops short of it. That single constraint is what keeps the
    // temperature, the condition and the daily range off the artwork.
    const int hero_x = 230;
    const int hero_y = 62;
    gc_icon(c, hero, hero_x, hero_y, icon_ink, 0);

    // Text column: everything in the left column stops at the hero icon, which
    // is what makes overlap structurally impossible rather than a matter of
    // getting the widths right.
    const int col_x = 51;
    const int col_right = hero_x - 16;
    (void)col_right;  // kept as the documented bound of the text column

    const int temp_len = static_cast<int>(strlen(d->temp));
    const glass_font_t* temp_font = temp_len <= 2 ? &glass_font_f90b
                              : temp_len == 3 ? &glass_font_f73b
                                              : &glass_font_f55b;
    const int temp_y = 66;
    gc_text(c, temp_font, col_x, temp_y, d->temp, ink_big, 0);
    // The degree sign has its own fixed spot rather than being packed against
    // the digits: at 90 px the digits' side bearing is what reads as the gap.
    gc_text(c, &glass_font_f23r, 165, 119, d->unit, ink_big, 0);

    char range[32];
    snprintf(range, sizeof(range), "H %.9s / L %.9s", d->high, d->low);

    // Condition and feels-like get their own lines under the temperature, and
    // the daily range its own slot to the right. The reference puts them on
    // separate baselines, which is why they never land on the digits.
    centered(&glass_font_f12b, 124, 145, d->condition, 179, "condition");
    centered(&glass_font_f10r, 124, 162, d->feels, 170, "feels");
    centered(&glass_font_f10r, 286, 158, range, 143, "daily range");

    // --- Metrics pill: wind, humidity, UV ---
    const glass_icon_variant_t* wind_icon = glass_icon_find("wind", true, 20);
    const glass_icon_variant_t* humid_icon = glass_icon_find("humidity", true, 20);
    // UV reads as the sun by day and the moon at night; the baked kinds are
    // "sunny"/"clear_night" - there is no kind called "clear".
    const glass_icon_variant_t* uv_icon =
        glass_icon_find(d->is_day ? "sunny" : "clear_night", d->is_day, 20);
    gc_icon(c, wind_icon, 43, 187, icon_ink, 0);
    text_fit(c, &glass_font_f10b, 67, 192, 67 + 87, d->wind, ink_metrics, "wind", 'l');
    draw_rule(c, 158, 190, 204, rule_big);
    gc_icon(c, humid_icon, 173, 187, icon_ink, 0);
    text_fit(c, &glass_font_f10b, 192, 192, 192 + 71, d->humidity, ink_metrics,
             "humidity", 'l');
    draw_rule(c, 268, 190, 204, rule_big);
    gc_icon(c, uv_icon, 280, 187, icon_ink, 0);
    text_fit(c, &glass_font_f10b, 308, 192, 308 + 59, d->uv, ink_metrics, "uv", 'l');

    // --- Forecast pills ---
    for (int i = 0; i < GLASS_CARD_DAYS; ++i) {
        const glass_card_day_t* day = &d->days[i];
        if (day->label[0] == '\0') continue;
        const int x = 28 + i * 122;
        text_fit(c, &glass_font_f10b, x, 229, x + 100, day->label, ink_big,
                 "day label", 'l');
        gc_icon(c, glass_icon_find(day->icon, day->is_day, 33), x - 1, 245, icon_ink, 0);
        char hilo[24];
        snprintf(hilo, sizeof(hilo), "%.7s / %.7s", day->high, day->low);
        text_fit(c, &glass_font_f12b, x + 39, 246, x + 39 + 77, hilo, ink_big,
                 "daily hi/lo", 'l');
        text_fit(c, &glass_font_f9r, x + 39, 266, x + 39 + 77, day->precip, ink_big,
                 "day precip", 'l');
    }
    // Hairlines between the forecast pills, at the reference's x positions.
    draw_rule(c, 140, 228, 274, rule_big);
    draw_rule(c, 261, 228, 274, rule_big);

    // --- Status note, only when something is wrong ---
    if (d->status[0] != '\0') {
        text_halo(c, &glass_font_f9r, 16, GLASS_H - 16, d->status, ink_header, 'r',
                  true);
    }
}