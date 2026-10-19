/**
 * Weather icon masks. Data lives in generated/glass_assets.cc.
 *
 * Each icon is a square 8-bit alpha mask rasterized from the vendored vector
 * geometry; the caller supplies the ink colour.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Mask edge length in pixels. */
    uint16_t size;
    /** size*size coverage bytes, row-major. */
    const uint8_t* coverage;
} glass_icon_variant_t;

typedef struct {
    /** Kind name, e.g. "partly_cloudy". */
    const char* kind;
    /** "day" or "night". */
    const char* suffix;
    uint8_t variant_count;
    const glass_icon_variant_t* variants;
} glass_icon_t;

extern const glass_icon_t glass_icons[];
extern const size_t glass_icon_count;

/**
 * Find the mask for a kind/day combination, nearest in size to `want`.
 * Falls back to "unknown" when the kind is not baked.
 */
const glass_icon_variant_t* glass_icon_find(const char* kind, bool day, int want);

#ifdef __cplusplus
}
#endif