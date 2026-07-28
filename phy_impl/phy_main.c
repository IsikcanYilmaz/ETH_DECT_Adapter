#include <zephyr/kernel.h>
#include <string.h>
#include <stdbool.h>
#include <nrf_modem_dect_phy.h>
#include <nrf_modem_dect_clock_sync.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/gpio.h>
#include "lean_wiznet_driver.h"
#include "phy_main.h"

LOG_MODULE_REGISTER(dect_phy, LOG_LEVEL_WRN);

#define CONFIG_CARRIER (1677) // from overlay-eu.conf

extern struct k_queue ethTxQueue;
extern struct k_queue ethRxQueue;

K_QUEUE_DEFINE(inFlightQueue); // on transmission op completes, free these pointers

extern const struct gpio_dt_spec tp23Switch; // todo put these elsewhere
extern const struct gpio_dt_spec tp24Switch;
extern const struct gpio_dt_spec tp25Switch;
extern const struct gpio_dt_spec tp26Switch;
extern const struct gpio_dt_spec tp27Switch;
// extern const struct gpio_dt_spec tp03Switch;
// extern const struct gpio_dt_spec tp04Switch;

static const struct gpio_dt_spec *beaconTxSwitch = &tp23Switch;
static const struct gpio_dt_spec *beaconRxSwitch = &tp23Switch;
static const struct gpio_dt_spec *dlSwitch = &tp24Switch;
static const struct gpio_dt_spec *ulSwitch = &tp25Switch;
// static const struct gpio_dt_spec *pdcSwitch = &tp03Switch;
// static const struct gpio_dt_spec *slotSwitch = &tp04Switch;

static const enum nrf_modem_dect_phy_radio_mode radioMode = NRF_MODEM_DECT_PHY_RADIO_MODE_LOW_LATENCY;

static struct nrf_modem_dect_phy_latency_info latencyInfo;
static uint32_t opTransitionLatency; 
static uint32_t opStartupLatency;
static uint32_t tx_idleToActiveLatency;
static uint32_t tx_activeToIdleLatency;
static uint32_t rx_idleToActiveLatency;

static volatile enum DectPtStateMachine_e PtState = PT_STATE_WAIT_FOR_BEACON;

#define US_TO_MODEM_TICKS(us) (((uint64_t)(us)/1000)*NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ)
#define MODEM_TICKS_TO_MS(t) (t / NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ)

#define DECT_FRAME_DURATION_MS (10)
#define DECT_FRAME_DURATION_US (10000)
#define DECT_SLOTS_PER_FRAME (24)
#define DECT_SLOT_DURATION_US (417) // 416.67
#define DECT_SLOT_DURATION_TICK ((uint64_t)((DECT_SLOT_DURATION_US * NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ) / 1000))
#define DECT_GAP_US (100) // ?
#define DECT_GAP_TICK ((uint64_t) (DECT_GAP_US * NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ) / 1000)

#define DECT_OPS_PER_BEACON 108

#define DECT_MASTER_BEACON_PERIOD_TICK (20 * 24 * DECT_SLOT_DURATION_TICK)

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
  // gpio_pin_set_dt(&tp03Switch, set);
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
static bool warmUp = false;
static uint16_t device_id;
static volatile uint64_t modem_time;

static uint32_t slotCounter = 0;
static uint32_t frameCounter = 0;

static uint64_t lastPccModemTick = 0;
static uint64_t lastPdcModemTick = 0;
static uint64_t lastBeaconModemTick = 0;
static uint64_t lastLoopModemTick = 0;

static uint32_t lastBeaconTs = 0;

const uint32_t beacon_tx_handle = 0;
const uint32_t beacon_rx_handle = 1;
const uint32_t ft_tx_handle = 2000;
const uint32_t ft_rx_handle = 3000;
const uint32_t pt_tx_handle = 4000;
const uint32_t pt_rx_handle = 5000;

const uint32_t test_tx_handle = 900;
const uint32_t test_rx_handle = 1000;

static const struct { int dummy; } queue_null_marker;

static volatile bool iAmMaster;

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

