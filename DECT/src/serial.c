#include "serial.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(serial);

static const struct device *uart_dev;
RING_BUF_DECLARE(serial_ringbuf, SERIAL_RING_BUF_SIZE);

static void uart_cb(const struct device *dev, void *user_data)
{
    uint8_t buf[64];
    int rx;
    ARG_UNUSED(user_data);

    while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
        if (uart_irq_rx_ready(dev)) {
            rx = uart_fifo_read(dev, buf, sizeof(buf));
            if (rx > 0) {
                ring_buf_put(&serial_ringbuf, buf, rx);
            }
        }
    }
}

void serial_init(void)
{
    uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));
    __ASSERT(device_is_ready(uart_dev), "UART1 not ready");
    ring_buf_reset(&serial_ringbuf);
    uart_irq_callback_set(uart_dev, uart_cb);
    uart_irq_rx_enable(uart_dev);
}

size_t serial_read(uint8_t *buf, size_t max_len)
{
    return ring_buf_get(&serial_ringbuf, buf, max_len);
}

size_t serial_rx_available(void)
{
    return ring_buf_size_get(&serial_ringbuf);
}

size_t serial_write(const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    for (size_t i = 0; i < len; ++i) {
        uart_poll_out(uart_dev, buf[i]);
        sent++;
    }
    return sent;
}

