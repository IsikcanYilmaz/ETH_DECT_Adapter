#
# Copyright (c) 2024 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hello_dect)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

target_sources(app PRIVATE
    src/button.c
    src/dect_phy_events.c
    src/dect_phy.c
    src/serial.c
    src/dect_serial.c
    src/main.c
    src/tdma.c
    src/dect_uart.c
    src/led.c
    src/tester.c
)
