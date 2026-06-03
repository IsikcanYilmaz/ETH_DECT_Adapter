/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "button.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(button);

int button_init(button_t *btn)
{
	int err;
	/*
	if (!btn || !btn->handler) {
		LOG_ERR("button_init: invalid button descriptor");
		return -EINVAL;
	}
	*/
	if (!gpio_is_ready_dt(&btn->spec)) {
		LOG_ERR("GPIO device %s is not ready", btn->spec.port->name);
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&btn->spec, GPIO_INPUT);
	if (err) {
		LOG_ERR("Failed to configure %s pin %d: %d",
			btn->spec.port->name, btn->spec.pin, err);
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(&btn->spec, GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		LOG_ERR("Failed to configure interrupt on %s pin %d: %d",
			btn->spec.port->name, btn->spec.pin, err);
		return err;
	}

	gpio_init_callback(&btn->cb_data, btn->handler, BIT(btn->spec.pin));
	gpio_add_callback(btn->spec.port, &btn->cb_data);

	LOG_DBG("Button on %s pin %d initialised", btn->spec.port->name, btn->spec.pin);
	return 0;
}
