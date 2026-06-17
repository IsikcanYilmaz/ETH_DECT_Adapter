/*
 * dect_phy_events.c — DECT PHY event dispatcher and application hook registration
 *
 * Owns:
 *   - All on_* internal callbacks
 *   - modem_time and last_rx_hdr shared state
 *   - Application handler registration (setters)
 *   - The top-level dect_phy_event_handler dispatcher
 *
 * Does NOT own:
 *   - Semaphores (defined in dect_phy.c, extern'd here)
 *   - TX/RX logic
 *   - Any task-specific state (RTT timestamps, echo flags, etc.)
 */

#include "dect_phy_events.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(dect_phy_events,LOG_LEVEL_ERR);

/* ------------------------------------------------------------------ */
/*  Shared modem state                                                 */
/* ------------------------------------------------------------------ */

volatile uint64_t modem_time;
struct nrf_modem_dect_phy_latency_info *latency_info;
struct nrf_modem_dect_phy_hdr_type_1 last_rx_hdr;

/* ------------------------------------------------------------------ */
/*  Semaphores owned by dect_phy.c                                     */
/*                                                                      */
/*  extern here because the event callbacks are what signal them, but  */
/*  the semaphores themselves are defined and waited on in dect_phy.c. */
/* ------------------------------------------------------------------ */
extern struct k_sem operation_sem;
extern struct k_sem deinit_sem;
extern struct k_sem rx_sem;
extern struct k_sem tx_sem;
extern struct k_sem time_sem;
extern struct k_sem tdma_sem;

/* Flag set by on_init / on_activate to signal fatal error to main.    */
/** EVIL */
extern volatile bool phy_fatal_error;

/* ------------------------------------------------------------------ */
/*  Registered application handlers (private)                          */
/* ------------------------------------------------------------------ */

static pdc_handler_t         app_pdc_handler         = NULL;
static pcc_handler_t         app_pcc_handler         = NULL;
static pdc_crc_err_handler_t app_pdc_crc_err_handler = NULL;
static capability_get_handler_t app_capabilities_get_handler = NULL;

/* ------------------------------------------------------------------ */
/*  Setter implementations                                             */
/* ------------------------------------------------------------------ */

void dect_events_register_pdc_handler(pdc_handler_t handler)
{
  app_pdc_handler = handler;
}

void dect_events_register_pcc_handler(pcc_handler_t handler)
{
  app_pcc_handler = handler;
}

void dect_events_register_pdc_crc_err_handler(pdc_crc_err_handler_t handler)
{
  app_pdc_crc_err_handler = handler;
}

void dect_events_register_capability_get_handler(capability_get_handler_t handler)
{
  app_capabilities_get_handler = handler;
}

/* ------------------------------------------------------------------ */
/*  Internal callbacks                                                 */
/* ------------------------------------------------------------------ */

static void on_init(const struct nrf_modem_dect_phy_init_event *evt)
{
  if (evt->err) {
    LOG_ERR("Init failed, err %d", evt->err);
    phy_fatal_error = true;
    return;
  }

  LOG_DBG("on_init: giving operation_sem (err=%d)", evt->err);
  k_sem_give(&operation_sem);
}

static void on_deinit(const struct nrf_modem_dect_phy_deinit_event *evt)
{
  if (evt->err) {
    LOG_ERR("Deinit failed, err %d", evt->err);
    return;
  }

  LOG_DBG("on_deinit: giving deinit_sem (err=%d)", evt->err);
  k_sem_give(&deinit_sem);
}

static void on_activate(const struct nrf_modem_dect_phy_activate_event *evt)
{
  if (evt->err) {
    LOG_ERR("Activate failed, err %d", evt->err);
    phy_fatal_error = true;
    return;
  }

  LOG_DBG("on_activate: giving operation_sem (err=%d)", evt->err);
  k_sem_give(&operation_sem);
}

static void on_deactivate(const struct nrf_modem_dect_phy_deactivate_event *evt)
{
  if (evt->err) {
    LOG_ERR("Deactivate failed, err %d", evt->err);
    return;
  }

  LOG_DBG("on_deactivate: giving deinit_sem (err=%d)", evt->err);
  k_sem_give(&deinit_sem);
}

