
#include <zephyr/kernel.h>
#include <string.h>
#include <stdbool.h>
#include <nrf_modem_dect_phy.h>
#include <nrf_modem_dect_clock_sync.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include "phy_main.h"

LOG_MODULE_REGISTER(dect_phy, LOG_LEVEL_WRN);

#define CONFIG_CARRIER (1677) // from overlay-eu.conf

extern struct k_queue ethTxQueue;
extern struct k_queue ethRxQueue;

typedef struct Ping_s
{
  char text[4]; // text that says TEST
  uint64_t ts1; // master send ts
  uint64_t ts2; // slave receive ts
} __attribute__((packed)) Ping_t;

static size_t len; // test ping
static char pingBuf[128]; // test ping buffer
static Ping_t *responsePing = (Ping_t *) pingBuf; // TODO remove all of these eventually

static bool exit;
static uint16_t device_id;
static uint64_t modem_time;
static uint64_t lastPccTs = 0;
static uint64_t lastPdcTs = 0;

uint32_t tx_handle = 0;
uint32_t rx_handle = 1;
static bool waitingForRx = false; // TODO may not be necessary

static bool iAmMaster;

/* Header type 1, due to endianness the order is different than in the specification. */
struct phy_ctrl_field_common {
	uint32_t packet_length : 4;
	uint32_t packet_length_type : 1;
	uint32_t header_format : 3;
	uint32_t short_network_id : 8;
	uint32_t transmitter_id_hi : 8;
	uint32_t transmitter_id_lo : 8;
	uint32_t df_mcs : 3;
	uint32_t reserved : 1;
	uint32_t transmit_power : 4;
	uint32_t pad : 24;
};

/* Dect PHY config parameters. */
static struct nrf_modem_dect_phy_config_params dect_phy_config_params = {
	.band_group_index = ((CONFIG_CARRIER >= 525 && CONFIG_CARRIER <= 551)) ? 1 : 0,
	.harq_rx_process_count = 4,
	.harq_rx_expiry_time_us = 5000000,
};

K_SEM_DEFINE(operation_sem, 0, 1);
K_SEM_DEFINE(rx_sem, 0, 1);
K_SEM_DEFINE(time_sem, 0, 1);
K_SEM_DEFINE(deinit_sem, 0, 1);

/* Callback after init operation. */
static void on_init(const struct nrf_modem_dect_phy_init_event *evt)
{
	if (evt->err) {
		LOG_ERR("Init failed, err %d", evt->err);
		exit = true;
		return;
	}
	k_sem_give(&operation_sem);
}

static void on_configure(const struct nrf_modem_dect_phy_configure_event *evt)
{
	if (evt->err) {
		LOG_ERR("Configure failed, err %d", evt->err);
		return;
	}
	k_sem_give(&operation_sem);
}

static void on_activate(const struct nrf_modem_dect_phy_activate_event *evt)
{
	if (evt->err) {
		LOG_ERR("Activate failed, err %d", evt->err);
		exit = true;
		return;
	}
	k_sem_give(&operation_sem);
}

static void on_capability_get(const struct nrf_modem_dect_phy_capability_get_event *evt)
{
	LOG_DBG("capability_get cb time %"PRIu64" status %d", modem_time, evt->err);
}

static void on_op_complete(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
	LOG_DBG("op_complete cb time %"PRIu64" status %d handle %d", modem_time, evt->err, evt->handle);
  if (evt->handle == tx_handle)
  {
    LOG_DBG("TX op_complete. handle : %d", tx_handle);
  }
  else if (evt->handle == rx_handle)
  {
    LOG_DBG("RX op_complete. handle : %d", rx_handle);
  }
	k_sem_give(&operation_sem);
}

static void on_pcc(const struct nrf_modem_dect_phy_pcc_event *evt)
{
	LOG_INF("PCC Received header from device ID %d", evt->hdr.hdr_type_1.transmitter_id_hi << 8 | evt->hdr.hdr_type_1.transmitter_id_lo);
  lastPccTs = modem_time;
}

static void on_pcc_crc_err(const struct nrf_modem_dect_phy_pcc_crc_failure_event *evt)
{
	LOG_DBG("pcc_crc_err cb time %"PRIu64"", modem_time);
}

static void on_pdc(const struct nrf_modem_dect_phy_pdc_event *evt)
{
	/* Received RSSI value is in fixed precision format Q14.1 */
	LOG_DBG("PDC Received data (RSSI: %d.%d): %s", (evt->rssi_2 / 2), (evt->rssi_2 & 0b1) * 5, (char *)evt->data);

  lastPdcTs = modem_time;

  if (waitingForRx && evt->handle == rx_handle)  // JON TODO this makes it so that we dont wait for more than one rx
  {
    nrf_modem_dect_phy_cancel(rx_handle);
    waitingForRx = false;
  }

  if (!iAmMaster) // If we are the PT we return the ping
  {
    memcpy(pingBuf, evt->data, sizeof(Ping_t));
    responsePing->ts2 = modem_time;
  }

  if (iAmMaster) // If we are the FT then we print the ping
  {
    memcpy(pingBuf, evt->data, sizeof(Ping_t));
    LOG_WRN("Ping response received. ts1: %llu, ts2: %llu, ts3: %llu, data: %s", responsePing->ts1, responsePing->ts2, modem_time, responsePing->text);
    uint64_t diff = modem_time - responsePing->ts1;
    LOG_WRN("Diff %llu ticks,  %llu ms", diff, diff / NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ);
  }

  k_sem_give(&rx_sem);
}

