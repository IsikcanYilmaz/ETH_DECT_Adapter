#include "tdma.h"
#include "dect_phy.h"
#include "dect_phy_events.h"
#include "dect_serial.h"

LOG_MODULE_REGISTER(tdma, LOG_LEVEL_INF);

uint8_t mode = UNDEFINED_ROLE;

uint8_t sync_flag = 0;

#define TDMA_SYNC_US           (100 * 1000)
#define TDMA_TX_US             (50 * 1000)
#define TDMA_BUFFER_TX_US      (125 * 1000)
#define TDMA_BUFFER_RX_US      (50 * 1000)
#define TDMA_RX_US             (100 * 1000)
#define TDMA_BUFFER_US         (200  * 1000)
#define SAFTY_MARGIN_US        (800)

#define TDMA_TOTAL_US (TDMA_SYNC_US + TDMA_BUFFER_TX_US + TDMA_BUFFER_RX_US + TDMA_RX_US + TDMA_TX_US + TDMA_BUFFER_US)

#define TDMA_MAX_STATE_COUNT (TDMA_MAX_TRANSMISSIONS + 1 + 1) /* + MASTER: Send sync + BOTH: RX-Messages */

uint64_t slave_frame_start = 0;
uint64_t slave_sync_start = 0;
K_SEM_DEFINE(slave_sem,    0, 1);
K_SEM_DEFINE(master_sem,    0, 1);
K_SEM_DEFINE(tdma_sem,    0, TDMA_MAX_STATE_COUNT);

const uint16_t device_id = 0;

struct nrf_modem_dect_phy_hdr_type_1 sync_hdr = {
	.packet_length      = TDMA_SYNC_SS,
	.packet_length_type = 0x00,
	.header_format      = 0x0,
	.short_network_id   = (CONFIG_NETWORK_ID & 0xff),
	.transmitter_id_hi  = (device_id >> 8) & 0xff,
	.transmitter_id_lo  = (device_id & 0xff),
	.df_mcs             = TDMA_SYNC_MCS,
	.reserved           = 0,
	.transmit_power     = CONFIG_TX_POWER,
};

// TODO not final positions. get shit working then make pretty
#define PAYLOAD_MAX 141 // MCS 3 with ss len 2
#define DECT_LEN_PREFIX 2

static uint8_t tx_buf[PAYLOAD_LEN_MAX * TDMA_MAX_TRANSMISSIONS];
static uint8_t serial_buf[PAYLOAD_MAX - DECT_LEN_PREFIX];

static struct nrf_modem_dect_phy_hdr_type_1 tx_hdr = {
	.packet_length      = 2,
	.packet_length_type = 0x00,
	.header_format      = 0x0,
	.short_network_id   = (CONFIG_NETWORK_ID & 0xff),
	.transmitter_id_hi  = (0 >> 8) & 0xff,
	.transmitter_id_lo  = (0 & 0xff),
	.df_mcs             = FRAME_TX_MCS,
	.reserved           = 0,
	.transmit_power     = CONFIG_TX_POWER,
};

void schedule_basic_master(void)
{
	while(1)
	{
		while(!serial_rx_available())
    {
      dect_phy_rx(RX_HANDLE, NRF_MODEM_DECT_PHY_RX_MODE_CONTINUOUS, 0, US_TO_MODEM_TICKS(TDMA_RX_US));
      k_sem_take(&tdma_sem, K_FOREVER);
			k_sleep(K_USEC(1));
		}

		int op_count = 0;

		LOG_DBG("before time get, sem count=%d", time_sem.count);
		nrf_modem_dect_phy_time_get();

		LOG_DBG("taking time sem with sem count=%d", time_sem.count);
		k_sem_take(&time_sem, K_FOREVER);

		LOG_DBG("schedule_master: scheduling TX payload");

		// char sync_buf[] = {0, 1, 9};
		// dect_phy_tx(TX_HANDLE, &sync_hdr, sizeof(sync_buf), sync_buf, 0);
		// op_count +=1;

		size_t num_ready_bytes = serial_read(serial_buf, 128);
		LOG_DBG("NUM SENDING BYTES %d", num_ready_bytes);
		LOG_DBG("NUM REMAINING BYTES %d", serial_rx_available());

		LOG_HEXDUMP_DBG(serial_buf, num_ready_bytes, "TX");

		int err = dect_phy_tx(TX_HANDLE, &tx_hdr, num_ready_bytes, serial_buf, 0);
		op_count += 1;
// int dect_phy_tx(uint32_t handle,
//                 struct nrf_modem_dect_phy_hdr_type_1 *hdr,
//                 uint32_t payload_len,
//                 uint8_t *buf,
//                 uint64_t start_time);


		// op_count += serial_to_dect(tx_start_tick, TDMA_TX_US);
		for(int i = 0; i<op_count; i++){
			LOG_DBG("taking semaphore with opcount: %d", i);
			k_sem_take(&tdma_sem, K_FOREVER);
		}
		k_sem_reset(&tdma_sem);
	}
}

