# ETH_DECT_Adapter

# Pico side:
- As an env variable have PICO_SDK_PATH point to pico-sdk
- building:
- in pico_usb_ecm/ do:
- `mkdir build; cd build; cmake ..; make` this will compile the code and output a uf2 file. 
- plug in the Pico W *while pressing and holding the bootsel button*. copy the uf2 file into the volume the pico shows itself as
- On the pico: GPIO17 = UART RX, GPIO16 = UART TX. So GP17 will connect to P0.28 of the nRF9151DK, GP16 will connect to P0.29
- On the pico: *one of the boards must have its GPIO 28 be connected to 3V3. This is to let one of the boards have different interface name when it introduces itself. ie. the board without the GP28 connection will be seen as enxcafe00000001 and the one with the connection will be seen with the name enxcafe00000002, in the network interfaces list*
- code for sure could use improvements; namely - a cleanup, - DMA based TX, - SPI instead of UART, - CRC on the packets

# nRF9151DK side:
- Tested on sdk version v3.2.0-rc2, though should work on other versions.
- run the `nrfutil sdk-manager toolchain launch --ncs-version "v3.2.0-rc2" --shell` command to drop into nrf's build environment
- in DECT/ do:
- `west build -p -b nrf9151dk/nrf9151/ns -- -DEXTRA_CONF_FILE=overlay-eu.conf -DDTC_OVERLAY_FILE=dongle.overlay`
- the `dongle.overlay` file defines the UART settings and enables it. the overlay-eu.conf may or may not be needed
- flash using `west flash`
- once the board boots up, press button1. Without this it wont start.
- as mentioned above; P0.28 is the UART TX of the DECT and P0.29 is the UART RX. so connect DECT P0.28 to Pico GP16, and DECT P0.29 to Pico GP17. Baud rate is 1000000. 
- Code could use improvements; namely - a cleanup, - DMA based everything, - SPI instead of UART, - MAC logic


