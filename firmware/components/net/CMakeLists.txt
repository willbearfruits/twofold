# TWOFOLD net: WiFi (AP setup portal / STA), captive DNS, httpd + embedded web app, WebSocket, OTA.
idf_build_get_property(python PYTHON)

idf_component_register(
    SRCS "net_wifi.c" "net_http.c" "net_api.c" "net_ws.c" "net_ota.c" "net_dns.c" "net_json.c" "net_usb.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES ui                     # net.h uses wifi_state_t / sys_status_t from ui_sys.h
    PRIV_REQUIRES bootdiag audio dsp espressif__tinyusb usbdev esp_wifi esp_netif esp_event esp_http_server esp_timer esp_app_format app_update
                  esp_partition lwip json params dsp storage)

# The web app is a single hand-written file (web/index.html). It is gzipped at build time
# (deterministic: mtime 0) and embedded; httpd serves it with Content-Encoding: gzip.
set(WEB_SRC "${CMAKE_CURRENT_LIST_DIR}/web/index.html")
set(WEB_GZ  "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(
    OUTPUT "${WEB_GZ}"
    COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/web/gzip_web.py" "${WEB_SRC}" "${WEB_GZ}"
    DEPENDS "${WEB_SRC}" "${CMAKE_CURRENT_LIST_DIR}/web/gzip_web.py"
    COMMENT "Compressing web app"
    VERBATIM)
add_custom_target(net_web_gz DEPENDS "${WEB_GZ}")
add_dependencies(${COMPONENT_LIB} net_web_gz)
target_add_binary_data(${COMPONENT_LIB} "${WEB_GZ}" BINARY DEPENDS net_web_gz)
set_property(DIRECTORY "${COMPONENT_DIR}" APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${WEB_GZ}")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
