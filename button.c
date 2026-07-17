#include "button.h"
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

LOG_MODULE_REGISTER(button, LOG_LEVEL_INF);

#include <dk_buttons_and_leds.h>

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
  LOG_DBG("%s Button state %x has changed %x", __FUNCTION__, state, has_changed);
}

int Button_Init(void)
{
  int err = dk_buttons_init(button_handler);
  if (err)
  {
    LOG_ERR("Failed to init buttons!");
    return err;
  }

  return 0;
}


#if 0

/*
 * yanked from zephyr/samples/drivers/gpio/button_interrupt
 * Uses zephyr gpio dt way of doing things
 */

static const struct gpio_dt_spec button0 = GPIO_DT_SPEC_GET_OR(DT_ALIAS(sw0), gpios, {0});
static const struct gpio_dt_spec button1 = GPIO_DT_SPEC_GET_OR(DT_ALIAS(sw1), gpios, {0});
static const struct gpio_dt_spec button2 = GPIO_DT_SPEC_GET_OR(DT_ALIAS(sw2), gpios, {0});
static const struct gpio_dt_spec button3 = GPIO_DT_SPEC_GET_OR(DT_ALIAS(sw3), gpios, {0});

static const struct gpio_dt_spec *buttons[4] = {&button0, &button1, &button2, &button3};

static struct gpio_callback button_callbacks[4];

void Button_Pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
  for (int i = 0; i < 4; i++)
  {
    if (&button_callbacks[i] == cb)
    {
      LOG_INF("Button %d pressed", i);
    }
  }
}

int Button_Init(void)
{
  int err;

  for (int i = 0; i < 4; i++)
  {
    struct gpio_dt_spec *btn = buttons[i];

    if (!gpio_is_ready_dt(btn)) 
    {
      LOG_ERR("GPIO device %s is not ready", btn->port->name);
      return -ENODEV;
    }

    err = gpio_pin_configure_dt(btn, GPIO_INPUT);
    if (err) 
    {
      LOG_ERR("Failed to configure %s pin %d: %d", btn->port->name, btn->pin, err);
      return err;
    }

    err = gpio_pin_interrupt_configure_dt(btn, GPIO_INT_EDGE_TO_ACTIVE);
    if (err) 
    {
      LOG_ERR("Failed to configure interrupt on %s pin %d: %d", btn->port->name, btn->pin, err);
      return err;
    }

    gpio_init_callback(&button_callbacks[i], Button_Pressed, BIT(btn->pin));
    gpio_add_callback(btn->port, &button_callbacks[i]);

    LOG_DBG("Button on %s pin %d initialised", btn->port->name, btn->pin);
  }

	return 0;
}
#endif
