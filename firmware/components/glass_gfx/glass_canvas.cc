#include "glass_canvas.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

/** Blend one pixel toward v by alpha (0..255) and update its weight. */
inline void blend_px(uint8_t* px, uint8_t* weight, int index, int v, int alpha,
                     int weight_value) {
    const int a = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
    const int src = px[index];
    px[index] = static_cast<uint8_t>(src + ((v - src) * a + 127) / 255);
    if (weight_value >= 0 && weight != nullptr) {
        const int w = weight_value < 0 ? 0 : (weight_value > 255 ? 255 : weight_value);
        // Later layers win, but a soft layer never fully removes an existing
        // weight: take the larger of "this layer's weight" and "what is left
        // after this layer's coverage".
        const int keep = (weight[index] * (255 - a)) / 255;
        weight[index] = clamp8(w > keep ? w : keep);
    }
}

/** Add delta to one pixel with clamping. */
inline void add_px(uint8_t* px, uint8_t* weight, int index, int delta,
                   int weight_value) {
    px[index] = clamp8(px[index] + delta);
    if (weight_value >= 0 && weight != nullptr) {
        weight[index] = clamp8(weight_value);
    }
}

/** Next UTF-8 codepoint, advancing *i. Returns 0 at end of string. */
int32_t next_cp(const char* s, size_t len, size_t* i) {
    if (*i >= len) return 0;
    const uint8_t c = static_cast<uint8_t>(s[*i]);
    if (c < 0x80) {
        *i += 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && *i + 1 < len) {
        const int32_t cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(s[*i + 1]) & 0x3F);
        *i += 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0 && *i + 2 < len) {
        const int32_t cp = ((c & 0x0F) << 12) |
                           ((static_cast<uint8_t>(s[*i + 1]) & 0x3F) << 6) |
                           (static_cast<uint8_t>(s[*i + 2]) & 0x3F);
        *i += 3;
        return cp;
    }
    *i += 1;
    return c;
}

// Standard 8x8 Bayer matrix / 64, kept as integers in 1/64ths.
const uint8_t kBayer8[64] = {
     0, 32,  8, 40,  2, 34, 10, 42,
    48, 16, 56, 24, 50, 18, 58, 26,
    12, 44,  4, 36, 14, 46,  6, 38,
    60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43,  1, 33,  9, 41,
    51, 19, 59, 27, 49, 17, 57, 25,
    15, 47,  7, 39, 13, 45,  5, 37,
    63, 31, 55, 23, 61, 29, 53, 21,
};

/** One separable box-blur pass over scratch (absolute canvas coordinates). */
void box_pass(uint8_t* scratch, int x, int y, int w, int h, int radius,
              bool horizontal) {
    const int span = 2 * radius + 1;
    if (horizontal) {
        for (int j = 0; j < h; ++j) {
            const int row = (y + j) * GLASS_W;
            int sum = 0;
            for (int k = -radius; k <= radius; ++k) {
                sum += scratch[row + clampi(x + k, x, x + w - 1)];
            }
            for (int i = 0; i < w; ++i) {
                scratch[row + x + i] = static_cast<uint8_t>((sum + radius) / span);
                const int add_x = clampi(x + i + radius + 1, x, x + w - 1);
                const int sub_x = clampi(x + i - radius, x, x + w - 1);
                sum += scratch[row + add_x] - scratch[row + sub_x];
            }
        }
        return;
    }
    for (int i = 0; i < w; ++i) {
        const int col = x + i;
        int sum = 0;
        for (int k = -radius; k <= radius; ++k) {
            sum += scratch[(clampi(y + k, y, y + h - 1)) * GLASS_W + col];
        }
        for (int j = 0; j < h; ++j) {
            scratch[(y + j) * GLASS_W + col] = static_cast<uint8_t>((sum + radius) / span);
            const int add_y = clampi(y + j + radius + 1, y, y + h - 1);
            const int sub_y = clampi(y + j - radius, y, y + h - 1);
            sum += scratch[add_y * GLASS_W + col] - scratch[sub_y * GLASS_W + col];
        }
    }
}

}  // namespace

