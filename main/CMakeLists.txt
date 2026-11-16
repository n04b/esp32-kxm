idf_component_register(
    SRCS
        "main.c"
        "app_config.c"
        "kvm_input.c"
        "kvm_rx.c"
        "net_wifi.c"
        "status.c"
        "ts_client.c"
        "usb_hid.c"
        "web_ui.c"
    INCLUDE_DIRS "."
    REQUIRES
        microlink
    PRIV_REQUIRES
        nvs_flash esp_wifi esp_netif esp_event esp_timer esp_http_server
        app_update esp_app_format lwip mbedtls json driver esp_driver_gpio
)

# Web page: compressed at build time and embedded in the firmware.
idf_build_get_property(python PYTHON)
set(WWW_SRC ${CMAKE_CURRENT_SOURCE_DIR}/www/index.html)
set(WWW_GZ  ${CMAKE_CURRENT_BINARY_DIR}/index.html.gz)
add_custom_command(
    OUTPUT ${WWW_GZ}
    COMMAND ${python} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gzip_www.py ${WWW_SRC} ${WWW_GZ}
    DEPENDS ${WWW_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gzip_www.py
    VERBATIM)
add_custom_target(www_gz DEPENDS ${WWW_GZ})
add_dependencies(${COMPONENT_LIB} www_gz)
target_add_binary_data(${COMPONENT_LIB} ${WWW_GZ} BINARY)
