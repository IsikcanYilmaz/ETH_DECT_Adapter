#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <stdbool.h>

#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)
#define LED2_NODE DT_ALIAS(led2)
#define LED3_NODE DT_ALIAS(led3)
#define NUM_LEDS 4

static const struct gpio_dt_spec leds[NUM_LEDS] = {GPIO_DT_SPEC_GET(LED0_NODE, gpios), GPIO_DT_SPEC_GET(LED1_NODE, gpios), GPIO_DT_SPEC_GET(LED2_NODE, gpios), GPIO_DT_SPEC_GET(LED3_NODE, gpios)};

int led_init(void)
{
	int ret = 0;
	for (int i = 0; i < NUM_LEDS; i++)
	{
		ret |= gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
	}
	return ret;
}

int led_set(int lednum, bool on)
{
	if (lednum < NUM_LEDS)
		return gpio_pin_set_dt(&leds[lednum], (int) on);
	else
		return 1;
}
