#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/net_buf.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/uart.h>

/*
 * UART1 to tx rx eth packets to and from the rpi pico
 * Rx pin: P0.28
 * Tx pin: P0.29
 * this code is pulled from the zephyr sample code drivers/uart/async_api
 */

/* change this to any other UART peripheral if desired */
#define UART_DEVICE_NODE DT_NODELABEL(uart1) // uart1

/* Maximum number of packets to generate per iteration */
#define LOOP_ITER_MAX_TX 4
/* Maximum size of our TX packets */
#define MAX_TX_LEN 32
#define RX_CHUNK_LEN 32

/* Buffer pool for our TX payloads */
NET_BUF_POOL_DEFINE(tx_pool, LOOP_ITER_MAX_TX, MAX_TX_LEN, 0, NULL);

struct k_fifo tx_queue;
struct net_buf *tx_pending_buffer;
uint8_t async_rx_buffer[2][RX_CHUNK_LEN];
volatile uint8_t async_rx_buffer_idx;

bool rxReady = false;
static struct k_work_delayable rxReadyTaskHandle;

static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

LOG_MODULE_REGISTER(dect_uart, LOG_LEVEL_ERR);

static void uart_callback(const struct device *dev, struct uart_event *evt, void *user_data)
{
	struct net_buf *buf;
	int rc;

	LOG_DBG("EVENT: %d", evt->type);

	switch (evt->type) {
	case UART_TX_DONE:
		LOG_DBG("TX complete %p", tx_pending_buffer);

		/* Free TX buffer */
		net_buf_unref(tx_pending_buffer);
		tx_pending_buffer = NULL;

		/* Handle any queued buffers */
		buf = k_fifo_get(&tx_queue, K_NO_WAIT);
		if (buf != NULL) {
			rc = uart_tx(dev, buf->data, buf->len, 0);
			if (rc != 0) {
				LOG_ERR("TX from ISR failed (%d)", rc);
				net_buf_unref(buf);
			} else {
				tx_pending_buffer = buf;
			}
		}
		break;
	case UART_RX_BUF_REQUEST:
		/* Return the next buffer index */
		LOG_DBG("Providing buffer index %d", async_rx_buffer_idx);
		rc = uart_rx_buf_rsp(dev, async_rx_buffer[async_rx_buffer_idx],
				     sizeof(async_rx_buffer[0]));
		__ASSERT_NO_MSG(rc == 0);
		async_rx_buffer_idx = async_rx_buffer_idx ? 0 : 1;
		break;
	case UART_RX_BUF_RELEASED:
	case UART_RX_DISABLED:
		break;
	case UART_RX_RDY:
		LOG_HEXDUMP_INF(evt->data.rx.buf + evt->data.rx.offset, evt->data.rx.len, "RX_RDY");
    k_work_schedule(&rxReadyTaskHandle, K_NO_WAIT);
		rxReady = true;
		break;
	default:
		LOG_WRN("Unhandled event %d", evt->type);
	}
}

// static k_work_delayable dectUartTaskHandle;
void DectUart_RxReadyTask(void)
{
	rxReady = false;
	LOG_INF("%s fired!!!!", __FUNCTION__);
}

int DectUart_Init(void)
{
	bool rx_enabled = false;
	int loop_counter = 0;
	uint8_t num_tx;
	int tx_len;
	int rc;

	// Register the async interrupt handler
	uart_callback_set(uart_dev, uart_callback, (void *)uart_dev);

	async_rx_buffer_idx = 1;
	uart_rx_enable(uart_dev, async_rx_buffer[0], RX_CHUNK_LEN, 100);

	// Kick off our handler task
	// k_work_init(dectUartTaskHandle, DectUart_Task);

	// k_work_init_delayable(&rxReadyTaskHandle, DectUart_RxReadyTask);

	return 0;
}

/*
* attempt 1: send everything without caring about packet encapsulations
*
*/

int DectUart_Tx(uint8_t *buf, uint16_t len)
{
	LOG_INF("%s", __FUNCTION__);
	struct net_buf *tx_buf = net_buf_alloc(&tx_pool, K_FOREVER);
	memcpy(tx_buf->data, buf, len); // do we even need this?
	net_buf_add(tx_buf, len);

	LOG_HEXDUMP_INF(tx_buf->data, len, "TX HEX DUMP");

	int ret = uart_tx(uart_dev, tx_buf->data, tx_buf->len, SYS_FOREVER_US);
	if (ret == 0)
	{
		tx_pending_buffer = tx_buf;
	}
	else if (ret == -EBUSY)
	{
		LOG_DBG("Queueing buffer %p", tx_buf);
		k_fifo_put(&tx_queue, tx_buf);
	}
	else
	{
		LOG_ERR("%s Unknown error! %d", __FUNCTION__, ret);
	}
	return ret;
}

void DectUart_Test(void)
{
	char *teststr = "aaaaaaaabbbbbbbbccccccccdddddddd\r\n";
  // uint8_t testbuf[] = {0xC0, 0xC4, 0xFA, 0xCE, 0x05, 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
	uint32_t magic = 0xCEFAC4C0;
	uint16_t len = 0x0005;
	const uint8_t header[] = {0xC0, 0xC4, 0xFA, 0xCE, 32, 0x00};
	const uint8_t pl[] = {0x30, 0x31, 0x32, 0x33, 0x34};
	int ret = DectUart_Tx(header, 6);
	k_sleep(K_USEC(100));
	ret = DectUart_Tx(teststr, 32);
	// int ret = DectUart_Tx(teststr, 32);
}
