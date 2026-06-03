cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w)
include(pico_sdk_import.cmake)

project(pico_ecm C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pico_ecm
    main.c
    usb_descriptors.c
    debug_log.c
    eth_uart.c
    utils.c
    ${PICO_SDK_PATH}/lib/tinyusb/lib/networking/dhserver.c
    ${PICO_SDK_PATH}/lib/tinyusb/lib/networking/dnserver.c
    ${PICO_SDK_PATH}/lib/tinyusb/lib/networking/rndis_reports.c
)

set(DEBUG_LOG 1 CACHE STRING "Enable USB CDC debug logging 1/0")
set(LWIP_DEBUG 1)
target_compile_definitions(pico_ecm PRIVATE DEBUG_LOG_ENABLED=${DEBUG_LOG})

target_include_directories(pico_ecm PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${PICO_SDK_PATH}/lib/tinyusb/lib/networking
)

target_link_libraries(pico_ecm PRIVATE
    pico_cyw43_arch_none
    pico_stdlib
    tinyusb_device
    tinyusb_board
    hardware_dma
    # tinyusb_additional_class_net    # older SDKs: use pico_lwip + pico_lwip_iperf
    pico_lwip
    pico_lwip_nosys
)

pico_add_extra_outputs(pico_ecm)
# pico_enable_stdio_usb(pico_ecm 1)
# pico_enable_stdio_uart(pico_ecm 0)