void glass_canvas_init(glass_canvas_t* canvas, uint8_t* px, uint8_t* weight,
                       uint8_t* scratch) {
    canvas->px = px;
    canvas->weight = weight;
    canvas->scratch = scratch;
}

void gc_fill(glass_canvas_t* c, int x, int y, int w, int h, uint8_t v, int weight) {
    const int x0 = clampi(x, 0, GLASS_W);
    const int y0 = clampi(y, 0, GLASS_H);
    const int x1 = clampi(x + w, 0, GLASS_W);
    const int y1 = clampi(y + h, 0, GLASS_H);
    for (int j = y0; j < y1; ++j) {
        for (int i = x0; i < x1; ++i) {
            const int index = j * GLASS_W + i;
            c->px[index] = v;
            if (weight >= 0) c->weight[index] = clamp8(weight);
        }
    }
}

void gc_blend(glass_canvas_t* c, int x, int y, int w, int h, uint8_t v, int alpha,
              int weight) {
    const int x0 = clampi(x, 0, GLASS_W);
    const int y0 = clampi(y, 0, GLASS_H);
    const int x1 = clampi(x + w, 0, GLASS_W);
    const int y1 = clampi(y + h, 0, GLASS_H);
    for (int j = y0; j < y1; ++j) {
        for (int i = x0; i < x1; ++i) {
            blend_px(c->px, c->weight, j * GLASS_W + i, v, alpha, weight);
        }
    }
}

void gc_add(glass_canvas_t* c, int x, int y, int w, int h, int delta, int weight) {
    const int x0 = clampi(x, 0, GLASS_W);
    const int y0 = clampi(y, 0, GLASS_H);
    const int x1 = clampi(x + w, 0, GLASS_W);
    const int y1 = clampi(y + h, 0, GLASS_H);
    for (int j = y0; j < y1; ++j) {
        for (int i = x0; i < x1; ++i) {
            add_px(c->px, c->weight, j * GLASS_W + i, delta, weight);
        }
    }
}

void gc_vgradient(glass_canvas_t* c, int x, int y, int w, int h, int v_top,
                  int v_bottom, int weight) {
    const int x0 = clampi(x, 0, GLASS_W);
    const int y0 = clampi(y, 0, GLASS_H);
    const int x1 = clampi(x + w, 0, GLASS_W);
    const int y1 = clampi(y + h, 0, GLASS_H);
    for (int j = y0; j < y1; ++j) {
        const int t = h > 1 ? (j - y) * 256 / (h - 1) : 0;
        const int v = v_top + ((v_bottom - v_top) * clampi(t, 0, 256) + 128) / 256;
        for (int i = x0; i < x1; ++i) {
            const int index = j * GLASS_W + i;
            c->px[index] = clamp8(v);
            if (weight >= 0) c->weight[index] = clamp8(weight);
        }
    }
}

void gc_radial(glass_canvas_t* c, int cx, int cy, int radius, int delta, int weight) {
    const int x0 = clampi(cx - radius, 0, GLASS_W);
    const int y0 = clampi(cy - radius, 0, GLASS_H);
    const int x1 = clampi(cx + radius + 1, 0, GLASS_W);
    const int y1 = clampi(cy + radius + 1, 0, GLASS_H);
    const int r2 = radius * radius;
    for (int j = y0; j < y1; ++j) {
        const int dy = j - cy;
        for (int i = x0; i < x1; ++i) {
            const int dx = i - cx;
            const int d2 = dx * dx + dy * dy;
            if (d2 > r2) continue;
            // Smoothstep falloff: no hard ring, no visible disc edge.
            const int t = (r2 - d2) * 256 / (r2 > 0 ? r2 : 1);
            const int s = (t * t * (768 - 2 * t) + 65536) >> 16;  // t^2*(3-2t)
            const int index = j * GLASS_W + i;
            add_px(c->px, c->weight, index, (delta * s) / 255, weight);
        }
    }
}

void gc_dot(glass_canvas_t* c, int x, int y, int coverage, uint8_t v, int weight) {
    if (x < 0 || y < 0 || x >= GLASS_W || y >= GLASS_H || coverage <= 0) return;
    blend_px(c->px, c->weight, y * GLASS_W + x, v, coverage, weight);
}

