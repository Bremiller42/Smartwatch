# PlatformIO / ESP-IDF shim: treat src/ as the app component

cmake_minimum_required(VERSION 3.16)

file(GLOB_RECURSE app_sources
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/src"
)