static void on_configure(const struct nrf_modem_dect_phy_configure_event *evt)
{
  if (evt->err) {
    LOG_ERR("Configure failed, err %d", evt->err);
    return;
  }

  LOG_DBG("on_configure: giving operation_sem (err=%d)", evt->err);
  k_sem_give(&operation_sem);
}

static void on_radio_config(const struct nrf_modem_dect_phy_radio_config_event *evt)
{
  if (evt->err) {
    LOG_ERR("Radio config failed, err %d", evt->err);
    return;
  }

  LOG_DBG("on_radio_config: giving operation_sem (err=%d)", evt->err);
  k_sem_give(&operation_sem);
}

static void on_op_complete(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
  LOG_DBG("on_op_complete: time=%" PRIu64 " err=%d -> giving operation_sem + tx_sem", modem_time, evt->err);

  k_sem_give(&operation_sem);
  k_sem_give(&tdma_sem);
}

static void on_cancel(const struct nrf_modem_dect_phy_cancel_event *evt)
{
  LOG_DBG("on_cancel: err=%d -> giving operation_sem", evt->err);

  k_sem_give(&operation_sem);
}
static void on_time_get(const struct nrf_modem_dect_phy_time_get_event *evt)
{
  LOG_DBG("on_time_get: time=%" PRIu64 " err=%d -> giving time_sem", modem_time, evt->err);

  k_sem_give(&time_sem);
}

/* ------------------------------------------------------------------ */
/*  Application-hookable callbacks                                     */
/* ------------------------------------------------------------------ */

/**
 * Captures the header into last_rx_hdr (always), then calls the
 * optional application PCC handler.
 */
static void on_pcc(const struct nrf_modem_dect_phy_pcc_event *evt)
{
  LOG_DBG("PCC from device ID %d",
          evt->hdr.hdr_type_1.transmitter_id_hi << 8 |
          evt->hdr.hdr_type_1.transmitter_id_lo);
  LOG_DBG("  Short Network ID  : %u", evt->hdr.hdr_type_1.short_network_id);
  LOG_DBG("  Header Format     : %u", evt->hdr.hdr_type_1.header_format);
  LOG_DBG("  Packet Length     : %u %s",
          evt->hdr.hdr_type_1.packet_length,
          evt->hdr.hdr_type_1.packet_length_type ? "slots" : "subslots");
  LOG_DBG("  DF MCS            : %u", evt->hdr.hdr_type_1.df_mcs);
  LOG_DBG("  Transmit Power    : %u", evt->hdr.hdr_type_1.transmit_power);

  /* Always capture — tasks read this without registering a PCC handler. */
  last_rx_hdr = evt->hdr.hdr_type_1;

  if (app_pcc_handler) {
    app_pcc_handler(evt);
  }
}

static void on_pcc_crc_err(const struct nrf_modem_dect_phy_pcc_crc_failure_event *evt)
{
  LOG_DBG("pcc_crc_err cb time %" PRIu64, modem_time);
  /* No application hook for PCC CRC errors — add one here if needed. */
}

/**
 * Forwards raw payload to the registered application handler.
 * The handler is responsible for all task-specific logic
 * (echo, timestamping, buffering, etc.).
 */
static void on_pdc(const struct nrf_modem_dect_phy_pdc_event *evt)
{
  LOG_DBG("PDC data len: %d", evt->len);
  LOG_HEXDUMP_DBG(evt->data, evt->len, "PDC Payload");

  if (app_pdc_handler) {
    app_pdc_handler(evt);
  }
}

static void on_pdc_crc_err(const struct nrf_modem_dect_phy_pdc_crc_failure_event *evt)
{
  LOG_DBG("pdc_crc_err cb time %" PRIu64, modem_time);

  if (app_pdc_crc_err_handler) {
    app_pdc_crc_err_handler(modem_time);
  }
}

/* ------------------------------------------------------------------ */
/*  Informational callbacks (no application hook needed)              */
/* ------------------------------------------------------------------ */

static void on_rssi(const struct nrf_modem_dect_phy_rssi_event *evt)
{
  LOG_DBG("rssi cb time %" PRIu64 " carrier %d", modem_time, evt->carrier);
}

static void on_capability_get(const struct nrf_modem_dect_phy_capability_get_event *evt)
{
  if (app_capabilities_get_handler != NULL)
  {
    app_capabilities_get_handler(evt);
  }
  LOG_DBG("capability_get cb time %" PRIu64 " status %d", modem_time, evt->err);
}

