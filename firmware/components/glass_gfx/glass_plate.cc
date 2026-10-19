#include "glass_plates.h"

#include <string.h>

#include "glass_canvas.h"

namespace {

struct ScenePlate {
    const char* scene;
    const char* day;
    const char* night;
};

/** Which photograph answers each weather scene. */
constexpr ScenePlate kScenes[] = {
    {"clear", "clear", "night"},
    {"cloud", "cloud", "cloud-night"},
    {"rain", "rain", "rain-night"},
    {"snow", "snow", "snow-night"},
    {"thunder", "thunder", "thunder"},
    {"fog", "fog", "fog-night"},
};

const glass_plate_t* find_plate(const char* name) {
    for (size_t i = 0; i < glass_plate_count; ++i) {
        if (strcmp(glass_plates[i].name, name) == 0) return &glass_plates[i];
    }
    return nullptr;
}

/** Nibble n to grey n * 17, so 15 becomes exactly 255 rather than 250. */
void expand(const uint8_t* packed, uint32_t bytes, uint8_t* out) {
    for (uint32_t i = 0; i < bytes; ++i) {
        const uint8_t b = packed[i];
        out[i * 2] = static_cast<uint8_t>((b & 0x0F) * 17);
        out[i * 2 + 1] = static_cast<uint8_t>((b >> 4) * 17);
    }
}

}  // namespace

extern "C" void glass_plate_fill(uint8_t* out, const char* scene, bool is_day) {
    const glass_plate_t* plate = nullptr;
    if (scene != nullptr) {
        for (const auto& s : kScenes) {
            if (strcmp(s.scene, scene) == 0) {
                plate = find_plate(is_day ? s.day : s.night);
                break;
            }
        }
    }
    if (plate == nullptr) {
        // Unknown scene, or a night variant that was never baked: an overcast
        // day is the least wrong thing to show, and better than a white panel.
        plate = find_plate("cloud");
    }
    if (plate == nullptr) {
        memset(out, 255, GLASS_PIXELS);
        return;
    }
    expand(plate->pixels, plate->length, out);
}