K_SEM_DEFINE(operation_sem, 0, 1); // 3 operations per frame beacon uplink downlink
K_SEM_DEFINE(cancel_sem, 0, 1);
K_SEM_DEFINE(beacon_sem, 0, 1);
K_SEM_DEFINE(rx_done_sem, 0, 1);
K_SEM_DEFINE(tx_done_sem, 0, 1);
K_SEM_DEFINE(time_sem, 0, 1);

// TODO more things to check here im sure but currently iut only checksi f the first bytes of the packet reads BEAC
static bool check_beacon(char *pkt)
{
  if (strncmp(pkt, "BEAC", 4) == 0)
  {
    return true;
  }
  return false;
}

static bool check_none_pkt(char *pkt) 
{
  if (strncmp(pkt, "NONE", 4) == 0)
  {
    return true;
  }
  return false;
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
	if (err != 0) 
  {
		return err;
	}

	return 0;
}

static int transmit_head_of_queue(uint32_t handle, uint64_t start_time)
{
  int err;
  struct DectInFlightPktStub_s *inFlight = k_malloc(sizeof(struct DectInFlightPktStub_s));
  inFlight->handle = handle;
  if (!k_queue_is_empty(&ethRxQueue))
  {
    struct LeanWiznet_Packet *pkt = (struct LeanWiznet_Packet *) k_queue_get(&ethRxQueue, K_FOREVER);
    LOG_DBG("%d BYTES READ FROM ETH, SCHEDULED FOR TX AT %llu", pkt->size, start_time);
    err = transmit(handle, pkt->payload, pkt->size, start_time);
    inFlight->ptr = (void *) pkt;
  }
  else
  {
    LOG_DBG("NO PKT FROM ETH. SENDING BLANK TX");
    err = transmit(handle, "NONE", 4, start_time);
    inFlight->ptr = NULL;
  }
  k_queue_append(&inFlightQueue, inFlight);
  return err;
}

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
	k_sem_give(&operation_sem);
}

static void on_pcc(const struct nrf_modem_dect_phy_pcc_event *evt)
{
	LOG_INF("PCC Received header from device ID %d", evt->hdr.hdr_type_1.transmitter_id_hi << 8 | evt->hdr.hdr_type_1.transmitter_id_lo);
  lastPccModemTick = modem_time;
}

static void on_pcc_crc_err(const struct nrf_modem_dect_phy_pcc_crc_failure_event *evt)
{
	LOG_DBG("pcc_crc_err cb time %"PRIu64"", modem_time);
}

static void on_op_complete_pt(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
  // LOG_WRN("%s", __FUNCTION__);
  
  if (evt->handle == pt_tx_handle) // Previous Uplink slot ended. schedule next downlink slot
  {
    int err = receive(pt_rx_handle + slotCounter, 100 * DECT_SLOT_DURATION_TICK + 2 * opTransitionLatency, 0); // schedule next rx slot
    slotCounter++;
    PtState = (slotCounter < DECT_OPS_PER_BEACON) ? PT_STATE_SCHEDULED_DOWNLINK : PT_STATE_WAIT_FOR_BEACON;
    gpio_pin_toggle_dt(ulSwitch);

    // Tx just completed. Free the pointer of what just got tx'd. 
    if (k_queue_is_empty(&inFlightQueue))
    {
      LOG_ERR("TX COMPLETE BUT IN FLIGHT QUEUE EMPTY!!!");
    }
    else
    {
      struct DectInFlightPktStub_s *pktToFree = k_queue_get(&inFlightQueue, K_FOREVER);
      if (pktToFree)
      {
        if (pktToFree->ptr)
          LOG_DBG("IN FLIGHT PKT FROM HANDLE %d DONE. 0x%08x. FREEING", pktToFree->handle, pktToFree->ptr);
        k_free(pktToFree->ptr);
        k_free(pktToFree);
      }
    }
  }
  k_sem_give(&operation_sem);
}