void schedule_basic_sink(void)
{
  while(1)
  {
    dect_phy_rx(RX_HANDLE, NRF_MODEM_DECT_PHY_RX_MODE_CONTINUOUS, 0, US_TO_MODEM_TICKS(TDMA_RX_US));
    k_sem_take(&tdma_sem, K_FOREVER);
  }
}


/* MASTER
 */


void schedule_master(void)
{
	while(1){
		int op_count = 0;
		LOG_DBG("schedule_master: requesting modem time");
		LOG_DBG("before time get, sem count=%d", time_sem.count);
		nrf_modem_dect_phy_time_get();

		LOG_DBG("schedule_master: waiting for modem time");
		LOG_DBG("taking time sem with sem count=%d", time_sem.count);
		k_sem_take(&time_sem, K_FOREVER);

		LOG_DBG("schedule_master: modem_time=%llu", modem_time);

		uint64_t sync_start_us = 0 + TDMA_BUFFER_US;
		uint64_t tx_start_us = sync_start_us + TDMA_SYNC_US + TDMA_BUFFER_TX_US;
		uint64_t rx_start_us = tx_start_us + TDMA_TX_US + TDMA_BUFFER_RX_US;

		LOG_DBG("schedule_master: slot layout");
		LOG_DBG("  sync_start_us=%llu", sync_start_us);
		LOG_DBG("  tx_start_us=%llu", tx_start_us);
		LOG_DBG("  rx_start_us=%llu", rx_start_us);

		uint64_t frame_start = modem_time;
		uint64_t sync_start_tick = frame_start + US_TO_MODEM_TICKS(sync_start_us);
		uint64_t tx_start_tick = frame_start + US_TO_MODEM_TICKS(tx_start_us);
		uint64_t rx_start_tick = frame_start + US_TO_MODEM_TICKS(rx_start_us);

		LOG_DBG("schedule_master: slot ticks");
		LOG_DBG("  sync_start_tick=%llu", sync_start_tick);
		LOG_DBG("  tx_start_tick=%llu", tx_start_tick);
		LOG_DBG("  rx_start_tick=%llu", rx_start_tick);

		char sync_buf[] = {0, 1, 9};

		dect_phy_tx(TX_HANDLE, &sync_hdr, sizeof(sync_buf), sync_buf, sync_start_tick);

		op_count +=1;

		LOG_DBG("schedule_master: scheduling TX payload");
		op_count += serial_to_dect(tx_start_tick, TDMA_TX_US);

		LOG_DBG("schedule_master: scheduling RX slot");

		dect_phy_rx(RX_HANDLE, NRF_MODEM_DECT_PHY_RX_MODE_CONTINUOUS, rx_start_tick, US_TO_MODEM_TICKS(TDMA_RX_US));
		op_count += 1;

		/**
				 * A total number of op count operations have been scheduled
				 * The maximum semaphore count is TDMA_MAX_STATE_COUNT
				 * To verify that op_count operations finished, one takes op_count semaphores
				 */
		for(int i = 0; i<op_count; i++){
			LOG_DBG("taking semaphore with opcount: %d", i);
			k_sem_take(&tdma_sem, K_FOREVER);
		}
		k_sem_reset(&tdma_sem);
		LOG_DBG("schedule_master: complete");
	}
}

