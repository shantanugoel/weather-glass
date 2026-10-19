/**
 * One compositor entry point for the whole frame.
 *
 * The device and the simulator both call this, so a render that looks right in
 * sim/out is the same composition the panel gets - the reason this exists as a
 * single function instead of each caller sequencing the steps itself.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "glass_canvas.h"
#include "glass_card.h"
#include "glass_plates.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Compose a complete 400x300 frame.
 *
 * `scene` is the weather scene name from weather_wmo(); `data` may be null to
 * render the plate alone. `workspace` is the caller-owned scratch the card
 * renderer needs for its largest mask.
 */
void glass_render_frame(glass_canvas_t* canvas, const char* scene, bool is_day,
                        const glass_card_data_t* data, const glass_style_t* style,
                        uint8_t* workspace);

#ifdef __cplusplus
}
#endif
