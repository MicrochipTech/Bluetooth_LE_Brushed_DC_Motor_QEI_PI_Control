#
# Copyright (c) 2025 Microchip Technology Inc.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ble_dc_motor_mppb)

FILE(GLOB app_sources CONFIGURE_DEPENDS src/*.c)
target_sources(app PRIVATE
  ${app_sources}
  )

zephyr_library_include_directories(.)