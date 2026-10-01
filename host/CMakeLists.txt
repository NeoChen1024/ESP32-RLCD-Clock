cmake_minimum_required(VERSION 3.16)
project(rlcd_host C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- SDL3 (host display backend) ----
find_package(SDL3 CONFIG REQUIRED)

# ---- u8g2 core (csrc is self-contained C; no SDL/hardware deps there) ----
file(GLOB U8G2_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../u8g2/csrc/*.c")
list(FILTER U8G2_SRCS EXCLUDE REGEX "/u8g2_fonts\\.c$")
add_library(u8g2_core STATIC ${U8G2_SRCS} src/u8g2_selected_fonts.c)
target_include_directories(u8g2_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../u8g2/csrc")

# ---- host simulator ----
add_executable(rlcd_host
  src/main.c
  src/host_time.c
  src/sdl3_backend.c
  ../common/time_model.c
  ../common/render_faces.c
  ../common/frame_export.c
)
target_include_directories(rlcd_host PRIVATE src ../common)
target_link_libraries(rlcd_host PRIVATE u8g2_core SDL3::SDL3)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_options(rlcd_host PRIVATE -O2)
endif()

# ---- frame-export unit test (no SDL needed) ----
add_executable(test_frame_export tests/test_frame_export.c ../common/frame_export.c)
target_include_directories(test_frame_export PRIVATE ../common)
enable_testing()
add_test(NAME frame_export COMMAND test_frame_export)

# ---- time-model unit test (no SDL needed) ----
add_executable(test_time_model tests/test_time_model.c ../common/time_model.c)
target_include_directories(test_time_model PRIVATE ../common ../u8g2/csrc)
enable_testing()
add_test(NAME time_model COMMAND test_time_model)

# Firmware policy regressions run against a deterministic fake IDF transport.
add_executable(test_clock_health tests/test_clock_health.c ../common/clock_health.c)
target_include_directories(test_clock_health PRIVATE ../common)
target_compile_options(test_clock_health PRIVATE -UNDEBUG)
add_test(NAME clock_health COMMAND test_clock_health)

add_executable(test_sensor_health tests/test_sensor_health.c ../common/sensor_health.c)
target_include_directories(test_sensor_health PRIVATE ../common)
target_compile_options(test_sensor_health PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_test(NAME sensor_health COMMAND test_sensor_health)

add_executable(test_rtc_clock tests/test_rtc_clock.c ../common/rtc_clock.c)
target_include_directories(test_rtc_clock PRIVATE ../common)
target_compile_options(test_rtc_clock PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_test(NAME rtc_clock COMMAND test_rtc_clock)

add_executable(test_network_managers tests/test_network_managers.c
  tests/fakes/fake_idf.c ../firmware/main/sntp_mgr.c ../firmware/main/wifi_mgr.c
  ../common/clock_health.c)
target_include_directories(test_network_managers PRIVATE tests/fakes ../firmware/main ../common)
target_compile_options(test_network_managers PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_test(NAME network_managers COMMAND test_network_managers)

add_executable(test_render_trust tests/test_render_trust.c src/sdl3_backend.c
  ../common/render_faces.c ../common/time_model.c ../common/frame_export.c)
target_include_directories(test_render_trust PRIVATE src ../common)
target_compile_options(test_render_trust PRIVATE -UNDEBUG)
target_link_libraries(test_render_trust PRIVATE u8g2_core SDL3::SDL3)
add_test(NAME render_trust COMMAND test_render_trust)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)
add_executable(test_tz_leap tests/test_tz_leap.c ../common/tz_rule.c
  ../common/leap_table.c ../common/sha1.c)
target_include_directories(test_tz_leap PRIVATE ../common)
target_compile_options(test_tz_leap PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_test(NAME tz_leap COMMAND test_tz_leap)

add_executable(test_audio_source tests/test_audio_source.c ../common/audio_source.c ../common/wav_format.c)
target_include_directories(test_audio_source PRIVATE ../common ../contrib/dr_libs)
target_compile_options(test_audio_source PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_compile_definitions(test_audio_source PRIVATE DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
add_test(NAME audio_source COMMAND test_audio_source)

add_executable(test_wav_format tests/test_wav_format.c ../common/wav_format.c)
target_include_directories(test_wav_format PRIVATE ../common)
target_compile_options(test_wav_format PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_test(NAME wav_format COMMAND test_wav_format)

add_executable(test_storage_files tests/test_storage_files.c ../common/storage_files.c
  ../common/leap_table.c ../common/sha1.c ../common/wav_format.c ../common/audio_source.c)
target_include_directories(test_storage_files PRIVATE ../contrib/dr_libs ../common)
target_link_libraries(test_storage_files PRIVATE PkgConfig::CJSON)
target_compile_options(test_storage_files PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_test(NAME storage_files COMMAND test_storage_files)

add_executable(test_config_mgr tests/test_config_mgr.c ../firmware/main/config_mgr.c
  ../common/storage_files.c ../common/tz_rule.c ../common/leap_table.c ../common/sha1.c
  ../common/wav_format.c ../common/audio_source.c)
target_include_directories(test_config_mgr PRIVATE ../contrib/dr_libs tests/fakes ../firmware/main ../common)
target_link_libraries(test_config_mgr PRIVATE PkgConfig::CJSON)
target_compile_options(test_config_mgr PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_test(NAME config_mgr COMMAND test_config_mgr)
target_compile_definitions(test_config_mgr PRIVATE REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(test_wifi_secrets tests/test_wifi_secrets.c ../firmware/main/wifi_secrets.c
  ../common/storage_files.c ../common/leap_table.c ../common/sha1.c ../common/wav_format.c ../common/audio_source.c)
target_include_directories(test_wifi_secrets PRIVATE ../contrib/dr_libs tests/fakes ../firmware/main ../common)
target_link_libraries(test_wifi_secrets PRIVATE PkgConfig::CJSON)
target_compile_options(test_wifi_secrets PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_compile_definitions(test_wifi_secrets PRIVATE REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME wifi_secrets COMMAND test_wifi_secrets)
