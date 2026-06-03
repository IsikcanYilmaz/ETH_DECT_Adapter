#ifndef dectSerial
#define dectSerial
/**
 * Dect_serial is the bridge between serial and dect. It defines
 * pdc function for dect to uart,
 * it creates functions to easily send packages from serial buffer to dect devices.
 * This is encapsulated by a custom tx and an rx function.
 */
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/drivers/hwinfo.h>
#include <string.h>

#include "serial.h"
#include "dect_phy.h"

#define FRAME_TX_MCS 3
#define FRAME_TX_SS 2

/**
 * @brief fragments serial traffic and sends via dect.
 * TODO implement adaptive subslot lenght, using shorter lenghts for smaller frames.
 * @return number of scheduled transmissions
 *
 * @param duration duration in modem units
 */
int serial_to_dect(uint64_t start_time, uint64_t duration);


/**
 * @brief callback for dect to serail functionality. Unpacks packages and forwards them to serial. This is registed as a callback function for dect events. 
 *
 * @param evt standart pdc callback event
 */
void serial_on_pdc(const struct nrf_modem_dect_phy_pdc_event *evt);

/*
 * @brief basic version of serial_on_pdc
 */
void sink_serial_on_pdc(const struct nrf_modem_dect_phy_pdc_event *evt);

/**
 * @brief Registers on_pdc function as callback
 */
void init_dect_serial();

#endif
