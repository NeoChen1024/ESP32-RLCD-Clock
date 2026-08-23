cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Minimal bring-up firmware: Wi-Fi STA + SNTP + HTTP debug server + serial CLI.
project(rlcd_fw)
