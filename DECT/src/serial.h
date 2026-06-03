#ifndef SERIAL_H
#define SERIAL_H

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

#define SERIAL_RING_BUF_SIZE 30000 // check needed size 

void serial_init(void);
size_t serial_read(uint8_t *buf, size_t max_len);
size_t serial_write(const uint8_t *buf, size_t len);
size_t serial_rx_available(void);

#endif