void gc_line(glass_canvas_t* c, int x0, int y0, int x1, int y1, uint8_t v,
             int thickness, int weight) {
    // Xiaolin Wu's algorithm: integer stepping, fractional coverage per pixel.
    const bool steep = abs(y1 - y0) > abs(x1 - x0);
    if (steep) {
        int t = x0; x0 = y0; y0 = t;
        t = x1; x1 = y1; y1 = t;
    }
    if (x0 > x1) {
        int t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    const int dx = x1 - x0;
    const int dy = y1 - y0;
    if (dx == 0 && dy == 0) {
        gc_dot(c, x0, y0, 255, v, weight);
        return;
    }
    const float gradient = dx ? static_cast<float>(dy) / static_cast<float>(dx) : 1.0f;
    float y = static_cast<float>(y0);
    const int half = (thickness - 1) / 2;
    for (int i = x0; i <= x1; ++i) {
        const int iy = static_cast<int>(y);
        const int frac = static_cast<int>((y - static_cast<float>(iy)) * 255.0f);
        for (int k = -half; k <= half; ++k) {
            if (steep) {
                gc_dot(c, iy + k, i, 255 - frac, v, weight);
                gc_dot(c, iy + k + 1, i, frac, v, weight);
            } else {
                gc_dot(c, i, iy + k, 255 - frac, v, weight);
                gc_dot(c, i, iy + k + 1, frac, v, weight);
            }
        }
        y += gradient;
    }
}

void gc_round_mask(uint8_t* out, int w, int h, int radius) {
    const int r = clampi(radius, 0, (w < h ? w : h) / 2);
    const float half_w = static_cast<float>(w) * 0.5f;
    const float half_h = static_cast<float>(h) * 0.5f;
    const float rr = static_cast<float>(r);
    for (int j = 0; j < h; ++j) {
        const float py = static_cast<float>(j) + 0.5f - half_h;
        for (int i = 0; i < w; ++i) {
            const float px = static_cast<float>(i) + 0.5f - half_w;
            const float qx = fabsf(px) - (half_w - rr);
            const float qy = fabsf(py) - (half_h - rr);
            const float ax = qx > 0.0f ? qx : 0.0f;
            const float ay = qy > 0.0f ? qy : 0.0f;
            const float outside = sqrtf(ax * ax + ay * ay);
            const float inside = qx < qy ? qx : qy;
            const float sdf = (qx > 0.0f || qy > 0.0f ? outside : (inside < 0.0f ? inside : 0.0f)) - rr;
            // Coverage: 1 well inside, falling off across one pixel centred on
            // the geometric edge.
            float cov = 0.5f - sdf;
            if (cov <= 0.0f) {
                out[j * w + i] = 0;
            } else if (cov >= 1.0f) {
                out[j * w + i] = 255;
            } else {
                out[j * w + i] = static_cast<uint8_t>(cov * 255.0f + 0.5f);
            }
        }
    }
}

void gc_blend_mask(glass_canvas_t* c, int x, int y, int w, int h,
                   const uint8_t* mask, uint8_t v, int alpha, int weight) {
    const int x0 = clampi(x, 0, GLASS_W);
    const int y0 = clampi(y, 0, GLASS_H);
    const int x1 = clampi(x + w, 0, GLASS_W);
    const int y1 = clampi(y + h, 0, GLASS_H);
    for (int j = y0; j < y1; ++j) {
        const int mj = j - y;
        for (int i = x0; i < x1; ++i) {
            const int cov = mask[mj * w + (i - x)];
            if (cov == 0) continue;
            blend_px(c->px, c->weight, j * GLASS_W + i, v, (cov * alpha) / 255, weight);
        }
    }
}

void gc_add_mask(glass_canvas_t* c, int x, int y, int w, int h,
                 const uint8_t* mask, int delta, int weight) {
    const int x0 = clampi(x, 0, GLASS_W);
    const int y0 = clampi(y, 0, GLASS_H);
    const int x1 = clampi(x + w, 0, GLASS_W);
    const int y1 = clampi(y + h, 0, GLASS_H);
    for (int j = y0; j < y1; ++j) {
        const int mj = j - y;
        for (int i = x0; i < x1; ++i) {
            const int cov = mask[mj * w + (i - x)];
            if (cov == 0) continue;
            add_px(c->px, c->weight, j * GLASS_W + i, (delta * cov) / 255, weight);
        }
    }
}

void gc_or_mask(uint8_t* dst, const uint8_t* src, int w, int h) {
    for (int i = 0; i < w * h; ++i) {
        if (src[i] > dst[i]) dst[i] = src[i];
    }
}

void gc_blur(glass_canvas_t* c, int x, int y, int w, int h, int radius,
             int passes, int dx_num, int dy_num, int scale_num) {
    const int x0 = clampi(x, 0, GLASS_W);
    const int y0 = clampi(y, 0, GLASS_H);
    const int x1 = clampi(x + w, 0, GLASS_W);
    const int y1 = clampi(y + h, 0, GLASS_H);
    const int rw = x1 - x0;
    const int rh = y1 - y0;
    if (rw <= 0 || rh <= 0) return;

    // Blur a copy so the lens resample reads a stable source.
    for (int j = 0; j < rh; ++j) {
        memcpy(&c->scratch[(y0 + j) * GLASS_W + x0],
               &c->px[(y0 + j) * GLASS_W + x0], rw);
    }
    for (int p = 0; p < passes; ++p) {
        box_pass(c->scratch, x0, y0, rw, rh, radius, true);
        box_pass(c->scratch, x0, y0, rw, rh, radius, false);
    }

    if (scale_num == 256 && dx_num == 0 && dy_num == 0) {
        for (int j = y0; j < y1; ++j) {
            memcpy(&c->px[j * GLASS_W + x0], &c->scratch[j * GLASS_W + x0], rw);
        }
        return;
    }

    // Resample with a scale about the rect centre (the glass lens squeeze) and
    // bilinear filtering, so the squeeze does not stair-step.
    const int cx2 = x0 * 2 + rw;
    const int cy2 = y0 * 2 + rh;
    const int scale = scale_num > 0 ? scale_num : 256;
    for (int j = y0; j < y1; ++j) {
        const int sy_raw = (cy2 + ((2 * j - cy2) * scale) / 256) + dy_num;
        const int syi = sy_raw >> 8;
        const int syf = sy_raw & 255;
        const int s0 = clampi(syi, y0, y1 - 1);
        const int s1 = clampi(syi + 1, y0, y1 - 1);
        for (int i = x0; i < x1; ++i) {
            const int sx_raw = (cx2 + ((2 * i - cx2) * scale) / 256) + dx_num;
            const int sxi = sx_raw >> 8;
            const int sxf = sx_raw & 255;
            const int s0x = clampi(sxi, x0, x1 - 1);
            const int s1x = clampi(sxi + 1, x0, x1 - 1);
            const int a = c->scratch[s0 * GLASS_W + s0x];
            const int b = c->scratch[s0 * GLASS_W + s1x];
            const int d = c->scratch[s1 * GLASS_W + s0x];
            const int e = c->scratch[s1 * GLASS_W + s1x];
            const int top = a + ((b - a) * sxf) / 256;
            const int bot = d + ((e - d) * sxf) / 256;
            c->px[j * GLASS_W + i] = static_cast<uint8_t>(top + ((bot - top) * syf) / 256);
        }
    }
}

uint8_t gc_get(const glass_canvas_t* c, int x, int y) {
    return c->px[clampi(y, 0, GLASS_H - 1) * GLASS_W + clampi(x, 0, GLASS_W - 1)];
}

void gc_scale_weight(glass_canvas_t* c, int x, int y, int w, int h, int scale) {
    const int x0 = clampi(x, 0, GLASS_W);
    const int y0 = clampi(y, 0, GLASS_H);
    const int x1 = clampi(x + w, 0, GLASS_W);
    const int y1 = clampi(y + h, 0, GLASS_H);
    for (int j = y0; j < y1; ++j) {
        for (int i = x0; i < x1; ++i) {
            const int index = j * GLASS_W + i;
            c->weight[index] = clamp8((c->weight[index] * scale) / 256);
        }
    }
}

void gc_copy_out(const glass_canvas_t* c, int x, int y, int w, int h, uint8_t* dst) {
    for (int j = 0; j < h; ++j) {
        memcpy(dst + j * w, &c->px[y * GLASS_W + x], w);
    }
}

void gc_copy_in(glass_canvas_t* c, int x, int y, int w, int h, const uint8_t* src,
                const uint8_t* weight_src) {
    for (int j = 0; j < h; ++j) {
        memcpy(&c->px[(y + j) * GLASS_W + x], src + j * w, w);
        if (weight_src != nullptr) {
            memcpy(&c->weight[(y + j) * GLASS_W + x], weight_src + j * w, w);
        }
    }
}

namespace {

/* Diagnostics state. Written from the draw calls, read by the simulator. Bounded
 * so a runaway string cannot flood the report. */
glass_diag_t s_diag;
char s_messages[GLASS_DIAG_MAX][GLASS_DIAG_LEN];
int s_message_n = 0;

void diag_note(const char* fmt, ...) {
    if (s_message_n >= GLASS_DIAG_MAX) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_messages[s_message_n], GLASS_DIAG_LEN, fmt, ap);
    va_end(ap);
    ++s_message_n;
}

}  // namespace