static void on_pdc_pt(const struct nrf_modem_dect_phy_pdc_event *evt) // TODO make this part as lean as possible. just copy over the bytes and let a thread do processing
{
  int err;
  switch(PtState)
  {
    case PT_STATE_WAIT_FOR_BEACON: 
      {
        // We received data while waiting for the beacon. Check if it is a beacon. If so, kick off the current frame's states
        if (check_beacon((char *) evt->data))
        {
          err = receive(pt_rx_handle + slotCounter, 100 * DECT_SLOT_DURATION_TICK + 2 * opTransitionLatency, 0);
          if (err)
          {
            LOG_ERR("%s ERROR SCHEDULING RECEIVE", err);
            break;
          }
          lastBeaconModemTick = modem_time;
          lastBeaconTs = k_uptime_get();
          gpio_pin_toggle_dt(beaconRxSwitch);
          slotCounter = 1;
          PtState = PT_STATE_SCHEDULED_DOWNLINK;
          LOG_DBG("BEACON RECEIVED. STATE -> SCHEDULED DOWNLINK");
        }
        else
        {
          LOG_DBG("NON BEACON DURING PT WAITING FOR BEACON");
        }
        break;
      }
    case PT_STATE_SCHEDULED_DOWNLINK:
      {
        err = transmit_head_of_queue(pt_tx_handle, modem_time + 2 * opTransitionLatency);
        if (!check_none_pkt(evt->data))
        {
          LOG_DBG("PT RECEIVED %d BYTES FROM FT IN SLOT %d", evt->len, slotCounter);
          LOG_HEXDUMP_DBG(evt->data, evt->len, "RX");

          // We got a packet from the DECT connection. Enqueue it to wiznet's tx queue // TODO reduce code dupes
          struct LeanWiznet_Packet *pkt = k_malloc(sizeof(struct LeanWiznet_Packet) + evt->len);
          memcpy(pkt->payload, evt->data, evt->len);
          pkt->size = evt->len;
          k_queue_append(&ethTxQueue, pkt);
        }
        // err = transmit(pt_tx_handle, "TEST", 4, 0);
        slotCounter++;
        gpio_pin_toggle_dt(dlSwitch);
        PtState = PT_STATE_SCHEDULED_UPLINK;
        break;
      }
    case PT_STATE_SCHEDULED_UPLINK:
      {
        // err = receive(pt_rx_handle + slotCounter, 4 * DECT_SLOT_DURATION_TICK, 0);
        // slotCounter++;
        // PtState = (slotCounter < 6) ? PT_STATE_SCHEDULED_DOWNLINK : PT_STATE_WAIT_FOR_BEACON;
        // gpio_pin_toggle_dt(ulSwitch);
        break;
      }
    case PT_STATE_FRAME_DONE:
      {
        break;
      }
    default:
    break;
  }

  k_sem_give(&rx_done_sem);
}

static void on_op_complete_ft(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
  int err;
  slotCounter++;

  if (evt->handle == beacon_tx_handle || evt->handle == beacon_tx_handle + 1 || evt->handle == beacon_tx_handle + 2 || evt->handle == beacon_tx_handle + 3) // BEACON TX GOT DONE
  {
    if (evt->err == 0)
    {
      err = transmit(beacon_tx_handle, "BEAC", 4, modem_time + DECT_MASTER_BEACON_PERIOD_TICK); // Next beacon
      // err = transmit(ft_tx_handle, "TEST", 4, modem_time + (1) * (2 * opTransitionLatency));
      err = transmit_head_of_queue(ft_tx_handle, modem_time + (1) * (2 * opTransitionLatency));
      slotCounter = 0;
      gpio_pin_toggle_dt(beaconTxSwitch);
      if (err)
      {
        LOG_ERR("Error scheduling tx %d", err);
      }
    }
  }
  else if (evt->handle >= ft_tx_handle && evt->handle < ft_rx_handle) // TX GOT DONE. SCHEDULE RX
  {
    if (evt->err == 0)
    {
      gpio_pin_toggle_dt(dlSwitch);
      if (slotCounter < DECT_OPS_PER_BEACON)
      {
        err = receive(ft_rx_handle + slotCounter, 2 * DECT_SLOT_DURATION_TICK + (2 * opTransitionLatency), modem_time + (2 * opTransitionLatency));
        if (err)
        {
          LOG_ERR("Error scheduling rx %d", err);
        }
      }
    }

    // Tx just completed. Free the pointer of what just got tx'd. 
    if (k_queue_is_empty(&inFlightQueue))
    {
      LOG_ERR("TX COMPLETE BUT IN FLIGHT QUEUE EMPTY!!!");
    }
    else
    {
      struct DectInFlightPktStub_s *pktToFree = k_queue_get(&inFlightQueue, K_FOREVER);
      if (pktToFree)
      {
        if (pktToFree->ptr)
          LOG_DBG("IN FLIGHT PKT FROM HANDLE %d DONE. 0x%08x. FREEING", pktToFree->handle, pktToFree->ptr);
        k_free(pktToFree->ptr);
        k_free(pktToFree);
      }
    }

  }
  else if (evt->handle >= ft_rx_handle && evt->handle < pt_tx_handle) // RX GOT DONE. SCHEDULE TX
  {
    if (evt->err == 0)
    {
      gpio_pin_toggle_dt(ulSwitch);
      if (slotCounter < DECT_OPS_PER_BEACON)
      {
        // err = transmit(ft_tx_handle + slotCounter, "TEST", 4, modem_time + (1) * (2 * opTransitionLatency));
        err = transmit_head_of_queue(ft_tx_handle + slotCounter, modem_time + (1) * (2 * opTransitionLatency));
        if (err)
        {
          LOG_ERR("Error scheduling tx %d", err);
        }
      }
    }
  }

  if (evt->err)
  {
    LOG_ERR("op_complete %s cb time %"PRIu64" status %x handle %d slotCounter %d", !is_rx_handle(evt->handle) ? "TX" : "RX", modem_time, evt->err, evt->handle, slotCounter);
  }

  k_sem_give(&operation_sem);
}

