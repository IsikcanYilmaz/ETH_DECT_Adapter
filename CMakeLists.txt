#
# Copyright (c) 2024 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mac_demo)

target_include_directories(app PRIVATE 
  mac_impl 
  phy_impl
  lean_wiznet_driver
)

target_sources(app PRIVATE
  main.c
  lean_wiznet_driver/lean_wiznet_driver.c
  mac_impl/dect_adapter.c
  mac_impl/mac_main.c
  phy_impl/phy_main.c
)
