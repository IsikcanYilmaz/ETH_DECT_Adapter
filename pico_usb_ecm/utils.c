#include "utils.h"
#include "debug_log.h"
#include "pico/cyw43_arch.h"
#include "hardware/gpio.h"

// #define CYW43_WL_GPIO_LED_PIN (0) // TODO 

void Utils_SwInit(void)
{
  gpio_init(MAC_ADDR_SWITCH_GPIO_PIN);
  gpio_set_input_enabled(MAC_ADDR_SWITCH_GPIO_PIN, true);

  gpio_init(LOG_ENABLE_SWITCH_GPIO_PIN);
  gpio_set_input_enabled(LOG_ENABLE_SWITCH_GPIO_PIN, true);
}

uint8_t Utils_GetMacSw(void)
{
  return gpio_get(MAC_ADDR_SWITCH_GPIO_PIN);
}

uint8_t Utils_GetLogEnSw(void)
{
  return gpio_get(LOG_ENABLE_SWITCH_GPIO_PIN);
}

int Utils_LedInit(void) 
{
  // For Pico W devices we need to initialise the driver etc
  return cyw43_arch_init();
}

// Turn the led on or off
void Utils_SetLed(bool led_on) 
{
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on);
}

void Utils_Hexdump(uint8_t *buf, uint16_t len)
{
  for (int i = 0; i < len; i++)
  {
    if (i % 8 == 0)
    {
      debug_log_printf("\n");
    }
    debug_log_printf("%02x ",* (uint8_t *)(buf + i));
  }
  debug_log_printf("\n");
}
