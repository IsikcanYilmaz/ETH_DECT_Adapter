/*
 * dect_phy.h — DECT NR+ PHY radio abstraction layer
 *
 * Owns the modem lifecycle, TX/RX primitives, and the generalised task runner.
 * All modem interaction goes through this module — no other file calls
 * nrf_modem_dect_phy_tx() or nrf_modem_dect_phy_rx() directly.
 *
 * Usage:
 *   1. Call dect_phy_init() after nrf_modem_lib_init().
 *   2. Register application PDC/PCC handlers via dect_phy_events.h.
 *   3. Call dect_run_task() to execute a TX/RX task.
 *   4. Call dect_phy_deinit() before nrf_modem_lib_shutdown().
 */

#ifndef DECT_PHY_H
#define DECT_PHY_H
#define US_TO_MODEM_TICKS(us) (((uint64_t)(us)/1000 )* NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ )

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <nrf_modem_dect_phy.h>


#define TX_HANDLE         0
#define RX_HANDLE         1
#define PAYLOAD_LEN_MAX 700

/* ------------------------------------------------------------------ */
/*  Sweep parameters                                                   */
/* ------------------------------------------------------------------ */

#define MCS_COUNT     5
#define SUBSLOT_COUNT 16

/** @brief Sentinel value returned by get_bit_count() for invalid combos. */
#define INVALID_BYTES -1

/* ------------------------------------------------------------------ */
/*  Owned variables (extern for read access by dect_phy_events.c)     */
/* ------------------------------------------------------------------ */
extern const int mcs_subslot_bits[MCS_COUNT][SUBSLOT_COUNT];
//extern struct nrf_modem_dect_phy_latency_info latency;
/* ------------------------------------------------------------------ */
/*  MCS / subslot payload size table                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief Payload sizes in bits for each MCS and subslot combination.
 *
 * Rows  = MCS index (0–4)
 * Cols  = subslot count (0–15)
 *
 * INVALID_BYTES marks combinations that are not supported by the standard.
 *
 * @note Values are in bits. Divide by 8 to get bytes.
 *       TODO: rename to mcs_subslot_bits once all callers are updated.
 */
extern const int mcs_subslot_bits[MCS_COUNT][SUBSLOT_COUNT];

/* ------------------------------------------------------------------ */
/*  Lookup function                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Get the payload size in bits for a given MCS and subslot.
 *
 * @param mcs     MCS index. Valid range: [0, MCS_COUNT).
 * @param subslot Subslot count. Valid range: [0, SUBSLOT_COUNT).
 *
 * @retval >0           Payload size in bits.
 * @retval INVALID_BYTES  Combination not supported, or arguments out of range.
 */
int get_bit_count(int mcs, int subslot);

/**
 * @brief Get the send time duration for a subslot length
 *
 * @param ss Subslot count. Valid range: [0, SUBSLOT_COUNT).
 *
 * @retval duration in micro seconds
 */
uint64_t get_delay_us(uint8_t ss);

extern struct k_sem operation_sem;
extern struct k_sem rx_sem;
extern struct k_sem tx_sem;
extern struct k_sem time_sem;
extern struct k_sem tdma_sem;

/**
 * @brief Semaphore signalled by on_deinit and on_deactivate callbacks.
 *
 * Separate from operation_sem because deactivate/deinit follow a different
 * lifecycle path than normal operations.
 */
extern struct k_sem deinit_sem;

/**
 * @brief Set to true by on_init or on_activate if a fatal modem error occurs.
 *
 * Checked after k_sem_take(operation_sem) during init sequence.
 * If true, the modem cannot be used and the application should shut down.
 */
extern volatile bool phy_fatal_error;

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */
/**
 * @brief Initialise the DECT PHY modem.
 *
 * Registers the event handler, initialises, configures, and activates
 * the modem in low-latency radio mode. Blocks until each step completes.
 *
 * Must be called after nrf_modem_lib_init() and before any TX/RX operation.
 *
 * @retval  0       Success.
 * @retval -EIO     Fatal modem error during init or activate.
 * @retval -errno   Modem API error.
 */
int dect_phy_init(void);

/**
 * @brief Deactivate and deinitialise the DECT PHY modem.
 *
 * Blocks until deactivation and deinit complete.
 * Call nrf_modem_lib_shutdown() after this returns.
 *
 * @retval  0       Success.
 * @retval -errno   Modem API error.
 */
int dect_phy_deinit(void);

/* ------------------------------------------------------------------ */
/*  Primitives (available for advanced use — prefer dect_run_task)    */
/* ------------------------------------------------------------------ */

/**
 * @brief Transmit a packet.
 *
 * Assembles TX params and calls nrf_modem_dect_phy_tx(). Does not wait
 * for completion — caller must k_sem_take(operation_sem) afterwards.
 *
 * @param handle      TX operation handle. Must be unique per in-flight op.
 * @param hdr         PHY header type 1 to transmit.
 * @param payload_len Payload length in bytes.
 * @param buf         Payload buffer. Must remain valid until op completes.
 * @param start_time        Time at which to send package. Leave at 0 for instanttransmit
 *
 * @retval  0       Success.
 * @retval -errno   Modem API error.
 */
int dect_phy_tx(uint32_t handle,
                struct nrf_modem_dect_phy_hdr_type_1 *hdr,
                uint32_t payload_len,
                uint8_t *buf,
                uint64_t start_time);

/**
 * @brief Start a receive operation.
 *
 * Assembles RX params and calls nrf_modem_dect_phy_rx(). Does not wait
 * for completion — caller must k_sem_take(operation_sem) afterwards.
 *
 * @param handle   RX operation handle.
 * @param rx_mode  Reception mode (single shot, continuous, etc.).
 * @param start_time        Time at which to send package. Leave at 0 for instanttransmit
 *
 * @retval  0       Success.
 * @retval -errno   Modem API error.
 */
int dect_phy_rx(uint32_t handle, enum nrf_modem_dect_phy_rx_mode rx_mode,uint64_t start_time, uint32_t duration);

#endif /* DECT_PHY_H */
