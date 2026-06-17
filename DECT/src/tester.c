
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include "tester.h"
#include "led.h"

#include "dect_phy.h"
#include "dect_phy_events.h"

LOG_MODULE_REGISTER(test,LOG_LEVEL_INF);

// NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ = 69120 kHz. Meaning, it ticks 69120000 times per second

#define MODEM_TIME_TO_MS(m) ((m) / NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ)
#define MODEM_TIME_TO_US(m) ((m) * 1000ULL / NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ)
#define MS_TO_MODEM_TIME(ms) ((ms) * NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ)

#define GUARD_TIME_US ()

#define MCS (3)

static struct k_work_delayable work_ptr;
static const uint16_t test_device_id = 0;
static struct nrf_modem_dect_phy_hdr_type_1 test_sync_hdr = {
	.packet_length      = 1,
	.packet_length_type = 0x00, //Indicates whether the packet_length is given in subslots (0) or slots (1). 
	.header_format      = 0x0,
	.short_network_id   = (CONFIG_NETWORK_ID & 0xff),
	.transmitter_id_hi  = (test_device_id >> 8) & 0xff,
	.transmitter_id_lo  = (test_device_id & 0xff),
	.df_mcs             = MCS, // 
	.reserved           = 0,
	.transmit_power     = CONFIG_TX_POWER,
};

typedef struct TestMacHeader_s {
  uint32_t magic;
  uint64_t timestamp;
} __attribute__((packed)) TestMacHeader_t;

// dect_events_register_pdc_crc_err_handler

void capability_get_handler(const struct nrf_modem_dect_phy_capability_get_event *evt)
{
// struct nrf_modem_dect_phy_capability_get_event {
// 	enum nrf_modem_dect_phy_err err;
// 	struct nrf_modem_dect_phy_capability *capability;
// };

// struct nrf_modem_dect_phy_capability {
// 	/**
// 	 * @brief DECT NR+ version.
// 	 */
// 	uint8_t dect_version;
// 	/**
// 	 * @brief Number of elements in @ref variant.
// 	 */
// 	uint8_t variant_count;
// 	/**
// 	 * @brief Capability variants.
// 	 */
// 	struct {
// 		/**
// 		 * @brief Supported reception capability of spatial stream transmissions.
// 		 */
// 		uint8_t rx_spatial_streams;
// 		/**
// 		 * @brief Reception capability of TX diversity transmission.
// 		 */
// 		uint8_t rx_tx_diversity;
// 		/**
// 		 * @brief Maximum supported modulation and coding scheme for reception.
// 		 */
// 		uint8_t mcs_max;
// 		/**
// 		 * @brief HARQ soft buffer size.
// 		 */
// 		uint32_t harq_soft_buf_size;
// 		/**
// 		 * @brief Maximum number of HARQ processes.
// 		 */
// 		uint8_t harq_process_count_max;
// 		/**
// 		 * @brief HARQ feedback delay, in subslots.
// 		 */
// 		uint8_t harq_feedback_delay;
// 		/**
// 		 * @brief Subcarrier scaling factor.
// 		 */
// 		uint8_t mu;
// 		/**
// 		 * @brief Fourier transform scaling factor.
// 		 */
// 		uint8_t beta;
// 	} variant[];
// };

  LOG_INF("CAPABILITY GET CALLBACK. ERR %d VARIANTS %d, U %d", evt->err, evt->capability->variant_count, evt->capability->variant[0].mu);

}

void slave_crc_err(const struct nrf_modem_dect_phy_pdc_crc_failure_event *evt)
{
  LOG_ERR("CRC ERROR!");
}

void slave_pdc(const struct nrf_modem_dect_phy_pdc_event *evt)
{	
  LOG_INF("PDC event received: len=%d", evt->len);
	const uint8_t *data = (const uint8_t *)evt->data;
  TestMacHeader_t *testmac = (TestMacHeader_t *) data;
  nrf_modem_dect_phy_time_get();
  int64_t diff = (int64_t) modem_time - (int64_t) testmac->timestamp;
  uint64_t absdiff = (modem_time > testmac->timestamp) ? modem_time - testmac->timestamp : testmac->timestamp - modem_time;

  LOG_INF("MAGIC 0x%x pkt modem time %llu my modem time %lld diff %lld (%lld)", testmac->magic, testmac->timestamp, modem_time, diff, MODEM_TIME_TO_MS(absdiff));
	// LOG_HEXDUMP_INF(data, evt->len, "PDC EVENT");
  // serial_write(data, evt->len);
}

void slave_pcc(const struct nrf_modem_dect_phy_pcc_event *evt)
{	
  static bool on = false;
  on = !on;
  led_set(SLAVE_LED_ID, on);
  LOG_INF("PCC event received");
	// const uint8_t *data = (const uint8_t *)evt->data;		slave_frame_start = evt->stf_start_time;
  LOG_INF("slave_frame_start=%llu", evt->stf_start_time);
}

static void slave_work_handler(struct k_work *work)
{
  struct k_work_delayable *dwork = k_work_delayable_from_work(work);
  while(1)
  {
    int ret = dect_phy_rx(RX_HANDLE, NRF_MODEM_DECT_PHY_RX_MODE_SINGLE_SHOT, 0, US_TO_MODEM_TICKS(10000000));
    k_sem_take(&tdma_sem, K_FOREVER);
    LOG_INF("%s ret %d", "dect_phy_rx", ret);
  }
  // k_work_schedule(dwork, K_MSEC(100));
}

static void master_work_handler(struct k_work *work)
{
  struct k_work_delayable *dwork = k_work_delayable_from_work(work);
  // struct work_context *ctx = CONTAINER_OF(dwork, struct work_context, timed_work);
  static bool on = false;

  on = !on;
  led_set(MASTER_LED_ID, on);

  int op_count = 0;
  
  nrf_modem_dect_phy_time_get();
  LOG_INF("schedule_master: modem_time=%llu", modem_time);

  TestMacHeader_t testMacHeader = {.magic = 0x31316969, .timestamp=modem_time};
  dect_phy_tx(TX_HANDLE, &test_sync_hdr, sizeof(TestMacHeader_t), &testMacHeader, 0);

  k_work_schedule(dwork, K_MSEC(1000));
}

void testloop(Role_e role)
{
  LOG_INF("ENTERING TESTLOOP AS %s", (role == MASTER) ? "MASTER" : "SLAVE");

  dect_events_register_capability_get_handler(capability_get_handler);
  nrf_modem_dect_phy_capability_get();

	dect_phy_init();
  
  if (role == MASTER)
  {
    k_work_init_delayable(&work_ptr, master_work_handler);
    k_work_schedule(&work_ptr, K_NO_WAIT);
    LOG_INF("ENTERING TESTLOOP AS MASTER");
  }
  else if (role == SLAVE || role == SINK)
  {
    k_work_init_delayable(&work_ptr, slave_work_handler);
    k_work_schedule(&work_ptr, K_NO_WAIT);
    dect_events_register_pdc_handler(slave_pdc);
    dect_events_register_pcc_handler(slave_pcc);
    dect_events_register_pdc_crc_err_handler(slave_crc_err);
    LOG_INF("ENTERING TESTLOOP AS SLAVE/SINK");
  }

  LOG_INF("TESTLOOP INIT DONE");

  while(1)
  {
    k_sleep(K_MSEC(1000));
  }
}
