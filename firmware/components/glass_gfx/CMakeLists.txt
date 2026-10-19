idf_component_register(
    SRCS "glass_canvas.cc" "glass_font.cc" "glass_astro.cc" "glass_sky.cc"
         "glass_glass.cc" "glass_plate.cc" "glass_scene.cc" "glass_card.cc"
         "generated/glass_assets.cc" "generated/glass_plates.cc"
    # "." so the sources can say #include "generated/glass_assets.h".
    INCLUDE_DIRS "." "include" "generated"
)
