/**
 * 400x300 8-bit grayscale canvas with a per-pixel dither weight.
 *
 * The panel only accepts 16 levels, so every layer is composited at 8 bits and
 * quantized once at the end. The weight plane tells the quantizer how much
 * dithering each pixel wants: smooth sky wants a lot (gradients band
 * otherwise), text and glass rims want none (dither makes them fuzzy).
 *
 * No ESP-IDF or OS dependencies: the same sources build for the host
 * simulator (see sim/) and for the firmware.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "glass_font.h"
#include "glass_icons.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GLASS_W 400
#define GLASS_H 300
#define GLASS_PIXELS (GLASS_W * GLASS_H)

/** Scratch bytes required by glass_canvas_t: one full plane. */
#define GLASS_SCRATCH_BYTES GLASS_PIXELS

typedef struct {
    /** GLASS_PIXELS bytes of 8-bit luminance. */
    uint8_t* px;
    /** GLASS_PIXELS bytes of dither weight, 0 = snap, 255 = dither fully. */
    uint8_t* weight;
    /** Caller-owned scratch plane, GLASS_SCRATCH_BYTES. */
    uint8_t* scratch;
} glass_canvas_t;

void glass_canvas_init(glass_canvas_t* canvas, uint8_t* px, uint8_t* weight,
                       uint8_t* scratch);

/** Flat fill. `weight` < 0 leaves the weight plane untouched. */
void gc_fill(glass_canvas_t* c, int x, int y, int w, int h, uint8_t v, int weight);

/** Blend toward `v` by alpha 0..255. */
void gc_blend(glass_canvas_t* c, int x, int y, int w, int h, uint8_t v,
              int alpha, int weight);

/** Additive/subtractive pass with clamping. */
void gc_add(glass_canvas_t* c, int x, int y, int w, int h, int delta, int weight);

/** Vertical gradient from v_top to v_bottom over h rows. */
void gc_vgradient(glass_canvas_t* c, int x, int y, int w, int h, int v_top,
                  int v_bottom, int weight);

/** Radial additive falloff; radius in pixels, delta at the centre. */
void gc_radial(glass_canvas_t* c, int cx, int cy, int radius, int delta,
               int weight);

/** Blend a single pixel with coverage. */
void gc_dot(glass_canvas_t* c, int x, int y, int coverage, uint8_t v, int weight);

/** Anti-aliased line. */
void gc_line(glass_canvas_t* c, int x0, int y0, int x1, int y1, uint8_t v,
             int thickness, int weight);

/**
 * Write an anti-aliased rounded-rect coverage mask into `out` (w*h bytes).
 * `radius` is clamped to min(w,h)/2 so a full-height radius gives a pill.
 */
void gc_round_mask(uint8_t* out, int w, int h, int radius);

/** Composite a coverage mask: blend toward `v` scaled by alpha. */
void gc_blend_mask(glass_canvas_t* c, int x, int y, int w, int h,
                   const uint8_t* mask, uint8_t v, int alpha, int weight);

/** Add `delta * coverage / 255` per pixel. */
void gc_add_mask(glass_canvas_t* c, int x, int y, int w, int h,
                 const uint8_t* mask, int delta, int weight);

/** Take the maximum coverage of a mask into the mask plane (union). */
void gc_or_mask(uint8_t* dst, const uint8_t* src, int w, int h);

/**
 * Separable box blur over a rectangle, `passes` times (3 approximates a
 * Gaussian). `dx`/`dy` shift the sampling position, which the glass layer uses
 * for its lens squeeze.
 */
void gc_blur(glass_canvas_t* c, int x, int y, int w, int h, int radius,
             int passes, int dx_num, int dy_num, int scale_num);

/** Read a pixel with clamped coordinates. */
uint8_t gc_get(const glass_canvas_t* c, int x, int y);

/* ---- layout diagnostics -------------------------------------------------
 *
 * Cheap counters that turn "the render looks wrong" into a line of text. The
 * simulator resets them before each frame and prints whatever was noted, so a
 * missing glyph, an icon that fell back to the question mark, or a string that
 * does not fit its pill fails loudly instead of shipping as a weird picture.
 * The firmware can ignore them; they are a handful of ints.
 */
typedef struct {
    int missing_glyphs;   /**< Codepoints with no baked glyph. */
    int icon_fallbacks;   /**< Icon lookups that landed on "unknown". */
    int clipped_text;     /**< Strings wider than the space allowed. */
} glass_diag_t;

#define GLASS_DIAG_MAX 8
#define GLASS_DIAG_LEN 96

void glass_diag_reset(void);
void glass_diag_get(glass_diag_t* out);
/** Copy up to GLASS_DIAG_MAX human-readable lines out; returns how many. */
int glass_diag_messages(char out[GLASS_DIAG_MAX][GLASS_DIAG_LEN]);
/** Record a codepoint the face does not have. */
void glass_diag_note_missing_glyph(int32_t codepoint);
/** Record an icon kind that had to be answered with the unknown mask. */
void glass_diag_note_icon_fallback(const char* kind, int want);
/** Record a string that needed `needed` px in `available` px. */
void glass_diag_note_clipped(const char* what, int needed, int available);

/** Multiply the weight plane inside a rect (used to soften glass dithering). */
void gc_scale_weight(glass_canvas_t* c, int x, int y, int w, int h, int scale);

/** Copy a rectangle of the canvas into a caller buffer (row stride = w). */
void gc_copy_out(const glass_canvas_t* c, int x, int y, int w, int h,
                 uint8_t* dst);

/** Restore a rectangle from a caller buffer. */
void gc_copy_in(glass_canvas_t* c, int x, int y, int w, int h,
                const uint8_t* src, const uint8_t* weight_src);

/* ---- text and icons ---------------------------------------------------- */

/** Draw UTF-8 text with the pen at the top-left of the text box. */
int gc_text(glass_canvas_t* c, const glass_font_t* font, int x, int y,
            const char* text, uint8_t color, int weight);

/** Draw right-aligned or centred; returns the x the text started at. */
int gc_text_aligned(glass_canvas_t* c, const glass_font_t* font, int x, int y,
                    const char* text, uint8_t color, int weight, char align);

/** Draw an icon mask with its top-left at x,y. */
void gc_icon(glass_canvas_t* c, const glass_icon_variant_t* icon, int x, int y,
             uint8_t color, int weight);

/* ---- quantization ------------------------------------------------------ */

typedef enum {
    GLASS_DITHER_NONE = 0,  /**< Straight quantization (what Zectrix ships). */
    GLASS_DITHER_BAYER8,    /**< 8x8 ordered dither, weighted. */
    GLASS_DITHER_FS,        /**< Floyd-Steinberg error diffusion, weighted. */
} glass_dither_mode_t;

/**
 * Quantize the canvas to 16 levels and pack two pixels per byte, left pixel in
 * the high nibble, 0 = black and 15 = white - the layout the SSD2683 4bpp
 * refresh expects (ZECTRIX_EPD_4BPP_FRAME_BYTES).
 */
void glass_quantize(const glass_canvas_t* c, uint8_t* out4bpp, size_t out_size,
                    glass_dither_mode_t mode, int strength);

#ifdef __cplusplus
}
#endif