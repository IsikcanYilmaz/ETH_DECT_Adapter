#include "dect_serial.h"
#include "dect_phy_events.h"
#include "tdma.h"

LOG_MODULE_REGISTER(dect_serial,LOG_LEVEL_INF);

#define PAYLOAD_MAX 141 // MCS 3 with ss len 2
#define DECT_LEN_PREFIX 2

static uint8_t tx_buf[PAYLOAD_LEN_MAX * TDMA_MAX_TRANSMISSIONS];
static uint8_t serial_buf[PAYLOAD_MAX - DECT_LEN_PREFIX];

uint8_t dect_rx_buf[PAYLOAD_MAX];
static volatile size_t dect_rx_len = 0;
static volatile bool dect_rx_flag = false;

struct nrf_modem_dect_phy_hdr_type_1 tx_hdr = {
	.packet_length      = FRAME_TX_SS,
	.packet_length_type = 0x00,
	.header_format      = 0x0,
	.short_network_id   = (CONFIG_NETWORK_ID & 0xff),
	.transmitter_id_hi  = (0 >> 8) & 0xff,
	.transmitter_id_lo  = (0 & 0xff),
	.df_mcs             = FRAME_TX_MCS,
	.reserved           = 0,
	.transmit_power     = CONFIG_TX_POWER,
};

int serial_to_dect(uint64_t start_time, uint64_t duration_us)
{
	/*
		 * Approximate one TX slot airtime.
		 * Later this should be replaced with the RTT matrix lookup:
		 * airtime = rtt_matrix[...]/2;
		 */
	//uint64_t airtime = 2 * get_delay_us(FRAME_TX_SS + 1);
	uint64_t airtime = 8000;

	if (airtime == 0) {
		LOG_ERR("Invalid airtime");
		return 0;
	}

	/* Maximum number of transmissions that fit into the window */
	uint16_t max_tx = duration_us / airtime;
	LOG_DBG("Max transmissions %u",max_tx);

	size_t avail = serial_rx_available();
	if ((int32_t)avail < 0) {
		LOG_ERR("serial_rx_available failed: %d", (int32_t)avail);
		return 0;
	}

	if (avail == 0) {
		LOG_DBG("No serial data available");
		return 0;
	}

	const size_t max_chunk = PAYLOAD_MAX - DECT_LEN_PREFIX;

	uint16_t chunks =
		(avail + max_chunk - 1) / max_chunk;

	uint16_t tx_count = MIN(MIN(chunks, max_tx), TDMA_MAX_TRANSMISSIONS);

	LOG_DBG("Scheduling %u/%u transmissions (airtime=%llu us)",
				 tx_count,
				 chunks,
				 airtime);

	size_t tx_buf_offset = 0;

	for (uint16_t i = 0; i < tx_count; i++) {

		avail = serial_rx_available();
		if (avail == 0) {
			break;
		}

		size_t chunk = MIN(avail, max_chunk);

		/* Ensure enough room remains in tx buffer */
		size_t frame_len = chunk + DECT_LEN_PREFIX;

		if ((tx_buf_offset + frame_len) > sizeof(tx_buf)) {
			LOG_ERR("TX buffer exhausted");
			break;
		}

		uint8_t *frame_ptr = &tx_buf[tx_buf_offset];

		size_t n = serial_read(serial_buf, chunk);

		if (n == 0) {
			LOG_WRN("serial_read returned 0");
			break;
		}

		LOG_DBG("Read %u bytes from serial", n);

		/* Prefix payload length */
		frame_ptr[0] = (n >> 8) & 0xFF;
		frame_ptr[1] = n & 0xFF;

		memcpy(&frame_ptr[DECT_LEN_PREFIX], serial_buf, n);

		uint64_t tx_start = start_time + US_TO_MODEM_TICKS(((uint64_t)i * airtime));

		LOG_DBG("Scheduling DECT TX #%u at %llu us", i, tx_start);

		int err = dect_phy_tx(
			TX_HANDLE,
			&tx_hdr,
			n + DECT_LEN_PREFIX,
			frame_ptr,
			tx_start);

		if (err) {
			LOG_ERR("dect_phy_tx failed: %d", err);
			continue;
		}
		//k_sem_take(&operation_sem, K_FOREVER);
		tx_buf_offset += frame_len;
	}
	return tx_count;
}

void serial_on_pdc(const struct nrf_modem_dect_phy_pdc_event *evt)
{
	if (sync_flag)
  {
		return;
	}

	LOG_INF("PDC event received: len=%d", evt->len);

	const uint8_t *data = (const uint8_t *)evt->data;

	LOG_HEXDUMP_INF(data, evt->len, "PDC EVENT");

	if (evt->len < DECT_LEN_PREFIX) {
		LOG_WRN("PDC event too short: %d", evt->len);
		return;
	}

	uint16_t msg_len = (data[0] << 8) | data[1];
	LOG_DBG("Decoded message length: %d", msg_len);

	if (msg_len == 0) {
		LOG_WRN("Received empty message");
		return;
	}

	if (msg_len > evt->len - DECT_LEN_PREFIX) {
		LOG_ERR("Invalid length: msg_len=%d, available=%d", msg_len, evt->len - DECT_LEN_PREFIX);
		return;
	}

	if (msg_len > sizeof(dect_rx_buf)) {
		LOG_ERR("Message too large for buffer: %d", msg_len);
		return;
	}

	memcpy(dect_rx_buf, data + DECT_LEN_PREFIX, msg_len);
	dect_rx_len = msg_len;
	dect_rx_flag = true;
	serial_write(dect_rx_buf, dect_rx_len);

	LOG_DBG("Stored RX message (%d bytes)", msg_len);
};

void sink_serial_on_pdc(const struct nrf_modem_dect_phy_pdc_event *evt)
{	
  LOG_INF("PDC event received: len=%d", evt->len);

	const uint8_t *data = (const uint8_t *)evt->data;

	LOG_HEXDUMP_INF(data, evt->len, "PDC EVENT");

  serial_write(data, evt->len);
}

void test_on_pdc(const struct nrf_modem_dect_phy_pdc_event *evt)
{
  //
}
