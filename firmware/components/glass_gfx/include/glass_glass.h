/**
 * Frosted "liquid glass" panels.
 *
 * Five things sell frosted glass in 16 gray levels, and all five are computed
 * from whatever is behind the panel rather than baked into an image:
 *
 *   1. the backdrop under the panel is blurred and squeezed slightly, like a
 *      thick lens, so shapes bend as they pass through;
 *   2. the blurred backdrop is lifted toward white, more at the top than the
 *      bottom, which is what a slab lit from above looks like;
 *   3. a soft drop shadow sits below the panel;
 *   4. the rim catches light: bright where the surface turns away at the top,
 *      a darker line where it turns under at the bottom;
 *   5. a faint diagonal sheen crosses the upper-left field.
 *
 * Because it is computed, the same code works over a black starfield or a
 * white overcast sheet, and it adapts to any pill geometry.
 */
#pragma once

#include <stdint.h>

#include "glass_canvas.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x;
    int y;
    int w;
    int h;
    /** Corner radius; min(w,h)/2 makes a pill. */
    int radius;
} glass_pill_t;

typedef struct {
    /** How far the panel lifts toward white, 0..255. */
    int lift;
    /** Extra lift at the top edge. */
    int lift_top;
    int blur_radius;
    int blur_passes;
    /** Backdrop scale about the panel centre, 256 = none (248 reads as a lens). */
    int squeeze;
    /** Drop shadow strength. */
    int shadow;
    /** Rim specular strength. */
    int rim;
    /** Diagonal sheen strength. */
    int sheen;
    /** Dither weight kept inside the panel (rims are always crisp). */
    int weight;
} glass_style_t;

void glass_style_default(glass_style_t* style);

/**
 * Draw one panel. `mask_buf` must hold at least pill->w * pill->h bytes and
 * may be reused for every panel in a frame.
 */
void glass_pill_draw(glass_canvas_t* c, const glass_pill_t* pill,
                     const glass_style_t* style, uint8_t* mask_buf);

/** Mean luminance inside a panel; used to pick ink that contrasts with it. */
int glass_pill_mean(const glass_canvas_t* c, const glass_pill_t* pill);

/**
 * Ink colour for a panel: dark on a light panel, light on a dark one, with a
 * dead band so a mid-gray panel does not flip between the two.
 */
uint8_t glass_ink_for(int panel_mean, uint8_t dark, uint8_t light);

#ifdef __cplusplus
}
#endif