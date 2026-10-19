#include "glass_scene.h"

#include <string.h>

extern "C" void glass_render_frame(glass_canvas_t* canvas, const char* scene,
                                   bool is_day, const glass_card_data_t* data,
                                   const glass_style_t* style, uint8_t* workspace) {
    // The plate *is* the artwork: sky, slabs, rims, refraction and shadows are
    // already in the photograph. Everything after this is data, and nothing
    // here paints a panel, a border or a highlight - upstream's own README is
    // explicit that procedural glass over these plates makes them worse.
    glass_plate_fill(canvas->px, scene, is_day);

    // The plate is authored at exactly 16 levels, so every pixel is already a
    // value the panel can hit. Dithering it would only add noise where there is
    // no error to diffuse; weight 0 says "show this tone as it is". Text sets
    // its own weight as it draws, over the top.
    memset(canvas->weight, 0, GLASS_PIXELS);

    if (data != nullptr) {
        glass_card_render(canvas, data, style, workspace);
    }
}
