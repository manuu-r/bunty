file(MAKE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../.pio/generated")
file(GLOB BUNTY_SOURCES "*.cpp" "flow32/graphics/*.cpp")
idf_component_register(SRCS ${BUNTY_SOURCES} INCLUDE_DIRS "." "../.pio/generated"
    REQUIRES animatedgif muse_core bunty_platform gfx arduinojson bt esp_driver_i2s esp_lcd esp_driver_spi
             esp_driver_i2c esp_driver_ledc esp_driver_gpio esp_wifi esp_netif
             esp_http_client esp-tls nvs_flash esp_psram)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter -fstack-usage -Wframe-larger-than=2048)
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=malloc" "-Wl,--wrap=calloc")
