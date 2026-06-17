#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/drivers/hwinfo.h>
#include <string.h>

#include "button.h"
#include "serial.h"
#include "dect_phy_events.h"
#include "dect_phy.h"
#include "ntp.h"
#include "dect_serial.h"
#include "tdma.h"
#include "led.h"

#include "tester.h"

LOG_MODULE_REGISTER(app,LOG_LEVEL_INF);

/*******************************************//**
 *  ... BUTTON SETUP
 ***********************************************/

void pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins);
BUTTON_DEFINE_DT(button1, DT_ALIAS(sw0), pressed);
BUTTON_DEFINE_DT(button2, DT_ALIAS(sw1), pressed);
BUTTON_DEFINE_DT(button3, DT_ALIAS(sw2), pressed);
BUTTON_DEFINE_DT(button4, DT_ALIAS(sw3), pressed);

K_SEM_DEFINE(test_sem,    0, 1);

static const char *teststr = "ASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWERASDFQWER";
void pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	// TODO probably better way to do this mapping
	if (pins == (1 << button1.spec.pin))
	{
		mode = MASTER;
		LOG_INF("MODE MASTER");
	}
	else if (pins == (1 << button2.spec.pin))
	{
		mode = SLAVE;
		LOG_INF("MODE SLAVE");
	}
	else if (pins == (1 << button3.spec.pin))
	{
		mode = SINK;
		LOG_INF("MODE SINK");
	}
	else if (pins == (1 << button4.spec.pin))
	{
		// nrf_modem_dect_phy_time_get();
		//   static struct nrf_modem_dect_phy_hdr_type_1 tx_hdr = {
		//     .packet_length      = 1,
		//     .packet_length_type = 0x01,
		//     .header_format      = 0x0,
		//     .short_network_id   = (CONFIG_NETWORK_ID & 0xff),
		//     .transmitter_id_hi  = (0 >> 8) & 0xff,
		//     .transmitter_id_lo  = (0 & 0xff),
		//     .df_mcs             = FRAME_TX_MCS,
		//     .reserved           = 0,
		//     .transmit_power     = CONFIG_TX_POWER,
		//   };
		//
		//   uint64_t start_time = 0;
		//   LOG_INF("SENDING TEST PAYLOAD WITH %d USECOND DELAY", start_time);
		//   // k_sem_take(&tdma_sem, K_FOREVER);
		//   dect_phy_tx(TX_HANDLE, &tx_hdr, 32, teststr, start_time);
		//   k_sem_take(&tdma_sem, K_FOREVER);
		//   k_sem_reset(&tdma_sem);
    //
    // nrf_modem_dect_phy_capability_get();
	}
	LOG_INF("BUTTON PRESSED %s %d", dev->name, pins);
}

/* ===================== DECT RX CALLBACK ===================== */

int main(void)
{
	int err;
	uint16_t device_id;

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Modem init failed: %d", err);
		return 0;
	}

	//dect_events_register_pdc_handler(on_pdc);
	LOG_DBG("PDC handler registered");

	// dect_phy_init();
	// LOG_DBG("DECT PHY initialized");

	serial_init();
	LOG_DBG("Serial initialized");

	button_init(&button1);
	button_init(&button2);
	button_init(&button3);
	button_init(&button4);
	LOG_DBG("Button initialized");

	led_init();

	// DectUart_Init();
	LOG_DBG("Dect UART Initialized");

	err = hwinfo_get_device_id((void *)&device_id, sizeof(device_id));
	if (err < 0) {
		LOG_ERR("Failed to get device ID: %d", err);
		device_id = 0;
	}
	LOG_DBG("Device ID: 0x%04x", device_id);

	while(mode == UNDEFINED_ROLE){
		LOG_INF("BUTTON1 FOR MASTER, BUTTON2 FOR SLAVE, BUTTON3 FOR SINK");
		k_sleep(K_MSEC(1000));
	}

  testloop(mode);

	led_set((mode == MASTER) ? MASTER_LED_ID : SLAVE_LED_ID, true);

	// dect_events_register_pcc_handler(tdma_on_pcc);
	// dect_events_register_pdc_handler(serial_on_pdc);
	// dect_events_register_pdc_handler(sink_serial_on_pdc);

	LOG_DBG("Dect serial initialized");
	init_tdma();
	LOG_DBG("Tdma initialized ID");
}
