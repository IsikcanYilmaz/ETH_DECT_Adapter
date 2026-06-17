/*
 * dect_phy.c — DECT NR+ PHY radio abstraction layer
 *
 * Owns:
 *   - operation_sem and deinit_sem (signalled by dect_phy_events.c)
 *   - phy_fatal_error flag
 *   - Modem lifecycle: init, configure, activate, deactivate, deinit
 *   - TX and RX primitives
 *   - dect_run_task() — generalised TX/RX loop
 *
 * Does NOT own:
 *   - Event callbacks (dect_phy_events.c)
 *   - Application logic (rtt.c, one_way_delay.c, etc.)
 *   - Radio parameter tables (radio_config.c)
 */

#include "dect_phy.h"
#include "dect_phy_events.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrf_modem_dect_phy.h>

LOG_MODULE_REGISTER(dect_phy,LOG_LEVEL_INF);

uint64_t get_delay_us(uint8_t ss){
  return ss * ((10 /*ms*/ * 1000  /* to us */)/24 /*to slots*/ )/16 /* to subslots */ ;
}

int get_bit_count(int mcs, int subslot)
{
  if (mcs < 0 || mcs >= MCS_COUNT ||
    subslot < 0 || subslot >= SUBSLOT_COUNT) {
    return INVALID_BYTES;
  }

  return mcs_subslot_bits[mcs][subslot];
}

K_SEM_DEFINE(operation_sem, 0, 1);
K_SEM_DEFINE(deinit_sem,    0, 1);
K_SEM_DEFINE(time_sem,    0, 1);
K_SEM_DEFINE(tx_sem,    0, 1);
K_SEM_DEFINE(rx_sem,    0, 1);

volatile bool phy_fatal_error = false;

/* ------------------------------------------------------------------ */
/*  Modem configuration                                                */
/*                                                                      */
/*  band_group_index: 1 for ETSI band 1 (carriers 525–551),           */
/*                    0 for all other carriers.                        */
/*  TODO: check if HARQ should be enabled for specific tasks.         */
/* ------------------------------------------------------------------ */
static const struct nrf_modem_dect_phy_config_params phy_config = {
  .band_group_index       = ((CONFIG_CARRIER >= 525 && CONFIG_CARRIER <= 551) ? 1 : 0),
  .harq_rx_process_count  = 4,
  .harq_rx_expiry_time_us = 5000000,
};

/*  Public fns                                                        */
int dect_phy_init(void)
{
  int err;

  /* Bind our event dispatcher before touching the modem. */
  err = nrf_modem_dect_phy_event_handler_set(dect_phy_event_handler);
  if (err) {
    LOG_ERR("event_handler_set failed, err %d", err);
    return err;
  }

  err = nrf_modem_dect_phy_init();
  if (err) {
    LOG_ERR("nrf_modem_dect_phy_init failed, err %d", err);
    return err;
  }

  k_sem_take(&operation_sem, K_FOREVER);
  if (phy_fatal_error) {
    LOG_ERR("Fatal error during PHY init");
    return -EIO;
  }
  nrf_modem_dect_phy_latency_get();

  err = nrf_modem_dect_phy_configure(&phy_config);
  if (err) {
    LOG_ERR("nrf_modem_dect_phy_configure failed, err %d", err);
    return err;
  }

  k_sem_take(&operation_sem, K_FOREVER);
  if (phy_fatal_error) {
    LOG_ERR("Fatal error during PHY configure");
    return -EIO;
  }
    
  nrf_modem_dect_phy_capability_get();
  k_sem_take(&operation_sem, K_FOREVER);
  if (phy_fatal_error) {
    LOG_ERR("Fatal error during PHY capa get");
    return -EIO;
  }

  err = nrf_modem_dect_phy_activate(NRF_MODEM_DECT_PHY_RADIO_MODE_LOW_LATENCY);
  if (err) {
    LOG_ERR("nrf_modem_dect_phy_activate failed, err %d", err);
    return err;
  }

  k_sem_take(&operation_sem, K_FOREVER);
  if (phy_fatal_error) {
    LOG_ERR("Fatal error during PHY activate");
    return -EIO;
  }

  LOG_DBG("DECT PHY initialised");
  return 0;
}

int dect_phy_deinit(void)
{
  int err;

  err = nrf_modem_dect_phy_deactivate();
  if (err) {
    LOG_ERR("nrf_modem_dect_phy_deactivate failed, err %d", err);
    return err;
  }

  k_sem_take(&deinit_sem, K_FOREVER);

  err = nrf_modem_dect_phy_deinit();
  if (err) {
    LOG_ERR("nrf_modem_dect_phy_deinit failed, err %d", err);
    return err;
  }

  k_sem_take(&deinit_sem, K_FOREVER);

  LOG_INF("DECT PHY deinitialised");
  return 0;
}

/*  TX primitive        */
/** TODO Shouldnt functions be static */
int dect_phy_tx(uint32_t handle,
                struct nrf_modem_dect_phy_hdr_type_1 *hdr,
                uint32_t payload_len,
                uint8_t *buf,
                uint64_t start_time)
{
  /* phy_hdr must persist until the TX operation completes. */
  static union nrf_modem_dect_phy_hdr phy_hdr;
  phy_hdr.hdr_type_1 = *hdr;

  struct nrf_modem_dect_phy_tx_params tx_params = {
    .start_time           = start_time,
    .handle               = handle,
    .network_id           = CONFIG_NETWORK_ID,
    .phy_type             = 0,
    .lbt_rssi_threshold_max = 0,
    .carrier              = CONFIG_CARRIER,
    .lbt_period           = 0,
    .phy_header           = &phy_hdr,
    .data                 = buf,
    .data_size            = payload_len,
  };

  int err = nrf_modem_dect_phy_tx(&tx_params);
  if (err) {
    LOG_ERR("nrf_modem_dect_phy_tx failed, err %d", err);
  }

  return err;
}

/*  RX primitive                                                       */
int dect_phy_rx(uint32_t handle, enum nrf_modem_dect_phy_rx_mode rx_mode,uint64_t start_time, uint32_t duration)
{
  const struct nrf_modem_dect_phy_rx_params rx_params = {
    .start_time                   = start_time,
    .handle                       = handle,
    .network_id                   = CONFIG_NETWORK_ID,
    .mode                         = rx_mode,
    .rssi_interval                = NRF_MODEM_DECT_PHY_RSSI_INTERVAL_OFF,
    .link_id                      = NRF_MODEM_DECT_PHY_LINK_UNSPECIFIED,
    .rssi_level                   = 0,
    .carrier                      = CONFIG_CARRIER,
    .duration                     = duration,//CONFIG_RX_TIMEOUT_MS * NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ,
    .filter.short_network_id      = CONFIG_NETWORK_ID & 0xff,
    .filter.is_short_network_id_used = 1,
    .filter.receiver_identity     = 0,   /* broadcast — accept all */
  };

  int err = nrf_modem_dect_phy_rx(&rx_params);
  if (err) {
    LOG_ERR("nrf_modem_dect_phy_rx failed, err %d", err);
  }

  return err;
}