static void on_pdc_crc_err(const struct nrf_modem_dect_phy_pdc_crc_failure_event *evt)
{
	LOG_DBG("pdc_crc_err cb time %"PRIu64"", modem_time);
}

static void on_latency_info_get(const struct nrf_modem_dect_phy_latency_info_event *evt)
{
	LOG_DBG("latency_info_get cb status %d", evt->err);
}

static void on_time_get(const struct nrf_modem_dect_phy_time_get_event *evt)
{
	LOG_DBG("time_get cb time %"PRIu64" status %d", modem_time, evt->err);
  k_sem_give(&time_sem);
}

static void dect_phy_event_handler(const struct nrf_modem_dect_phy_event *evt)
{
  modem_time = evt->time;
  LOG_DBG("%s modem_time %ull Event %d", __FUNCTION__, evt->time, evt->id);
	switch (evt->id) {
	case NRF_MODEM_DECT_PHY_EVT_INIT:
		on_init(&evt->init);
		break;
	case NRF_MODEM_DECT_PHY_EVT_DEINIT:
		// on_deinit(&evt->deinit);
		break;
	case NRF_MODEM_DECT_PHY_EVT_ACTIVATE:
		on_activate(&evt->activate);
		break;
	case NRF_MODEM_DECT_PHY_EVT_DEACTIVATE:
		// on_deactivate(&evt->deactivate);
		break;
	case NRF_MODEM_DECT_PHY_EVT_CONFIGURE:
		on_configure(&evt->configure);
		break;
	case NRF_MODEM_DECT_PHY_EVT_RADIO_CONFIG:
		// on_radio_config(&evt->radio_config);
		break;
	case NRF_MODEM_DECT_PHY_EVT_COMPLETED:
		on_op_complete(&evt->op_complete);
		break;
	case NRF_MODEM_DECT_PHY_EVT_CANCELED:
		// on_cancel(&evt->cancel);
		break;
	case NRF_MODEM_DECT_PHY_EVT_RSSI:
		// on_rssi(&evt->rssi);
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
		// on_bands_get(&evt->band_get);
		break;
	case NRF_MODEM_DECT_PHY_EVT_LATENCY:
		on_latency_info_get(&evt->latency_get);
		break;
	case NRF_MODEM_DECT_PHY_EVT_LINK_CONFIG:
		// on_link_config(&evt->link_config);
		break;
	case NRF_MODEM_DECT_PHY_EVT_STF_CONFIG:
		// on_stf_cover_seq_control(&evt->stf_cover_seq_control);
		break;
	case NRF_MODEM_DECT_PHY_EVT_TEST_RF_TX_CW_CONTROL_CONFIG:
		// on_test_rf_tx_cw_ctrl(&evt->test_rf_tx_cw_control);
		break;
	}
}

// CLOCK SYNC CODE
static void on_clk_sync_enable(const struct nrf_modem_dect_clock_sync_event *evt)
{

}

static void on_clk_sync_state(const struct nrf_modem_dect_clock_sync_event *evt)
{
}

static void dect_clock_sync_event_handler(const struct nrf_modem_dect_clock_sync_event *evt)
{
  switch (evt->id)
  {
    case NRF_MODEM_DECT_CLOCK_SYNC_EVT_ENABLE:
      {
        on_clk_sync_enable(evt);
        break;
      }
    case NRF_MODEM_DECT_CLOCK_SYNC_EVT_STATE:
      {
        on_clk_sync_state(evt);
        break;
      }
    default:
    break;
  }
}

// CLOCK SYNC CODE END 

static int transmit(uint32_t handle, void *data, size_t data_len, uint64_t start_time)
{
  int err;

  struct phy_ctrl_field_common header = {
    .header_format = 0x0,
    .packet_length_type = 0x0,
    .packet_length = 0x01,
    .short_network_id = (CONFIG_APP_NETWORK_ID & 0xff),
    .transmitter_id_hi = (device_id >> 8),
    .transmitter_id_lo = (device_id & 0xff),
    .transmit_power = CONFIG_APP_TX_POWER,
    .reserved = 0,
    .df_mcs = CONFIG_APP_MCS,
  };

  struct nrf_modem_dect_phy_tx_params tx_op_params = {
    .start_time = start_time,
    .handle = handle,
    .network_id = CONFIG_APP_NETWORK_ID,
    .phy_type = 0,
    .lbt_rssi_threshold_max = 0,
    .carrier = CONFIG_CARRIER,
    .lbt_period = NRF_MODEM_DECT_LBT_PERIOD_MAX, // JON EXPERIMENTAL
    .phy_header = (union nrf_modem_dect_phy_hdr *)&header,
    .data = data,
    .data_size = data_len,
  };

  LOG_DBG("Transmitting %d bytes", data_len);

  err = nrf_modem_dect_phy_tx(&tx_op_params);
	if (err != 0) {
		return err;
	}

	return 0;
}

