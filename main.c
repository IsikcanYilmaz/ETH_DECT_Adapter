#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/input/input.h>
#include <string.h>
#include <dk_buttons_and_leds.h>
#include "lean_wiznet_driver.h"

#if CONFIG_MAC_IMPL
#include "mac_main.h"
#endif

#if CONFIG_PHY_IMPL
#include "phy_main.h"
#endif

static const struct gpio_dt_spec ftPtSwitchInput = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), ft_pt_input_gpios);
static const struct gpio_dt_spec ftPtSwitchOutput = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), ft_pt_output_gpios);

const struct gpio_dt_spec tp23Switch = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tp_23_gpios);
const struct gpio_dt_spec tp24Switch = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tp_24_gpios);
const struct gpio_dt_spec tp25Switch = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tp_25_gpios);
const struct gpio_dt_spec tp26Switch = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tp_26_gpios);
const struct gpio_dt_spec tp27Switch = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tp_27_gpios);

#if 0
const struct gpio_dt_spec tp03Switch = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tp_03_gpios);
const struct gpio_dt_spec tp04Switch = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tp_04_gpios);
#endif

LOG_MODULE_REGISTER(app,LOG_LEVEL_INF);

static void button_handler(uint32_t state, uint32_t has_changed)
{
  if (has_changed & DK_BTN1_MSK)
  {
    if (state & DK_BTN1_MSK)
    {
      
    }
    else
    {

    }
  }
  else if (has_changed & DK_BTN2_MSK)
  {
    if (state & DK_BTN2_MSK)
    {

    }
    else
    {

    }
  }
  else if (has_changed & DK_BTN3_MSK)
  {
    if (state & DK_BTN3_MSK)
    {

    }
    else
    {

    }
  }
  else if (has_changed & DK_BTN4_MSK)
  {
    if (state & DK_BTN4_MSK)
    {
      
    }
    else
    {

    }
  }
  // dk_set_leds(state);
  LOG_DBG("%s Button state %x has changed %x", __FUNCTION__, state, has_changed);
}

int main(void)
{
	int err;
	uint16_t device_id;

  err = dk_buttons_init(button_handler);
  // err = dk_leds_init();

  // FT PT SWITCH
  // On Init P0.21 will be read. If it's HIGH then this board is FT 
  err = gpio_pin_configure_dt(&ftPtSwitchInput, GPIO_INPUT | GPIO_PULL_DOWN);
  err = gpio_pin_configure_dt(&ftPtSwitchOutput, GPIO_OUTPUT_ACTIVE);

  // Test points
  err = gpio_pin_configure_dt(&tp23Switch, GPIO_OUTPUT_INACTIVE);
  err = gpio_pin_configure_dt(&tp24Switch, GPIO_OUTPUT_INACTIVE);
  err = gpio_pin_configure_dt(&tp25Switch, GPIO_OUTPUT_INACTIVE);
  err = gpio_pin_configure_dt(&tp26Switch, GPIO_OUTPUT_INACTIVE);
  err = gpio_pin_configure_dt(&tp27Switch, GPIO_OUTPUT_INACTIVE);
  // err = gpio_pin_configure_dt(&tp03Switch, GPIO_OUTPUT_INACTIVE);
  // err = gpio_pin_configure_dt(&tp04Switch, GPIO_OUTPUT_INACTIVE);

  // Check the ft/pt switch 
  gpio_pin_set_dt(&ftPtSwitchOutput, 1);
  bool iAmFt = gpio_pin_get_dt(&ftPtSwitchInput);
  LOG_WRN("I AM %s", (iAmFt) ? "FT" : "PT");

	err = hwinfo_get_device_id((void *)&device_id, sizeof(device_id));
	if (err < 0) 
  {
		LOG_ERR("Failed to get device ID: %d", err);
		device_id = 0;
	}
	LOG_INF("Device ID: 0x%04x", device_id);

  LeanWiznet_Init();

  #if CONFIG_MAC_IMPL
  LeanWiznet_SetRxCallback(Mac_TxReady);
  Mac_main(iAmFt);
  #endif

  #if CONFIG_PHY_IMPL
  LeanWiznet_SetRxCallback(DectPhy_WiznetAlert);
  DectPhy_Main(iAmFt); 
  #endif

  while (true)
  {
    LOG_ERR("SET CONFIG_MAC_IMPL OR CONFIG_PHY_IMPL TO y");
    k_sleep(K_MSEC(1000));
  }
  return 0;
}
