idf_component_register(
    SRCS "app_main.c"
         "cli.c"
         "wifi_mgr.c"
         "sntp_mgr.c"
         "http_srv.c"
         "http_files.c"
         "snapshot.c"
         "display.c"
         "render.c"
         "model.c"
         "sensors.c"
         "rtc_mgr.c"
         "storage_mgr.c"
         "config_mgr.c"
         "leap_mgr.c"
         "wifi_secrets.c"
         "audio_mgr.c"
         "http_audio.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "files.html" "home.html" "web_style.css"
    REQUIRES esp_wifi esp_netif nvs_flash esp_http_server lwip console esp_timer u8g2 esp_driver_usb_serial_jtag u8g2_st7305 esp_driver_i2c esp_adc fatfs sdmmc esp_driver_sdmmc wear_levelling esp_driver_i2s
)

# Shared pure-C sources: compile the common/ copies directly so host and
# target can never drift (see docs/architecture.md).
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../common/storage_files.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../common/clock_health.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../common/sensor_health.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../common/rtc_clock.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../common/time_model.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../common/tz_rule.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../common/leap_table.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../common/sha1.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../common/wav_format.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../common/render_faces.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../common/frame_export.c"
)
target_include_directories(${COMPONENT_LIB} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../common")

# Capture option 42 independently of the active SNTP table (manual overrides
# and fallback must not destroy the DHCP selection used by reset/retry).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=dhcp_set_ntp_servers")
