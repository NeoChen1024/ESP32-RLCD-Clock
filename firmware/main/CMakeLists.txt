idf_component_register(
    SRCS "app_main.c"
         "cli.c"
         "wifi_mgr.c"
         "sntp_mgr.c"
         "http_srv.c"
         "snapshot.c"
         "display.c"
         "render.c"
         "model.c"
         "sensors.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif nvs_flash esp_http_server lwip console esp_timer u8g2 esp_driver_usb_serial_jtag u8g2_st7305 esp_driver_i2c esp_adc
)

# Shared pure-C sources: compile the host copies directly so host and target
# can never drift (design notes §9 / frame_export pattern).
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../host/src/time_model.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../host/src/render_faces.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../host/src/frame_export.c"
)
target_include_directories(${COMPONENT_LIB} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../host/src")