static void on_pdc_ft(const struct nrf_modem_dect_phy_pdc_event *evt) // TODO make this part as lean as possible. just copy over the bytes and let a thread do processing
{
  // LOG_HEXDUMP_WRN(evt->data, evt->len, "RX");
  LOG_DBG("%s", __FUNCTION__);
  if (!check_none_pkt(evt->data))
  {
    LOG_DBG("%s %d handle, %d bytes", __FUNCTION__, evt->handle, evt->len);
    LOG_HEXDUMP_DBG(evt->data, evt->len, "RX");
    
    // We got a packet from the DECT connection. Enqueue it to wiznet's tx queue
    struct LeanWiznet_Packet *pkt = k_malloc(sizeof(struct LeanWiznet_Packet) + evt->len);
    memcpy(pkt->payload, evt->data, evt->len);
    pkt->size = evt->len;
    k_queue_append(&ethTxQueue, pkt);
  }
  k_sem_give(&rx_done_sem);
}

static void on_pdc_crc_err(const struct nrf_modem_dect_phy_pdc_crc_failure_event *evt)
{
	LOG_DBG("pdc_crc_err cb time %"PRIu64"", modem_time);
}

static void on_latency_info_get(const struct nrf_modem_dect_phy_latency_info_event *evt)
{
	LOG_WRN("latency_info_get cb status %x", evt->err);
  if (evt->err == 0)
  {
    memcpy(&latencyInfo, evt->latency_info, sizeof(struct nrf_modem_dect_phy_latency_info));
    opTransitionLatency = latencyInfo.radio_mode[radioMode].scheduled_operation_transition;
    opStartupLatency = latencyInfo.radio_mode[radioMode].scheduled_operation_startup;
    tx_idleToActiveLatency = latencyInfo.operation.transmit.idle_to_active;
    tx_activeToIdleLatency = latencyInfo.operation.transmit.active_to_idle;
    rx_idleToActiveLatency = latencyInfo.operation.receive.idle_to_active;
    LOG_WRN("Latency info: \n\
            slot_ticks:              %llu\n\
            gap_ticks:               %llu\n\
            scheduled_op_transition: %d\n\
            op_startup:              %d\n\
            tx_idleToActiveLatency:  %d\n\
            tx_activeToIdleLatency:  %d\n\
            rx_idleToActiveLatency:  %d\n\
            current modem_time:      %llu\n\
            current uptime ticks:    %llu\n\
            modem ticks per ms:      %llu\n\
            host ticks per ms:       %llu\n", 
            DECT_SLOT_DURATION_TICK, 
            DECT_GAP_TICK, 
            opTransitionLatency, opStartupLatency, tx_idleToActiveLatency, tx_activeToIdleLatency, rx_idleToActiveLatency,
            modem_time, k_uptime_ticks(), (uint64_t)(NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ), (uint64_t) (CONFIG_SYS_CLOCK_TICKS_PER_SEC / 1000));
    
  }
  k_sem_give(&operation_sem);
}

