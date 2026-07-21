
#include <zephyr/kernel.h>
#include <string.h>
#include <stdbool.h>
#include <nrf_modem_dect_phy.h>
#include <nrf_modem_dect_clock_sync.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/gpio.h>
#include "phy_main.h"

LOG_MODULE_REGISTER(dect_phy, LOG_LEVEL_ERR);

#define CONFIG_CARRIER (1677) // from overlay-eu.conf

extern struct k_queue ethTxQueue;
extern struct k_queue ethRxQueue;

extern const struct gpio_dt_spec tp23Switch; // todo put these elsewhere
extern const struct gpio_dt_spec tp24Switch;
extern const struct gpio_dt_spec tp25Switch;
extern const struct gpio_dt_spec tp26Switch;
extern const struct gpio_dt_spec tp27Switch;
extern const struct gpio_dt_spec tp03Switch;
extern const struct gpio_dt_spec tp04Switch;

static const struct gpio_dt_spec *beaconTxSwitch = &tp23Switch;
static const struct gpio_dt_spec *dlSwitch = &tp24Switch;
static const struct gpio_dt_spec *ulSwitch = &tp25Switch;
static const struct gpio_dt_spec *pdcSwitch = &tp03Switch;
static const struct gpio_dt_spec *slotSwitch = &tp04Switch;

#define US_TO_MODEM_TICKS(us) (((uint64_t)(us)/1000)*NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ)
#define MODEM_TICKS_TO_MS(t) (t / NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ)

#define DECT_FRAME_DURATION_MS (10)
#define DECT_FRAME_DURATION_US (10000)
#define DECT_SLOTS_PER_FRAME (24)
#define DECT_SLOT_DURATION_US (417) // 416.67
#define DECT_SLOT_DURATION_TICK ((uint64_t)(2 * DECT_SLOT_DURATION_US * NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ / 1000))
#define DECT_GAP_US (100) // ?
#define DECT_GAP_TICK (DECT_GAP_US * NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ / 1000)

inline uint64_t us_to_modem_ticks(uint64_t us)
{
  return (((uint64_t) us / 1000) * NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ);
}

inline bool is_rx_handle(uint32_t h)
{
  return ((h % 2) != 0);
}

static void set_all_tps(int set)
{
  gpio_pin_set_dt(&tp23Switch, set);
  gpio_pin_set_dt(&tp24Switch, set);
  gpio_pin_set_dt(&tp25Switch, set);
  gpio_pin_set_dt(&tp26Switch, set);
  gpio_pin_set_dt(&tp27Switch, set);
  gpio_pin_set_dt(&tp03Switch, set);
  // gpio_pin_set_dt(&tp04Switch, set);
}

typedef struct Ping_s
{
  char text[4]; // text that says TEST
  uint16_t counter; 
  uint64_t ts1; // master send ts
  uint64_t ts2; // slave receive ts
} __attribute__((packed)) Ping_t; // 20

static size_t len; // test ping
static char pingBuf[128]; // test ping buffer
static Ping_t *responsePing = (Ping_t *) pingBuf; // TODO remove all of these eventually

static bool exit;
static uint16_t device_id;
static volatile uint64_t modem_time;

static uint64_t lastPccTs = 0;
static uint64_t lastPdcTs = 0;
static uint64_t lastLoopTs = 0;

const uint32_t beacon_tx_handle = 0;
const uint32_t beacon_rx_handle = 1;
const uint32_t ft_tx_handle = 2;
const uint32_t ft_rx_handle = 3;
const uint32_t pt_tx_handle = 4;
const uint32_t pt_rx_handle = 5;

static bool iAmMaster;

static volatile bool gotBeacon = false;
static volatile bool gotData = false;

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

K_SEM_DEFINE(operation_sem, 0, 3); // 3 operations per frame beacon uplink downlink
K_SEM_DEFINE(cancel_sem, 0, 1);
K_SEM_DEFINE(beacon_sem, 0, 1);
K_SEM_DEFINE(rx_done_sem, 0, 1);
K_SEM_DEFINE(tx_done_sem, 0, 1);
K_SEM_DEFINE(time_sem, 0, 1);

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
  if (evt->err)
  {
    LOG_ERR("capability_get cb time %"PRIu64" status %x", modem_time, evt->err);
  }
  else 
  {
    LOG_WRN("capability_get cb time %"PRIu64" status %x", modem_time, evt->err);
  }
}

