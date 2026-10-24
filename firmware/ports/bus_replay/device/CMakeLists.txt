# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bus-replay-device)

target_sources(app PRIVATE
    src/main.c
    src/control.c
    src/capture.c
    src/capture_loop.S
)

if(CAPTURE_PROFILE)
    target_compile_definitions(app PRIVATE CAPTURE_PROFILE)
endif()
