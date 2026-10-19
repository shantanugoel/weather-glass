idf_component_register(
    SRCS "app_main.cc" "glass_wifi.cc" "glass_net.cc"
    INCLUDE_DIRS "."
    REQUIRES glass_app glass_gfx zectrix_epd zectrix_board
             nvs_flash esp_wifi esp_netif esp_event esp_http_client esp-tls
             esp_timer esp_driver_gpio esp_driver_i2c heap
)
