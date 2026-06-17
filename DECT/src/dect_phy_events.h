/*
 * dect_phy_events.h — DECT PHY event dispatcher and application hook registration
 *
 * This module owns the single Zephyr-facing event handler and dispatches
 * each event to an internal on_* callback.
 *
 * For events that carry application-specific behaviour (pcc, pdc, pdc_crc_err),
 * the application registers a handler at runtime via the register functions
 * below.  All other events are handled internally (semaphore signalling,
 * logging) and do not need application involvement.
 *
 * Usage:
 *   1. Call dect_events_register_pdc_handler() before starting any task.
 *   2. Pass dect_phy_event_handler to nrf_modem_dect_phy_event_handler_set().
 */

#ifndef DECT_PHY_EVENTS_H
#define DECT_PHY_EVENTS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <nrf_modem_dect_phy.h>

extern volatile uint64_t modem_time;
extern struct nrf_modem_dect_phy_latency_info *latency_info;
/* ------------------------------------------------------------------ */
/*  Application callback types                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Called when a PDC (data) event is received.
 *
 * @param data  Pointer to received payload. Valid only during the callback.
 * @param len   Length of @p data in bytes.
 */
typedef void (*pdc_handler_t)(const struct nrf_modem_dect_phy_pdc_event *evt);

/**
 * @brief Called when a PCC (header) event is received.
 *
 * @param hdr  Pointer to the received PHY header type 1.
 */
typedef void (*pcc_handler_t)(const struct nrf_modem_dect_phy_pcc_event *evt);

/**
 * @brief Called when a PDC CRC error is detected.
 *
 * @param time  Modem timestamp of the error.
 */
typedef void (*pdc_crc_err_handler_t)(uint64_t time);

/**
 * @brief Called when a capabilities get response comes in
 *
 * @param evt
 */
typedef void (*capability_get_handler_t)(const struct nrf_modem_dect_phy_capability_get_event *evt);

/* ------------------------------------------------------------------ */
/*  Handler registration (setters)                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Register the application PDC handler.
 *
 * Replaces any previously registered handler. Pass NULL to clear.
 * Must be called before starting a task that expects data reception.
 *
 * @param handler  Function to call on PDC event. May be NULL.
 */
void dect_events_register_pdc_handler(pdc_handler_t handler);

/**
 * @brief Register the application PCC handler.
 *
 * @param handler  Function to call on PCC event. May be NULL.
 */
void dect_events_register_pcc_handler(pcc_handler_t handler);

/**
 * @brief Register the application PDC CRC error handler.
 *
 * Useful for tasks that track error rates (e.g. link quality measurement).
 *
 * @param handler  Function to call on PDC CRC error. May be NULL.
 */
void dect_events_register_pdc_crc_err_handler(pdc_crc_err_handler_t handler);

/**
 * @brief Register the application for capability get handler
 *
 * @param handler function to call on capability get
 */
void dect_events_register_capability_get_handler(capability_get_handler_t handler);

/* ------------------------------------------------------------------ */
/*  Zephyr-facing event handler                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Top-level DECT PHY event handler.
 *
 * Pass this to nrf_modem_dect_phy_event_handler_set() during initialisation.
 * Do not call directly.
 */
void dect_phy_event_handler(const struct nrf_modem_dect_phy_event *evt);

/* ------------------------------------------------------------------ */
/*  Getter function                                                   */
/* ------------------------------------------------------------------ */
#endif /* DECT_PHY_EVENTS_H */