extern "C" void glass_diag_reset(void) {
    s_diag = glass_diag_t{};
    s_message_n = 0;
}

extern "C" void glass_diag_get(glass_diag_t* out) {
    if (out != nullptr) *out = s_diag;
}

extern "C" int glass_diag_messages(char out[GLASS_DIAG_MAX][GLASS_DIAG_LEN]) {
    for (int i = 0; i < s_message_n; ++i) {
        memcpy(out[i], s_messages[i], GLASS_DIAG_LEN);
    }
    return s_message_n;
}

extern "C" void glass_diag_note_missing_glyph(int32_t codepoint) {
    ++s_diag.missing_glyphs;
    bool known = false;
    for (int i = 0; i < s_message_n; ++i) {
        if (strncmp(s_messages[i], "missing glyph U+", 14) == 0) {
            char want[24];
            snprintf(want, sizeof(want), "missing glyph U+%04X",
                     static_cast<unsigned>(codepoint));
            if (strcmp(s_messages[i], want) == 0) known = true;
        }
    }
    if (!known) diag_note("missing glyph U+%04X", static_cast<unsigned>(codepoint));
}

extern "C" void glass_diag_note_icon_fallback(const char* kind, int want) {
    ++s_diag.icon_fallbacks;
    diag_note("icon \"%s\" @%dpx fell back to the unknown mask",
              kind == nullptr ? "(null)" : kind, want);
}

