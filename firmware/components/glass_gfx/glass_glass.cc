#include "glass_glass.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int iabs(int v) { return v < 0 ? -v : v; }

/** Signed distance to a rounded rect of size w*h centred on its own origin. */
float sdf_round(float px, float py, float w, float h, float r) {
    const float qx = fabsf(px) - (w * 0.5f - r);
    const float qy = fabsf(py) - (h * 0.5f - r);
    const float ax = qx > 0.0f ? qx : 0.0f;
    const float ay = qy > 0.0f ? qy : 0.0f;
    const float outside = sqrtf(ax * ax + ay * ay);
    const float inside = qx < qy ? qx : qy;
    const float major = (qx > 0.0f || qy > 0.0f) ? outside : (inside < 0.0f ? inside : 0.0f);
    return major - r;
}

}  // namespace

extern "C" void glass_style_default(glass_style_t* style) {
    // The lift is what makes it read as glass or as a sticky note. At 108 the
    // panel is ~60% white over any sky and the background disappears - the
    // blurred backdrop has to survive, so the lift stays modest and the
    // top-lit gradient does most of the shaping.
    style->lift = 66;
    style->lift_top = 62;
    style->blur_radius = 6;
    style->blur_passes = 3;
    style->squeeze = 246;
    // With a translucent panel the shadow is what separates it from the sky.
    style->shadow = 92;
    style->rim = 112;
    style->sheen = 26;
    style->weight = 74;
}

extern "C" void glass_pill_draw(glass_canvas_t* c, const glass_pill_t* pill,
                                const glass_style_t* st, uint8_t* mask_buf) {
    const int w = pill->w;
    const int h = pill->h;
    if (w <= 0 || h <= 0 || mask_buf == nullptr) return;
    const int radius = clampi(pill->radius, 0, (w < h ? w : h) / 2);

    gc_round_mask(mask_buf, w, h, radius);

    // 1. Drop shadow: the panel's own distance field, sampled 4 px above its
    //    real position, so the falloff lands below it.
    if (st->shadow > 0) {
        const int pad = 14;
        const float half_w = static_cast<float>(w) * 0.5f;
        const float half_h = static_cast<float>(h) * 0.5f;
        for (int j = -pad; j < h + pad; ++j) {
            for (int i = -pad; i < w + pad; ++i) {
                if (i >= 0 && i < w && j >= 0 && j < h) continue;  // under the glass
                const float d = sdf_round(static_cast<float>(i) + 0.5f - half_w,
                                          static_cast<float>(j) + 0.5f - half_h - 4.0f,
                                          static_cast<float>(w), static_cast<float>(h),
                                          static_cast<float>(radius));
                const float cov = clampf(1.0f - (d + 1.0f) / 13.0f, 0.0f, 1.0f);
                if (cov <= 0.02f) continue;
                gc_dot(c, pill->x + i, pill->y + j, static_cast<int>(cov * st->shadow), 0,
                       255);
            }
        }
    }

    // 2. Blur and squeeze the backdrop, then lift it toward white with a
    //    vertical gradient (a slab lit from above is brighter at the top).
    gc_blur(c, pill->x, pill->y, w, h, st->blur_radius, st->blur_passes, 0, 0,
            st->squeeze);
    for (int j = 0; j < h; ++j) {
        const float t = static_cast<float>(j) / static_cast<float>(h > 1 ? h - 1 : 1);
        const int alpha = clampi(st->lift + static_cast<int>(st->lift_top * (1.0f - t)),
                                 0, 255);
        gc_blend_mask(c, pill->x, pill->y + j, w, 1, mask_buf + j * w, 255, alpha,
                      st->weight);
    }

    // 3. Diagonal sheen across the upper-left field.
    if (st->sheen > 0) {
        const float span = static_cast<float>(w) * 0.55f + static_cast<float>(h);
        for (int j = 0; j < h; ++j) {
            for (int i = 0; i < w; ++i) {
                const int cov = mask_buf[j * w + i];
                if (cov == 0) continue;
                const float d = (static_cast<float>(i) * 0.55f + static_cast<float>(j)) / span;
                const float off = d - 0.30f;
                const float band = expf(-(off * off) / 0.012f);
                const int delta = static_cast<int>(band * st->sheen);
                if (delta <= 0) continue;
                gc_add_mask(c, pill->x + i, pill->y + j, 1, 1, mask_buf + j * w, delta,
                            -1);
            }
        }
    }

    // 4. Rim lighting from the mask's own derivatives: the vertical derivative
    //    is positive at the top edge and negative at the bottom, and the
    //    horizontal derivative marks the sides.
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            const int here = mask_buf[j * w + i];
            if (here == 0) continue;
            const int up = (j > 0) ? mask_buf[(j - 1) * w + i] : 0;
            const int down = (j + 1 < h) ? mask_buf[(j + 1) * w + i] : 0;
            const int left = (i > 0) ? mask_buf[j * w + i - 1] : 0;
            const int right = (i + 1 < w) ? mask_buf[j * w + i + 1] : 0;
            const int v_deriv = here - up;      // >0 at the top edge
            const int b_deriv = here - down;    // >0 at the bottom edge
            const int h_deriv = iabs(here - left) + iabs(here - right);
            int delta = 0;
            if (v_deriv > 0) delta += (v_deriv * st->rim) / 255;
            if (b_deriv > 0) delta -= (b_deriv * st->rim) / 510;
            if (h_deriv > 0) delta += (h_deriv * st->rim) / 900;
            if (delta == 0) continue;
            gc_add_mask(c, pill->x + i, pill->y + j, 1, 1, mask_buf + j * w, delta, -1);
            // Rims stay crisp: no dithering on an edge that is already 1 px.
            const int index = (pill->y + j) * GLASS_W + pill->x + i;
            if (index >= 0 && index < GLASS_PIXELS) c->weight[index] = 0;
        }
    }
}

extern "C" int glass_pill_mean(const glass_canvas_t* c, const glass_pill_t* pill) {
    long sum = 0;
    int n = 0;
    for (int j = 0; j < pill->h; j += 3) {
        for (int i = 0; i < pill->w; i += 3) {
            sum += gc_get(c, pill->x + i, pill->y + j);
            ++n;
        }
    }
    return n ? static_cast<int>(sum / n) : 128;
}

extern "C" uint8_t glass_ink_for(int panel_mean, uint8_t dark, uint8_t light) {
    // Dead band around mid gray keeps a panel from flipping ink as the sun
    // moves a few percent.
    if (panel_mean > 150) return dark;
    if (panel_mean < 105) return light;
    return panel_mean > 128 ? dark : light;
}