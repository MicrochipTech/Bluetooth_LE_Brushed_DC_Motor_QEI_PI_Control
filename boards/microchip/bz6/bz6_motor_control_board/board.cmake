# Copyright (c) 2025 Microchip Technology Inc.
# SPDX-License-Identifier: Apache-2.0

board_runner_args(openocd
  "--hex-file=${CMAKE_BINARY_DIR}/zephyr/zephyr_signed.hex"
)

include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