static void on_op_complete(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
  if (evt->err)
  {
    LOG_ERR("op_complete %s cb time %"PRIu64" status %x handle %d", !is_rx_handle(evt->handle) ? "TX" : "RX", modem_time, evt->err, evt->handle);
  }

  if (!is_rx_handle(evt->handle))
  {
    k_sem_give(&tx_done_sem);
  }

  switch(evt->handle) // TODO test poijnts remove eventually
  {
    case beacon_tx_handle:
      gpio_pin_set_dt(beaconTxSwitch, 0);
      break;
    case ft_tx_handle:
      gpio_pin_set_dt(dlSwitch, 0);
      break;
    case ft_rx_handle:
      gpio_pin_set_dt(ulSwitch, 0);
      break;
    default:
      break;
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

// TODO more things to check here im sure but currently iut only checksi f the first bytes of the packet reads BEAC
static bool check_beacon(char *pkt)
{
  if (strncmp(pkt, "BEAC", 4) == 0)
  {
    return true;
  }
  return false;
}

static void on_pdc(const struct nrf_modem_dect_phy_pdc_event *evt)
{
	/* Received RSSI value is in fixed precision format Q14.1 */
	// LOG_WRN("PDC Received data %d bytes (RSSI: %d.%d): %s", evt->len, (evt->rssi_2 / 2), (evt->rssi_2 & 0b1) * 5, (char *)evt->data);

  lastPdcTs = modem_time;

  if (!iAmMaster) // If we are the PT we return the ping
  {
    if (!gotBeacon && check_beacon((char *) evt->data))
    {
      gotBeacon = true;
      k_sem_give(&beacon_sem); // todo not needed
    }
    else if (!gotData)
    {
      gotData = true;
      // LOG_INF("GOT DATA %s", evt->data);
      memcpy(pingBuf, evt->data, sizeof(Ping_t));
      // responsePing->ts1 = 31;
      responsePing->ts2 = 31;
      strncpy(responsePing->text, "QWER", 4);
    }
  }

  if (iAmMaster) // If we are the FT then we print the ping
  {
    LOG_INF("GOT DATA %s", evt->data);
    memcpy(pingBuf, evt->data, sizeof(Ping_t));
    // LOG_WRN("Ping response received. ts1: %llu, ts2: %llu, ts3: %llu, data: %s, cnt: %d", responsePing->ts1, responsePing->ts2, modem_time, responsePing->text, responsePing->counter);
    uint64_t diff = modem_time - responsePing->ts1;
    // LOG_WRN("Diff %llu ticks,  %llu ms", diff, diff / NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ);
    gpio_pin_set_dt(pdcSwitch, 1);
  }

  k_sem_give(&rx_done_sem);
}

static void on_pdc_crc_err(const struct nrf_modem_dect_phy_pdc_crc_failure_event *evt)
{
	LOG_DBG("pdc_crc_err cb time %"PRIu64"", modem_time);
}

static void on_latency_info_get(const struct nrf_modem_dect_phy_latency_info_event *evt)
{
	LOG_DBG("latency_info_get cb status %x", evt->err);
}

static void on_cancel(const struct nrf_modem_dect_phy_cancel_event *evt)
{
  k_sem_give(&cancel_sem);
}

static void on_time_get(const struct nrf_modem_dect_phy_time_get_event *evt)
{
	LOG_DBG("time_get cb time %"PRIu64" status %x", modem_time, evt->err);
  k_sem_give(&time_sem);
}

static void dect_phy_event_handler(const struct nrf_modem_dect_phy_event *evt)
{
  modem_time = evt->time;
  // LOG_DBG("%s modem_time %llu Event %d", __FUNCTION__, evt->time, evt->id);
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
		on_cancel(&evt->cancel);
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

static int transmit(uint32_t handle, void *data, size_t data_len, uint64_t start_time)
{
  int err;

  struct phy_ctrl_field_common header = {
    .header_format = 0x0,
    .packet_length_type = 0x1,
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
    .lbt_period = 0,// NRF_MODEM_DECT_LBT_PERIOD_MAX, // JON EXPERIMENTAL
    .phy_header = (union nrf_modem_dect_phy_hdr *) &header,
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

static int receive(uint32_t handle, uint32_t durationTicks, uint64_t start_time)
{
  int err;

	struct nrf_modem_dect_phy_rx_params rx_op_params = {
		.start_time = start_time,
		.handle = handle,
		.network_id = CONFIG_APP_NETWORK_ID,
		.mode = NRF_MODEM_DECT_PHY_RX_MODE_SINGLE_SHOT, //NRF_MODEM_DECT_PHY_RX_MODE_CONTINUOUS,
		.rssi_interval = NRF_MODEM_DECT_PHY_RSSI_INTERVAL_OFF,
		.link_id = NRF_MODEM_DECT_PHY_LINK_UNSPECIFIED,
		.rssi_level = -60,
		.carrier = CONFIG_CARRIER,
		.duration = durationTicks,
		.filter.short_network_id = CONFIG_APP_NETWORK_ID & 0xff,
		.filter.is_short_network_id_used = 1,
		/* listen for everything (broadcast mode used) */
		.filter.receiver_identity = 0,
	};

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
}

static void test_point_thread(void)
{
  while(gpio_pin_get_dt(slotSwitch) == GPIO_OUTPUT_INACTIVE){}
  while(true)
  {
    gpio_pin_toggle_dt(slotSwitch);
    k_sleep(K_USEC(DECT_SLOT_DURATION_US));
  }
}
K_KERNEL_STACK_MEMBER(testPointThreadStack, 256); // JON pound define
static struct k_thread testPointThreadHandle;

void DectPhy_Main(bool master)
{	
  set_all_tps(0);

	//  k_thread_create(&testPointThreadHandle, testPointThreadStack, 
	// 		256,
	// 		test_point_thread,
	// 		NULL, NULL, NULL,
	// 		K_PRIO_COOP(2),
	// 		0, K_NO_WAIT);
	// k_thread_name_set(&testPointThreadHandle, "test_point_thread");

  DectPhy_Init();
  int err;
  iAmMaster = master;
  
  Ping_t pingPkt;
  strncpy(&pingPkt.text, "TEST", 4);

  nrf_modem_dect_phy_time_get(); 
  k_sem_take(&time_sem, K_FOREVER);

  uint16_t cnt = 0;

  gpio_pin_set_dt(slotSwitch, 1);

  while(true)
  {
    if (master) // FT
    {
      LOG_DBG("LOOP %d BEGINNING", cnt);
      set_all_tps(0);

      const uint64_t start_lead = (4ULL * DECT_SLOT_DURATION_TICK);

      uint64_t base = modem_time + start_lead;
      
      uint64_t beacon_tx_start_time = base;
      uint64_t ft_tx_start_time = base + (3 * DECT_SLOT_DURATION_TICK);
      uint64_t ft_rx_start_time = base + (5 * DECT_SLOT_DURATION_TICK);

      pingPkt.ts1 = base;
      pingPkt.ts2 = 0;
      pingPkt.counter = cnt;

      lastLoopTs = base;

      gpio_pin_set_dt(beaconTxSwitch, 1);
      err = transmit(beacon_tx_handle, "BEAC", 4, beacon_tx_start_time); // FAKE BEACON // t = 0
      
      gpio_pin_set_dt(dlSwitch, 1);
      err = transmit(ft_tx_handle, &pingPkt, sizeof(Ping_t) , ft_tx_start_time);

      gpio_pin_set_dt(ulSwitch, 1);
      err = receive(ft_rx_handle, 4 * DECT_SLOT_DURATION_TICK, ft_rx_start_time);

      for (int i = 0; i < 3; i++) // wait for all 3 operations
      {
        k_sem_take(&operation_sem, K_FOREVER);
      }
      
      LOG_DBG("LOOP %d COMPLETE. DIFF %llu", cnt, modem_time - base);
      cnt++;

      nrf_modem_dect_phy_time_get(); 
      k_sem_take(&time_sem, K_FOREVER);
    }
    else // PT
    {
      gotBeacon = false;
      gotData = false;
      uint64_t base = 0;

      err = receive(beacon_rx_handle, 24 * DECT_SLOT_DURATION_TICK, 0); // GET FAKE BEACON
      k_sem_take(&operation_sem, K_FOREVER);
      if (!gotBeacon)
      {
        // LOG_DBG("BEACON FAILED");
        continue;
      }

      err = receive(pt_rx_handle, 3 * DECT_SLOT_DURATION_TICK, lastPccTs + (2 * DECT_SLOT_DURATION_TICK));
      err = transmit(pt_tx_handle, responsePing, sizeof(Ping_t), lastPccTs + (5 * DECT_SLOT_DURATION_TICK) + 1);

      for (int i = 0; i < 2; i++)
      {
        k_sem_take(&operation_sem, K_MSEC(10000));
      }

      LOG_DBG("LOOP COMPLETE %s %s", (gotBeacon) ? "BEACON" : "", (gotData) ? "DATA" : "");
    }
  }
}

