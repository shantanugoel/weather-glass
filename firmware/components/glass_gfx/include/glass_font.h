/**
 * Baked font face types. Data lives in generated/glass_assets.cc.
 *
 * Glyph coverage is 8-bit (0 = untouched, 255 = full ink) so text composites
 * cleanly over a grayscale background and survives the 16-level quantizer.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Unicode codepoint (only BMP codepoints are baked). */
    int32_t codepoint;
    /** Ink box size in pixels; both zero for blank glyphs such as space. */
    uint8_t w;
    uint8_t h;
    /** Ink box offset from the text pen position (top-left origin). */
    int8_t left;
    int8_t top;
    /** Pen advance in pixels. */
    uint8_t advance;
    /** w*h coverage bytes, row-major, or nullptr when blank. */
    const uint8_t* coverage;
} glass_glyph_t;

typedef struct {
    const glass_glyph_t* glyphs;
    uint16_t count;
    /** Requested pixel size; also the nominal line height. */
    uint8_t size;
    uint8_t ascent;
    uint8_t descent;
} glass_font_t;

/** Look up a glyph, or the fallback box for a missing codepoint. */
const glass_glyph_t* glass_font_glyph(const glass_font_t* font, int32_t codepoint);

/** Pixel width of a UTF-8 string, including the trailing pen advance. */
int glass_font_text_width(const glass_font_t* font, const char* text);

#ifdef __cplusplus
}
#endif