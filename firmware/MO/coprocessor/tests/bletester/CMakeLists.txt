cmake_minimum_required(VERSION 3.20.0)
set(BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. CACHE PATH "")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bletester)

target_sources(app PRIVATE
    src/main.c
)
