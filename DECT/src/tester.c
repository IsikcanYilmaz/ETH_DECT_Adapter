
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include "tester.h"
#include "led.h"

#include "dect_phy.h"
#include "dect_phy_events.h"

LOG_MODULE_REGISTER(test,LOG_LEVEL_INF);

static struct k_work_delayable work_ptr;
static const uint16_t test_device_id = 0;
static struct nrf_modem_dect_phy_hdr_type_1 test_sync_hdr = {
	.packet_length      = 1,
	.packet_length_type = 0x00, //Indicates whether the packet_length is given in subslots (0) or slots (1). 
	.header_format      = 0x0,
	.short_network_id   = (CONFIG_NETWORK_ID & 0xff),
	.transmitter_id_hi  = (test_device_id >> 8) & 0xff,
	.transmitter_id_lo  = (test_device_id & 0xff),
	.df_mcs             = 1, // 
	.reserved           = 0,
	.transmit_power     = CONFIG_TX_POWER,
};

typedef struct TestMacHeader_s {
  uint32_t magic;
  uint64_t timestamp;
} __attribute__((packed)) TestMacHeader_t;

static void slave_work_handler(struct k_work *work)
{
  struct k_work_delayable *dwork = k_work_delayable_from_work(work);
  k_work_schedule(dwork, K_MSEC(1000));
}

static void master_work_handler(struct k_work *work)
{
  struct k_work_delayable *dwork = k_work_delayable_from_work(work);
  // struct work_context *ctx = CONTAINER_OF(dwork, struct work_context, timed_work);
  static bool on = false;

  on = !on;
  led_set(SLAVE_LED_ID, on);

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

  if (role == MASTER)
  {
    k_work_init_delayable(&work_ptr, master_work_handler);
    k_work_schedule(&work_ptr, K_NO_WAIT);
  }
  else if (role == SLAVE)
  {
    k_work_init_delayable(&work_ptr, slave_work_handler);
    k_work_schedule(&work_ptr, K_NO_WAIT);
  }

  LOG_INF("TESTLOOP INIT DONE");

  while(1)
  {
    k_sleep(K_MSEC(1000));
  }
}