static void on_bands_get(const struct nrf_modem_dect_phy_band_get_event *evt)
{
  LOG_DBG("bands_get cb status %d", evt->err);
}

static void on_latency_info_get(const struct nrf_modem_dect_phy_latency_info_event *evt)
{
  if (evt->err) 
  {
    LOG_ERR("latency_info_get cb status %d", evt->err);
    return;
  }
  latency_info = evt->latency_info;
}

static void on_link_config(const struct nrf_modem_dect_phy_link_config_event *evt)
{
  LOG_DBG("link_config cb time %" PRIu64 " status %d", modem_time, evt->err);
}

static void on_stf_cover_seq_control(const struct nrf_modem_dect_phy_stf_control_event *evt)
{
  LOG_WRN("Unexpectedly in %s", __func__);
}

static void on_test_rf_tx_cw_ctrl(
  const struct nrf_modem_dect_phy_test_rf_tx_cw_control_event *evt)
{
  LOG_WRN("Unexpectedly in %s", __func__);
}

/* ------------------------------------------------------------------ */
/*  Top-level dispatcher                                               */
/* ------------------------------------------------------------------ */

void dect_phy_event_handler(const struct nrf_modem_dect_phy_event *evt)
{
  /* Update shared timestamp first — all callbacks may read it. */
  modem_time = evt->time;

  switch (evt->id) {
    case NRF_MODEM_DECT_PHY_EVT_INIT:
      on_init(&evt->init);
      break;
    case NRF_MODEM_DECT_PHY_EVT_DEINIT:
      on_deinit(&evt->deinit);
      break;
    case NRF_MODEM_DECT_PHY_EVT_ACTIVATE:
      on_activate(&evt->activate);
      break;
    case NRF_MODEM_DECT_PHY_EVT_DEACTIVATE:
      on_deactivate(&evt->deactivate);
      break;
    case NRF_MODEM_DECT_PHY_EVT_CONFIGURE:
      on_configure(&evt->configure);
      break;
    case NRF_MODEM_DECT_PHY_EVT_RADIO_CONFIG:
      on_radio_config(&evt->radio_config);
      break;
    case NRF_MODEM_DECT_PHY_EVT_COMPLETED:
      on_op_complete(&evt->op_complete);
      break;
    case NRF_MODEM_DECT_PHY_EVT_CANCELED:
      on_cancel(&evt->cancel);
      break;
    case NRF_MODEM_DECT_PHY_EVT_RSSI:
      on_rssi(&evt->rssi);
      break;
    case NRF_MODEM_DECT_PHY_EVT_PCC:
      on_pcc(&evt->pcc);
      break;
    case NRF_MODEM_DECT_PHY_EVT_PCC_ERROR:
      on_pcc_crc_err(&evt->pcc_crc_err);
      break;
    case NRF_MODEM_DECT_PHY_EVT_PDC:
      on_pdc(&evt->pdc);
      break;
    case NRF_MODEM_DECT_PHY_EVT_PDC_ERROR:
      on_pdc_crc_err(&evt->pdc_crc_err);
      break;
    case NRF_MODEM_DECT_PHY_EVT_TIME:
      on_time_get(&evt->time_get);
      break;
    case NRF_MODEM_DECT_PHY_EVT_CAPABILITY:
      on_capability_get(&evt->capability_get);
      break;
    case NRF_MODEM_DECT_PHY_EVT_BANDS:
      on_bands_get(&evt->band_get);
      break;
    case NRF_MODEM_DECT_PHY_EVT_LATENCY:
      on_latency_info_get(&evt->latency_get);
      break;
    case NRF_MODEM_DECT_PHY_EVT_LINK_CONFIG:
      on_link_config(&evt->link_config);
      break;
    case NRF_MODEM_DECT_PHY_EVT_STF_CONFIG:
      on_stf_cover_seq_control(&evt->stf_cover_seq_control);
      break;
    case NRF_MODEM_DECT_PHY_EVT_TEST_RF_TX_CW_CONTROL_CONFIG:
      on_test_rf_tx_cw_ctrl(&evt->test_rf_tx_cw_control);
      break;
    default:
      LOG_WRN("Unhandled event id: %d", evt->id);
      break;
  }
}
