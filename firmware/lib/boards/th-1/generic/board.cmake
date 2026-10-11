# SPDX-License-Identifier: Apache-2.0

# SWD pins are not defined by this board. Pass them at flash time:
#   west flash --cmd-pre-init "th1_swd <swclk> <swdio> [srst]"
board_runner_args(openocd
	"--config=${BOARD_DIR}/support/interface-gpio.cfg"
	"--config=${BOARD_DIR}/support/target-nrf52840.cfg"
)

include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
