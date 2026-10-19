/**
 * The weather card: sky, then glass panels, then type.
 *
 * Layout follows the Zectrix "Liquid Glass Weather" reference (MIT) closely
 * enough to be recognisable - city and date across the top, one large panel
 * for the observation, a slim metrics pill, three forecast pills - but the
 * panels are computed here, so the geometry is ours and the ink colour is
 * chosen per panel from what the sky actually looks like behind it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "glass_canvas.h"
#include "glass_glass.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GLASS_CARD_DAYS 3

typedef struct {
    char label[10];
    char icon[20];
    char high[8];
    char low[8];
    char precip[12];
    bool is_day;
} glass_card_day_t;

typedef struct {
    char city[28];
    char date[20];
    char updated[28];
    char temp[10];
    char unit[8];
    char condition[28];
    char feels[20];
    char high[10];
    char low[10];
    char wind[20];
    char humidity[12];
    char uv[10];
    char icon[20];
    bool is_day;
    glass_card_day_t days[GLASS_CARD_DAYS];
    /** Set when the observation is stale or unavailable; shown as a note. */
    char status[28];
} glass_card_data_t;

/**
 * Render a complete frame. `workspace` needs at least 380*110 bytes for the
 * largest panel mask; the caller owns it (PSRAM on the device).
 */
void glass_card_render(glass_canvas_t* canvas, const glass_card_data_t* data,
                       const glass_style_t* style, uint8_t* workspace);

/** Panel rectangles, exposed so the simulator and tests can reason about them. */
void glass_card_panels(glass_pill_t* out_big, glass_pill_t* out_metrics,
                       glass_pill_t* out_days);

#ifdef __cplusplus
}
#endif