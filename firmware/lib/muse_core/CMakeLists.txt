set(SDK "${CMAKE_CURRENT_LIST_DIR}/../../.deps/muse/esp32")
set(names app ble_server wifi_mgr wifi_known identity config_store vm_api
    diagnostic_log stack_monitor bug_report ota image_fetch pixel_font tunnel_netif
    net_discovery link_pairing pairing_signer_policy pairing_transcript factory_test)
set(sources)
foreach(name ${names})
    list(APPEND sources "${SDK}/main/${name}.c")
endforeach()
list(APPEND sources "${SDK}/main/noise_control.cpp" "${SDK}/main/noise_tunnel.cpp"
    "${SDK}/avatar/happy_anim.c" "${SDK}/components/muse/muse_chat_link.c"
    "${SDK}/components/muse/muse_chat_text.c" "${SDK}/components/muse/muse_text.c")
idf_component_register(SRCS ${sources} INCLUDE_DIRS "." "${SDK}/main" "${SDK}/components/muse"
    PRIV_INCLUDE_DIRS "${SDK}/avatar"
    REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_http_client esp_https_ota
      app_update esp_app_format esp-tls noise_core product_info bt driver esp_driver_gpio
      esp_driver_i2c esp_driver_ledc esp_driver_spi esp_driver_uart esp_lcd lwip mbedtls
      efuse esp_timer esp_psram espressif__cjson)
target_compile_options(${COMPONENT_LIB} PRIVATE "-include${CMAKE_CURRENT_LIST_DIR}/../../.deps/muse-token.h")
