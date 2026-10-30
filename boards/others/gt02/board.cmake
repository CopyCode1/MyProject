# SPDX-License-Identifier: Apache-2.0

# 为J-Link runner指定目标芯片和SWD通信速度。
# 这里只配置烧录工具，不会在CMake配置或编译阶段连接真实硬件。
board_runner_args(jlink "--device=nRF52840_xxAA" "--speed=4000")

# 注册J-Link烧录、调试和RTT能力；因为首先注册，所以成为默认runner。
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
# 同时保留OpenOCD runner，便于后续显式使用--runner openocd。
include(${ZEPHYR_BASE}/boards/common/openocd-nrf5.board.cmake)
