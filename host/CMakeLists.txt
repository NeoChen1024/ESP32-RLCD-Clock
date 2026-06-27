cmake_minimum_required(VERSION 3.16)
project(rlcd_host C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- SDL3 (host display backend) ----
find_package(SDL3 CONFIG REQUIRED)

# ---- u8g2 core (csrc is self-contained C; no SDL/hardware deps there) ----
file(GLOB U8G2_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/../u8g2/csrc/*.c")
add_library(u8g2_core STATIC ${U8G2_SRCS})
target_include_directories(u8g2_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../u8g2/csrc")

# ---- host simulator ----
add_executable(rlcd_host
  src/main.c
  src/time_model.c
  src/render_faces.c
  src/sdl3_backend.c
)
target_include_directories(rlcd_host PRIVATE src)
target_link_libraries(rlcd_host PRIVATE u8g2_core SDL3::SDL3)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_options(rlcd_host PRIVATE -O2)
endif()

# ---- time-model unit test (no SDL needed) ----
add_executable(test_time_model tests/test_time_model.c src/time_model.c)
target_include_directories(test_time_model PRIVATE src ../u8g2/csrc)
enable_testing()
add_test(NAME time_model COMMAND test_time_model)