static void on_cancel(const struct nrf_modem_dect_phy_cancel_event *evt)
{
  k_sem_give(&cancel_sem);
}

static void on_time_get(const struct nrf_modem_dect_phy_time_get_event *evt)
{
	LOG_DBG("time_get cb time %"PRIu64" status %x", modem_time, evt->err);
  if (!warmUp && iAmMaster)
  {
    int err;
    uint64_t base = modem_time + 50 * DECT_SLOT_DURATION_TICK;
    uint64_t downlinkScheduleTick = base + (3 * DECT_SLOT_DURATION_TICK) + opTransitionLatency; 
    
    LOG_WRN("Modem time %llu, Base %llu, beacon tx at %llu, DL at %llu", modem_time, base, base, base + (2 * DECT_SLOT_DURATION_TICK));

    err = transmit(beacon_tx_handle, "BEAC", 4, base);

    if (err)
    {
      LOG_ERR("%s ERR %x", __FUNCTION__, err);
    }
  }
  warmUp = true;
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
    if (iAmMaster)
    {
      on_op_complete_ft(&evt->op_complete);
    }
    else
    {
      on_op_complete_pt(&evt->op_complete);
    }
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
    lastPdcModemTick = modem_time;
    if (iAmMaster)
    {
		  on_pdc_ft(&evt->pdc);
    }
    else 
    {
		  on_pdc_pt(&evt->pdc);
    }
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

  nrf_modem_dect_phy_latency_get();
  k_sem_take(&operation_sem, K_FOREVER);

  nrf_modem_dect_phy_capability_get();
  k_sem_take(&operation_sem, K_FOREVER);

  ////////////////////////////////////// on_configure will release the semaphore
  
	err = nrf_modem_dect_phy_activate(radioMode);
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
}

static void test_point_thread(void)
{
  // while(gpio_pin_get_dt(slotSwitch) == GPIO_OUTPUT_INACTIVE){}
  while(true)
  {
    // gpio_pin_toggle_dt(slotSwitch);
    k_sleep(K_USEC(DECT_SLOT_DURATION_US));
  }
}
K_KERNEL_STACK_MEMBER(testPointThreadStack, 256); // JON pound define
static struct k_thread testPointThreadHandle;

bool DectPhy_WiznetAlert(void) // TODO better way of doing this
{
  return true;
}

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
  
  set_all_tps(0);

  // LOG_WRN("ATTACH DEBUGGER NOW IF YOU WANT");
  // k_sleep(K_MSEC(5000));

  nrf_modem_dect_phy_time_get(); 

  uint16_t cnt = 0;

  while(true)
  {
    if (iAmMaster) // FT
    {
      LOG_DBG("LOOP %d BEGINNING", cnt);
      k_sleep(K_MSEC(1000));
      LOG_DBG("LOOP %d COMPLETE", cnt);
      cnt++;
    }
    else // PT
    {
      if (!warmUp)
      {
        k_sem_take(&time_sem, K_FOREVER);
      }

      LOG_DBG("PT LOOP BEGIN");

      err = receive(beacon_rx_handle, US_TO_MODEM_TICKS(1000000), 0); // GET FAKE BEACON
      k_sem_take(&operation_sem, K_FOREVER);
      
      if (PtState == PT_STATE_WAIT_FOR_BEACON)
      {
        // LOG_WRN("PT BEACON NOT RECEIVED");
        continue;
      }

      LOG_DBG("PT BEACON RECEIVED");

      while (PtState > PT_STATE_WAIT_FOR_BEACON && k_uptime_get() - lastBeaconTs < MODEM_TICKS_TO_MS(DECT_MASTER_BEACON_PERIOD_TICK))
      {
        k_sleep(K_USEC(1));
      }

      LOG_DBG("PT UNLATCHED");
    }
  }
}

