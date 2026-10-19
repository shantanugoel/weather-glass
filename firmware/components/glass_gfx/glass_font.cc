#include "glass_canvas.h"

#include <string.h>

#include "generated/glass_assets.h"

namespace {
// Box drawn for a codepoint we never baked, so a bad string is visible rather
// than silently blank.
const uint8_t kFallbackCoverage[8 * 6] = {
    0,   220, 220, 220, 220, 220, 220, 0,
    0,   220, 0,   0,   0,   0,   220, 0,
    0,   220, 0,   0,   0,   0,   220, 0,
    0,   220, 0,   0,   0,   0,   220, 0,
    0,   220, 220, 220, 220, 220, 220, 0,
    0,   0,   0,   0,   0,   0,   0,   0,
};
const glass_glyph_t kFallback = {0xFFFD, 8, 6, 0, 4, 9, kFallbackCoverage};

/** Variant whose size is closest to `want`; never blindly the first one. */
const glass_icon_variant_t* nearest(const glass_icon_t* entry, int want) {
    if (entry == nullptr || entry->variant_count <= 0) return nullptr;
    const glass_icon_variant_t* best = &entry->variants[0];
    int best_gap = 1 << 30;
    for (int v = 0; v < entry->variant_count; ++v) {
        int gap = entry->variants[v].size - want;
        if (gap < 0) gap = -gap;
        if (gap < best_gap) {
            best_gap = gap;
            best = &entry->variants[v];
        }
    }
    return best;
}
}  // namespace

extern "C" const glass_glyph_t* glass_font_glyph(const glass_font_t* font,
                                                 int32_t codepoint) {
    if (font == nullptr || font->glyphs == nullptr) return nullptr;
    // Baked codepoints are dense and sorted; a short linear scan beats a
    // binary search at this size and keeps the table layout simple.
    for (int i = 0; i < font->count; ++i) {
        if (font->glyphs[i].codepoint == codepoint) return &font->glyphs[i];
    }
    // A string the face cannot draw is a build error, not a rendering choice:
    // say so where the simulator can see it.
    if (codepoint != 0xFFFD) glass_diag_note_missing_glyph(codepoint);
    return &kFallback;
}

extern "C" int glass_font_text_width(const glass_font_t* font, const char* text) {
    const size_t len = strlen(text);
    size_t i = 0;
    int width = 0;
    while (i < len) {
        const uint8_t c = static_cast<uint8_t>(text[i]);
        int32_t cp;
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < len) {
            cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(text[i + 1]) & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len) {
            cp = ((c & 0x0F) << 12) |
                 ((static_cast<uint8_t>(text[i + 1]) & 0x3F) << 6) |
                 (static_cast<uint8_t>(text[i + 2]) & 0x3F);
            i += 3;
        } else {
            cp = c;
            i += 1;
        }
        const glass_glyph_t* g = glass_font_glyph(font, cp);
        width += (g != nullptr) ? g->advance : 0;
    }
    return width;
}

extern "C" const glass_icon_variant_t* glass_icon_find(const char* kind, bool day,
                                                       int want) {
    if (kind == nullptr) kind = "unknown";
    const char* suffix = day ? "day" : "night";
    const glass_icon_t* other_suffix = nullptr;
    const glass_icon_t* unknown = nullptr;
    for (size_t i = 0; i < glass_icon_count; ++i) {
        const glass_icon_t* entry = &glass_icons[i];
        if (strcmp(entry->kind, "unknown") == 0 &&
            strcmp(entry->suffix, suffix) == 0) {
            unknown = entry;
        }
        if (strcmp(entry->kind, kind) != 0) continue;
        if (strcmp(entry->suffix, suffix) != 0) {
            if (other_suffix == nullptr) other_suffix = entry;
            continue;
        }
        // A kind we baked at this time of day: take the size closest to what
        // was asked for. Picking variants[0] here handed the metrics row the
        // 103 px hero mask and a 96 px question mark landed on the card.
        const glass_icon_variant_t* hit = nearest(entry, want);
        if (hit != nullptr) return hit;
    }
    // Wrong time of day beats a question mark: the shapes are the same family.
    const glass_icon_variant_t* alt = nearest(other_suffix, want);
    if (alt != nullptr) return alt;
    // Nothing at all matched the kind. Landing on the question mark is the
    // usual symptom of a typo in a kind name, so report it.
    glass_diag_note_icon_fallback(kind, want);
    alt = nearest(unknown, want);
    return alt;
}