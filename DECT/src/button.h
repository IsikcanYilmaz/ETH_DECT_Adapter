/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef BUTTON_H
#define BUTTON_H

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

/**
 * @brief Descriptor for a single button.
 *
 * Populate this struct (usually via BUTTON_DEFINE) and pass it to
 * button_init() to configure the GPIO pin and attach an interrupt.
 *
 * The handler field uses Zephyr's own gpio_callback_handler_t:
 *   void handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
 */
typedef struct {
	const struct gpio_dt_spec  spec;    /**< GPIO DT spec (port + pin + flags). */
	gpio_callback_handler_t    handler; /**< Callback invoked on press.          */
	struct gpio_callback       cb_data; /**< Internal callback data – do not set. */
} button_t;

/**
 * @brief Convenience macro for buttons described in the devicetree.
 *
 * Usage example:
 *   BUTTON_DEFINE_DT(my_button, DT_ALIAS(sw0), my_button_handler);
 *
 * @param _name     C identifier for the resulting button_t variable.
 * @param _node_id  DT node identifier (e.g. DT_ALIAS(sw0)).
 * @param _handler  Function of type gpio_callback_handler_t to call on press.
 */
#define BUTTON_DEFINE_DT(_name, _node_id, _handler) \
	button_t _name = {                           \
		.spec    = GPIO_DT_SPEC_GET(_node_id, gpios), \
		.handler = (_handler),                   \
	}

/**
 * @brief Initialise a button: configure pin as input, attach edge interrupt.
 *
 * Must be called once per button before the button can generate callbacks.
 *
 * @param btn Pointer to the button_t to initialise.
 * @return 0 on success, negative errno on failure.
 */
int button_init(button_t *btn);

#endif /* BUTTON_H */
