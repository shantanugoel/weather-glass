idf_component_register(
    SRCS "weather_model.cc" "mini_json.cc"
    INCLUDE_DIRS "." "include" "generated"
    REQUIRES glass_gfx
)