extern "C" void glass_diag_note_clipped(const char* what, int needed, int available) {
    ++s_diag.clipped_text;
    diag_note("%s needs %dpx, has %dpx", what == nullptr ? "text" : what, needed,
              available);
}

int gc_text(glass_canvas_t* c, const glass_font_t* font, int x, int y,
            const char* text, uint8_t color, int weight) {
    const size_t len = strlen(text);
    size_t i = 0;
    int pen = x;
    while (i < len) {
        const int32_t cp = next_cp(text, len, &i);
        if (cp == '\n') {
            pen = x;
            y += font->size + 2;
            continue;
        }
        const glass_glyph_t* g = glass_font_glyph(font, cp);
        if (g == nullptr) continue;
        if (g->w == 0 || g->h == 0) {
            pen += g->advance;
            continue;
        }
        const int gx = pen + g->left;
        const int gy = y + g->top;
        for (int j = 0; j < g->h; ++j) {
            for (int k = 0; k < g->w; ++k) {
                const int cov = g->coverage[j * g->w + k];
                if (cov == 0) continue;
                gc_dot(c, gx + k, gy + j, cov, color, weight);
            }
        }
        pen += g->advance;
    }
    return pen - x;
}

int gc_text_aligned(glass_canvas_t* c, const glass_font_t* font, int x, int y,
                    const char* text, uint8_t color, int weight, char align) {
    const int width = glass_font_text_width(font, text);
    int sx = x;
    if (align == 'r') sx = x - width;
    if (align == 'c') sx = x - width / 2;
    gc_text(c, font, sx, y, text, color, weight);
    return sx;
}

