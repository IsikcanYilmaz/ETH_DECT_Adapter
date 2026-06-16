#ifndef TDMA
#define TDMA
/**
 * This defines the access table,
 * Provides callback functions
 * Implements sceduling according to access table.
 * - nrf_modem_dect_phy_time_get()
 *
 * Implementation steps:
 *  1. Create stupid sceduler
 *  2. Add start delays
 */
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/drivers/hwinfo.h>
#include <string.h>

#include "dect_phy.h"

typedef enum
{
	UNDEFINED_ROLE,
	MASTER,
	SLAVE,
	SINK, // TODO this is a temporary role just to test out Tx.
  TEST, // 
	ROLE_MAX,
} Role_e;

extern uint8_t mode;

extern uint8_t sync_flag;
#define TDMA_SYNC_MCS 1
#define TDMA_SYNC_SS 2
#define TDMA_MAX_TRANSMISSIONS 10

/**
 * @brief callback for pcc and sync flag *
 */
extern void tdma_on_pcc(const struct nrf_modem_dect_phy_pcc_event *evt);
#define TDMA_ERROR 1

/**
 * @brief intiation function of TDMA. Creates interrupt function for specific tdma mode(Slave or Master).
*/
void init_tdma();


/**
 * @brief schedules the corresponding sync, dect_to_serial, serial_to_dect, and buffer functions
 *  t + startup + sched_startup   start_time_op1 + duration_op1 + sched_transition
*/
void schedule_master();

/**
 * @brief For testing purposes
 */
void schedule_basic_master();

/**
 * @brief schedules the corresponding sync_receive, dect_to_serial, serial_to_dect, and buffer functions
 *  t + startup + sched_startup   start_time_op1 + duration_op1 + sched_transition
*/
void schedule_slave();


/**
 * @brief schedule sync ping
*/
int schedule_sync(uint64_t start_time);

#endif
