# u8g2 core compiled as an ESP-IDF component.
#
# Sources come from the repo's u8g2 submodule (csrc is pure C, no Arduino
# dependency). Excluded: the all-fonts blob (we link selected fonts instead)
# and the device setup/display files (ST7305 init is the backend's job).

set(U8G2_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../u8g2/csrc")

file(GLOB U8G2_SRCS "${U8G2_ROOT}/u8g2_*.c" "${U8G2_ROOT}/u8x8_*.c")
list(FILTER U8G2_SRCS EXCLUDE REGEX "/u8g2_fonts\\.c$")
list(FILTER U8G2_SRCS EXCLUDE REGEX "/u8g2_d_setup\\.c$")
list(FILTER U8G2_SRCS EXCLUDE REGEX "/u8x8_d_.*\\.c$")

idf_component_register(
    SRCS ${U8G2_SRCS} "u8g2_selected_fonts.c"
    INCLUDE_DIRS "."
)

target_include_directories(${COMPONENT_LIB} PUBLIC "${U8G2_ROOT}")
target_compile_definitions(${COMPONENT_LIB} PUBLIC U8X8_WITH_USER_PTR)