void gc_icon(glass_canvas_t* c, const glass_icon_variant_t* icon, int x, int y,
             uint8_t color, int weight) {
    if (icon == nullptr) return;
    for (int j = 0; j < icon->size; ++j) {
        for (int i = 0; i < icon->size; ++i) {
            const int cov = icon->coverage[j * icon->size + i];
            if (cov == 0) continue;
            gc_dot(c, x + i, y + j, cov, color, weight);
        }
    }
}

void glass_quantize(const glass_canvas_t* c, uint8_t* out4bpp, size_t out_size,
                    glass_dither_mode_t mode, int strength) {
    const size_t need = GLASS_PIXELS / 2;
    if (out4bpp == nullptr || out_size < need) return;
    const int s = clampi(strength, 0, 255);
    memset(out4bpp, 0xFF, need);

    auto level_of = [](int v) -> int {
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return (v + 8) / 17;  // 0..15, 255 -> 15
    };

    if (mode == GLASS_DITHER_FS) {
        // Serpentine Floyd-Steinberg. Error is scaled by the weight plane so
        // crisp layers stay crisp. Only two rows of error are live, so the
        // scratch plane is used as the two row buffers.
        int32_t* err_cur = reinterpret_cast<int32_t*>(c->scratch);
        int32_t* err_next = err_cur + GLASS_W;
        for (int i = 0; i < GLASS_W; ++i) {
            err_cur[i] = 0;
            err_next[i] = 0;
        }
        // Errors are carried in 1/256ths of an 8-bit unit.
        for (int j = 0; j < GLASS_H; ++j) {
            const bool ltr = (j & 1) == 0;
            for (int k = 0; k < GLASS_W; ++k) {
                const int i = ltr ? k : (GLASS_W - 1 - k);
                const int index = j * GLASS_W + i;
                const int w = c->weight[index];
                const int scaled = static_cast<int>(
                    (err_cur[i] * w * s) / (255 * 255));
                const int lv = level_of(c->px[index] + scaled);
                const int q = lv * 17;
                const int diff = (c->px[index] + scaled - q) * 256;
                const int dir = ltr ? 1 : -1;
                if (i + dir >= 0 && i + dir < GLASS_W) {
                    err_cur[i + dir] += (diff * 7) / 16;
                }
                if (j + 1 < GLASS_H) {
                    if (i - dir >= 0) err_next[i - dir] += (diff * 3) / 16;
                    err_next[i] += (diff * 5) / 16;
                    if (i + dir < GLASS_W) err_next[i + dir] += (diff * 1) / 16;
                }
                if ((i & 1) == 0) {
                    out4bpp[index >> 1] = static_cast<uint8_t>(lv << 4);
                } else {
                    out4bpp[index >> 1] = static_cast<uint8_t>((out4bpp[index >> 1] & 0xF0) | lv);
                }
            }
            for (int i = 0; i < GLASS_W; ++i) {
                err_cur[i] = err_next[i];
                err_next[i] = 0;
            }
        }
        return;
    }

    for (int j = 0; j < GLASS_H; ++j) {
        for (int i = 0; i < GLASS_W; ++i) {
            const int index = j * GLASS_W + i;
            int v = c->px[index];
            if (mode == GLASS_DITHER_BAYER8) {
                // Ordered dither at +/-0.5 of a panel level when weight and
                // strength are both maxed: enough to break gradient banding
                // without turning flat areas into visible noise.
                const int w = c->weight[index];
                const int b = kBayer8[(j & 7) * 8 + (i & 7)];
                v += ((b - 32) * 17 * w * s) / 4161600;
            }
            const int lv = level_of(v);
            if ((i & 1) == 0) {
                out4bpp[index >> 1] = static_cast<uint8_t>(lv << 4);
            } else {
                out4bpp[index >> 1] = static_cast<uint8_t>((out4bpp[index >> 1] & 0xF0) | lv);
            }
        }
    }
}