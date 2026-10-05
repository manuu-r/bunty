idf_component_register(SRCS "Runtime.cpp" "Preferences.cpp" "WiFi.cpp"
    INCLUDE_DIRS "." REQUIRES esp_driver_gpio esp_driver_ledc esp_timer
    esp_psram esp_wifi esp_netif nvs_flash esp_event esp_pm lwip)