// 664 us approx airtime
void schedule_slave(void)
{
	while (1) {
		LOG_DBG("RX duration: %u us", 10 * TDMA_TOTAL_US);
		int op_count = 0;
		int err = dect_phy_rx(
			RX_HANDLE,
			NRF_MODEM_DECT_PHY_RX_MODE_SINGLE_SHOT,
			0,//slave_sync_start,
			US_TO_MODEM_TICKS(10 * TDMA_TOTAL_US));
		k_sem_take(&operation_sem, K_FOREVER);;
		LOG_DBG("operation_sem released");
		LOG_DBG("sync_flag = %d", sync_flag);
		slave_frame_start = modem_time;
		if (sync_flag) {
			LOG_DBG("SYNC acquired");

			int64_t propagation_delay_us =
				get_delay_us(TDMA_SYNC_SS + 1);

			LOG_DBG("propagation_delay_us = %lld", propagation_delay_us);

			//slave_frame_start = slave_frame_start - US_TO_MODEM_TICKS(propagation_delay_us - SAFTY_MARGIN_US);
			int64_t rx_offset_us = TDMA_BUFFER_US + TDMA_SYNC_US + TDMA_BUFFER_RX_US;

			int64_t tx_offset_us = rx_offset_us + TDMA_RX_US + TDMA_BUFFER_TX_US;

			LOG_DBG("rx_offset_us = %lld", rx_offset_us);
			LOG_DBG("tx_offset_us = %lld", tx_offset_us);

			uint64_t rx_start_time =
				slave_frame_start +
				US_TO_MODEM_TICKS(rx_offset_us);

			uint64_t tx_start_time =
				slave_frame_start +
				US_TO_MODEM_TICKS(tx_offset_us);

			LOG_DBG("slave_frame_start = %llu",slave_frame_start);
			LOG_DBG("rx_start_time ticks = %llu",rx_start_time);
			LOG_DBG("tx_start_time ticks = %llu",tx_start_time);
			LOG_DBG("TDMA_RX_US = %u", TDMA_RX_US);
			LOG_DBG("TDMA_TX_US = %u", TDMA_TX_US);

			LOG_DBG("Starting CONTINUOUS RX");

			err = dect_phy_rx(
				RX_HANDLE,
				NRF_MODEM_DECT_PHY_RX_MODE_CONTINUOUS,
				rx_start_time,
				US_TO_MODEM_TICKS(TDMA_RX_US));
			op_count += 1;
			LOG_DBG("dect_phy_rx(continuous) returned: %d",
					 err);

			LOG_DBG("Calling serial_to_dect()");
			LOG_DBG("TX start time = %llu", tx_start_time);

			op_count += serial_to_dect(tx_start_time, TDMA_TX_US);

			LOG_DBG("serial_to_dect() finished");

			LOG_DBG("Modem ticks TDMA_TOTAL_US");
			slave_sync_start = slave_frame_start + US_TO_MODEM_TICKS(TDMA_TOTAL_US);

			LOG_DBG("sync_flag reset");

			for(int i = 0; i<op_count; i++){
				LOG_DBG("taking semaphore with opcount: %d", i);
				k_sem_take(&tdma_sem, K_FOREVER);
			}
			k_sem_reset(&tdma_sem);
			LOG_DBG("schedule_master: complete");
			sync_flag = 0;
		}
		else {
			LOG_DBG("No sync detected");
			//slave_sync_start = modem_time + US_TO_MODEM_TICKS(TDMA_TOTAL_US);
		}
	}
}

void init_tdma(void)
{
	LOG_DBG("init_tdma");
	//LOG_DBG("mode=%s",mode == MASTER ? "MASTER" : "SLAVE");

	LOG_DBG("TDMA timings:");
	LOG_DBG("  SYNC_US=%u", TDMA_SYNC_US);
	LOG_DBG("  TX_US=%u", TDMA_TX_US);
	LOG_DBG("  RX_US=%u", TDMA_RX_US);
	LOG_DBG("  TOTAL_US=%u", TDMA_TOTAL_US);

	if (mode == MASTER) {
		LOG_DBG("init_tdma: starting MASTER scheduler");
		schedule_basic_master();

	} else if (mode == SINK)
  {
    LOG_DBG("Starting basic sink");
    schedule_basic_sink();
  }
  else
  {
		LOG_DBG("init_tdma: starting SLAVE scheduler");
		schedule_slave();
	}
}

void tdma_on_pcc(const struct nrf_modem_dect_phy_pcc_event *evt)
{
	if (evt->hdr.hdr_type_1.df_mcs == TDMA_SYNC_MCS &&
		evt->hdr.hdr_type_1.packet_length == TDMA_SYNC_SS &&
		mode == SLAVE)
	{
		slave_frame_start = evt->stf_start_time;
		sync_flag = 1;

		LOG_DBG("valid sync detected");
		LOG_DBG("slave_frame_start=%llu", slave_frame_start);
	}
}
