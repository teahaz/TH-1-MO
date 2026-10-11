# SPDX-License-Identifier: Apache-2.0

board_runner_args(openocd
	"--config=${BOARD_DIR}/support/interface-carrier-board.cfg"
	"--config=${BOARD_DIR}/support/target-nrf52840.cfg"
)

include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