static int receive(uint32_t handle, uint32_t durationMs, uint64_t start_time)
{
  int err;

	struct nrf_modem_dect_phy_rx_params rx_op_params = {
		.start_time = start_time,
		.handle = handle,
		.network_id = CONFIG_APP_NETWORK_ID,
		.mode = NRF_MODEM_DECT_PHY_RX_MODE_CONTINUOUS,
		.rssi_interval = NRF_MODEM_DECT_PHY_RSSI_INTERVAL_OFF,
		.link_id = NRF_MODEM_DECT_PHY_LINK_UNSPECIFIED,
		.rssi_level = -60,
		.carrier = CONFIG_CARRIER,
		.duration = durationMs * NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ,
		.filter.short_network_id = CONFIG_APP_NETWORK_ID & 0xff,
		.filter.is_short_network_id_used = 1,
		/* listen for everything (broadcast mode used) */
		.filter.receiver_identity = 0,
	};

  waitingForRx = true;
	err = nrf_modem_dect_phy_rx(&rx_op_params);
	if (err != 0) {
		return err;
	}

	return 0;
}

// Public fns
int DectPhy_Init(void)
{
  int err;

  err = nrf_modem_lib_init();
  if (err) {
		LOG_ERR("modem init failed, err %d", err);
		return err;
	}

  err = nrf_modem_dect_phy_event_handler_set(dect_phy_event_handler);
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_event_handler_set failed, err %d", err);
		return err;
	}

  err = nrf_modem_dect_phy_init();
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_init failed, err %d", err);
		return err;
	}

	k_sem_take(&operation_sem, K_FOREVER);
	if (exit) {
		return -EIO;
	}
  ////////////////////////////////////// on_init will release the semaphore

	err = nrf_modem_dect_phy_configure(&dect_phy_config_params);
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_configure failed, err %d", err);
		return err;
	}

	k_sem_take(&operation_sem, K_FOREVER);
	if (exit) {
		return -EIO;
	}
  ////////////////////////////////////// on_configure will release the semaphore
  
	err = nrf_modem_dect_phy_activate(NRF_MODEM_DECT_PHY_RADIO_MODE_LOW_LATENCY);
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_activate failed, err %d", err);
		return err;
	}

	k_sem_take(&operation_sem, K_FOREVER);
	if (exit) {
		return -EIO;
	}
  ///////////////////////////////////// on_activate will release the semaphore
	
  hwinfo_get_device_id((void *)&device_id, sizeof(device_id));
	
  LOG_INF("Dect NR+ PHY initialized, device ID: %d", device_id);

  err = nrf_modem_dect_phy_capability_get();
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_capability_get failed, err %d", err);
	}

  ///////////////////////////////////// clock sync TODO look into if we want this
  // nrf_modem_dect_clock_sync_event_handler_set(dect_clock_sync_event_handler);
  // nrf_modem_dect_clock_sync_enable();
}


void DectPhy_Main(bool master)
{	
  DectPhy_Init();
  int err;
  iAmMaster = master;
  
  Ping_t pingPkt;
  strncpy(&pingPkt.text, "TEST", 4);

  nrf_modem_dect_phy_time_get(); 
  k_sem_take(&time_sem, K_FOREVER);

  // if (!master)
  // {
  //   err = receive(rx_handle, 50 * MSEC_PER_SEC, 0); // Wait for the first beacon
  //   k_sem_take(&operation_sem, K_FOREVER);
  // }

  while(true)
  {
    if (master) // FT
    {
      err = transmit(tx_handle, "BEAC", 4, 0); // FAKE BEACON // t = 0
      k_sem_take(&operation_sem, K_FOREVER);

      pingPkt.ts1 = modem_time;
      pingPkt.ts2 = 0;

      err = transmit(tx_handle, &pingPkt, sizeof(Ping_t), 0); // TX 
      k_sem_take(&operation_sem, K_FOREVER);

      err = receive(rx_handle, 10 * MSEC_PER_SEC, 0); // RX
      k_sem_take(&operation_sem, K_FOREVER);
      
      LOG_ERR("LOOP COMPLETE");
      k_sleep(K_SECONDS(10));
    }
    else // PT
    {
      err = receive(rx_handle, 50 * MSEC_PER_SEC, 0); // GET FAKE BEACON
      k_sem_take(&operation_sem, K_FOREVER);

      err = receive(rx_handle, 10, 0); // RX 
      k_sem_take(&operation_sem, K_FOREVER);

      err = transmit(tx_handle, responsePing, sizeof(Ping_t), 0); // TX
      k_sem_take(&operation_sem, K_FOREVER);

      LOG_ERR("LOOP COMPLETE");
    }
  }
}
