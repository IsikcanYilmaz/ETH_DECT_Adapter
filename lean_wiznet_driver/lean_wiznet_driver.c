/* W5500 Stand-alone Ethernet Controller with SPI
 * 
 * This is a stripped down version of Zephyr's Wiznet W5500 driver
 * Since we only need promiscuous mode and none of the other features of the W5500, this module
 * initializes the hardware with MACRAW enabled and MAC Filtering disabled. It does not touch any of the
 * other features of the chip (like actual network controller tasks).
 *
 * For our intents and purposes, this should be fine. Later in the future it would be better to separate
 * some higher level tasks that this module currently does (like taking and sending packets to higher levels)
 * to other locations.
 *
 * Currently, all one has to do is call the init function, then wait for packets in the ethRxQueue, or put tx packets
 * into the ethTxQueue for them to be sent away
 */

#include <zephyr/logging/log.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <string.h>
#include <errno.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/byteorder.h>
#include "lean_wiznet_driver.h"

#define DT_DRV_COMPAT	wiznet_w5500

LOG_MODULE_REGISTER(lean_wiznet, LOG_LEVEL_WRN);

#define W5500_NODE      DT_NODELABEL(w5500_dev)
static const struct spi_dt_spec w5500_spi_handle = SPI_DT_SPEC_GET(W5500_NODE, SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8), 0);

static const struct gpio_dt_spec w5500_rst_handle = GPIO_DT_SPEC_GET(DT_NODELABEL(w5500_rst), gpios);
static const struct gpio_dt_spec w5500_int_handle = GPIO_DT_SPEC_GET(DT_NODELABEL(w5500_int), gpios);

DEVICE_DECLARE(eth_w5500_phy_0);

static struct LeanWiznet_runtime leanwiznet_0_runtime = { 
	.tx_sem = Z_SEM_INITIALIZER(leanwiznet_0_runtime.tx_sem, 1, UINT_MAX),
	.int_sem  = Z_SEM_INITIALIZER(leanwiznet_0_runtime.int_sem, 0, UINT_MAX),
  .spi_mutex = Z_MUTEX_INITIALIZER(leanwiznet_0_runtime.spi_mutex),
};

static const struct LeanWiznet_config leanwiznet_0_config = {
	.spi = &w5500_spi_handle,
	.interrupt = &w5500_int_handle,
	.reset = &w5500_rst_handle,
};

#define XSTR(x) STR(x)
#define STR(x) #x

#if CONFIG_W5500_INTERRUPTLESS
#define CONFIG_ETH_W5500_MONITOR_PERIOD (1)
#endif

#define WIZNET_OUI_B0	0x00
#define WIZNET_OUI_B1	0x08
#define WIZNET_OUI_B2	0xdc

#define W5500_SPI_BLOCK_SELECT(addr)	(((addr) >> 16) & 0x1f)
#define W5500_SPI_READ_CONTROL(addr)	(W5500_SPI_BLOCK_SELECT(addr) << 3)
#define W5500_SPI_WRITE_CONTROL(addr) ((W5500_SPI_BLOCK_SELECT(addr) << 3) | BIT(2))

static RxHappenedCallback rxcb = NULL; 

K_QUEUE_DEFINE(ethRxQueue);
K_QUEUE_DEFINE(ethTxQueue);

static int w5500_spi_read(struct LeanWiznet_config *cfg, uint32_t addr,uint8_t *data, size_t len)
{
	int ret;

	uint8_t cmd[3] = {
		addr >> 8,
		addr,
		W5500_SPI_READ_CONTROL(addr)
	};
	const struct spi_buf tx_buf = {
		.buf = cmd,
		.len = ARRAY_SIZE(cmd),
	};
	const struct spi_buf_set tx = {
		.buffers = &tx_buf,
		.count = 1,
	};
	/* skip the default dummy 0x010203 */
	const struct spi_buf rx_buf[2] = {
		{
			.buf = NULL,
			.len = 3
		},
		{
			.buf = data,
			.len = len
		},
	};
	const struct spi_buf_set rx = {
		.buffers = rx_buf,
		.count = ARRAY_SIZE(rx_buf),
	};

	ret = spi_transceive_dt(cfg->spi, &tx, &rx);

	return ret;
}

static int w5500_spi_write(struct LeanWiznet_config *cfg, uint32_t addr, uint8_t *data, size_t len)
{
	int ret;
	uint8_t cmd[3] = {
		addr >> 8,
		addr,
		W5500_SPI_WRITE_CONTROL(addr),
	};
	const struct spi_buf tx_buf[2] = {
		{
			.buf = cmd,
			.len = ARRAY_SIZE(cmd),
		},
		{
			.buf = data,
			.len = len,
		},
	};
	const struct spi_buf_set tx = {
		.buffers = tx_buf,
		.count = ARRAY_SIZE(tx_buf),
	};

	ret = spi_write_dt(cfg->spi, &tx);

	return ret;
}

static int w5500_readbuf(struct LeanWiznet_config *cfg, uint16_t offset, uint8_t *buf, size_t len)
{
	uint32_t addr;
	size_t remain = 0;
	int ret;
	const uint32_t mem_start = W5500_Sn_RX_MEM_START;
	const uint32_t mem_size = W5500_RX_MEM_SIZE;

	offset %= mem_size;
	addr = mem_start + offset;

	if (offset + len > mem_size) {
		remain = (offset + len) % mem_size;
		len = mem_size - offset;
	}

	ret = w5500_spi_read(cfg, addr, buf, len);
	if (ret || !remain) {
		return ret;
	}

	return w5500_spi_read(cfg, mem_start, buf + len, remain);
}

static int w5500_writebuf(struct LeanWiznet_config *cfg, uint16_t offset, uint8_t *buf, size_t len)
{
	uint32_t addr;
	size_t remain = 0;
	int ret;
	const uint32_t mem_start = W5500_Sn_TX_MEM_START;
	const uint32_t mem_size = W5500_TX_MEM_SIZE;

	offset %= mem_size;
	addr = mem_start + offset;

	if (offset + len > mem_size) {
		remain = (offset + len) % mem_size;
		len = mem_size - offset;
	}

	ret = w5500_spi_write(cfg, addr, buf, len);
	if (ret || !remain) {
		return ret;
	}

	return w5500_spi_write(cfg, mem_start, buf + len, remain);
}

static int w5500_command(struct LeanWiznet_config *cfg, uint8_t cmd)
{
	uint8_t reg;
	k_timepoint_t end = sys_timepoint_calc(K_MSEC(100));

	w5500_spi_write(cfg, W5500_S0_CR, &cmd, 1);
	while (true) {
		w5500_spi_read(cfg, W5500_S0_CR, &reg, 1);
		if (!reg) {
			break;
		}
		if (sys_timepoint_expired(end)) {
			return -EIO;
		}
		k_busy_wait(W5500_PHY_ACCESS_DELAY);
	}
	return 0;
}

static int w5500_tx(struct LeanWiznet_config *cfg, struct LeanWiznet_runtime *ctx, char *buf, size_t len)
{
	uint16_t offset;
	uint8_t off[2];
	int ret;

  int mutexret = k_mutex_lock(&ctx->spi_mutex, K_MSEC(LEAN_WIZNET_SPI_MUTEX_TIMEOUT_MS));
  if (mutexret)
  {
    LOG_ERR("%s Couldnt acquire mutex", __FUNCTION__);
    return NULL;
  }

	w5500_spi_read(cfg, W5500_S0_TX_WR, off, 2);
	offset = sys_get_be16(off);

	ret = w5500_writebuf(cfg, offset, buf, len);
	if (ret < 0) {
		return ret;
	}

	sys_put_be16(offset + len, off);
	w5500_spi_write(cfg, W5500_S0_TX_WR, off, 2);

	w5500_command(cfg, S0_CR_SEND);
	if (k_sem_take(&ctx->tx_sem, K_MSEC(10))) {
		return -EIO;
	}

  k_mutex_unlock(&ctx->spi_mutex);

	return 0;
}

static struct LeanWiznet_packet* w5500_rx(struct LeanWiznet_config *cfg, struct LeanWiznet_runtime *ctx)
{
	uint8_t header[2];
	uint8_t tmp[2];
	uint16_t off;
	uint16_t rx_len;
	uint16_t rx_buf_len;
	uint16_t read_len;
	uint16_t reader;
  
  int mutexret = k_mutex_lock(&ctx->spi_mutex, K_MSEC(LEAN_WIZNET_SPI_MUTEX_TIMEOUT_MS));
  if (mutexret)
  {
    LOG_ERR("%s Couldnt acquire mutex", __FUNCTION__);
    return NULL;
  }

	w5500_spi_read(cfg, W5500_S0_RX_RSR, tmp, 2); // Get Rx Received Size
	rx_buf_len = sys_get_be16(tmp);

	if (rx_buf_len == 0) {
		return NULL;
	}

	w5500_spi_read(cfg, W5500_S0_RX_RD, tmp, 2); // Get Rx Read Pointer
	off = sys_get_be16(tmp);

	w5500_readbuf(cfg, off, header, 2); // From read pointer read 2 bytes
	rx_len = sys_get_be16(header) - 2;

  struct LeanWiznet_packet *buf = (struct LeanWiznet_packet *) k_malloc(rx_len + sizeof(struct LeanWiznet_packet));
  
	read_len = rx_len;
	reader = off + 2;

  w5500_readbuf(cfg, reader, buf->payload, rx_len);

  buf->size = rx_len;

  LOG_DBG("Rx %d bytes", rx_len);
  LOG_HEXDUMP_DBG(buf->payload, rx_len, "RX");

	sys_put_be16(off + 2 + rx_len, tmp);
	w5500_spi_write(cfg, W5500_S0_RX_RD, tmp, 2);
	w5500_command(cfg, S0_CR_RECV);

  k_mutex_unlock(&ctx->spi_mutex);

  return buf;
}

static int w5500_hw_start(struct LeanWiznet_config *cfg)
{
	uint8_t mode = S0_MR_MACRAW | BIT(W5500_S0_MR_MF);
	uint8_t mask = IR_S0;

	/* configure Socket 0 with MACRAW mode and MAC filtering enabled */
	w5500_spi_write(cfg, W5500_S0_MR, &mode, 1);
	w5500_command(cfg, S0_CR_OPEN);

	/* enable interrupt */
	w5500_spi_write(cfg, W5500_SIMR, &mask, 1);

  // JON disable MAC filtering. TODO do it above why not?
  w5500_spi_read(cfg, W5500_S0_MR, &mode, 1);
  WRITE_BIT(mode, W5500_S0_MR_MF, 0);
  w5500_spi_write(cfg, W5500_S0_MR, &mode, 1);

	return 0;
}

static int w5500_hw_stop(struct LeanWiznet_config *cfg)
{
	uint8_t mask = 0;

	/* disable interrupt */
	w5500_spi_write(cfg, W5500_SIMR, &mask, 1);
	w5500_command(cfg, S0_CR_CLOSE);

	return 0;
}

static int w5500_soft_reset(struct LeanWiznet_config *cfg)
{
	int ret;
	uint8_t mask = 0;
	uint8_t tmp = MR_RST;

	ret = w5500_spi_write(cfg, W5500_MR, &tmp, 1);
	if (ret < 0) {
		return ret;
	}

	k_msleep(5);
	tmp = MR_PB;
	w5500_spi_write(cfg, W5500_MR, &tmp, 1);

	/* disable interrupt */
	return w5500_spi_write(cfg, W5500_SIMR, &mask, 1);
}

static void w5500_gpio_callback(struct gpio_callback *cb, uint32_t pins)
{
	struct LeanWiznet_runtime *ctx = &leanwiznet_0_runtime;
	k_sem_give(&ctx->int_sem);
}

static void w5500_set_macaddr(struct LeanWiznet_config *cfg, const struct device *dev)
{
	struct LeanWiznet_runtime *ctx = &leanwiznet_0_runtime;

	w5500_spi_write(cfg, W5500_SHAR, ctx->mac_addr, sizeof(ctx->mac_addr));
}

static void w5500_memory_configure(struct LeanWiznet_config *cfg)
{
	int i;
	uint8_t mem = 0x10;

	/* Configure RX & TX memory to 16K */
	w5500_spi_write(cfg, W5500_Sn_RXMEM_SIZE(0), &mem, 1);
	w5500_spi_write(cfg, W5500_Sn_TXMEM_SIZE(0), &mem, 1);

	mem = 0;
	for (i = 1; i < 8; i++) {
		w5500_spi_write(cfg, W5500_Sn_RXMEM_SIZE(i), &mem, 1);
		w5500_spi_write(cfg, W5500_Sn_TXMEM_SIZE(i), &mem, 1);
	}
}

// Query the version register. It should read 0x04
static bool w5500_query_version(struct LeanWiznet_config *wiznet_config)
{
  uint8_t version = 0;
	w5500_spi_read(wiznet_config, W5500_CHIP_VERSION, &version, 1);
  LOG_INF("W5500 Version 0x%02x. %s", version, (version == 0x04) ? "PASS" : "FAIL");
  return version == W5500_CORRECT_VERSION_NUM;
}

// RX thread
static void w5500_rx_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint8_t ir;
	const struct LeanWiznet_config *cfg = p1;
	const struct LeanWiznet_runtime *ctx = p2;

  while (true)
  {
    int res = k_sem_take(&ctx->int_sem, K_FOREVER);
    
    if (!res) // interrupt happened
    {
      LOG_DBG("INTERRUPT HAPPENED");

      while (gpio_pin_get_dt(cfg->interrupt)) {
        /* Read interrupt */
        w5500_spi_read(cfg, W5500_S0_IR, &ir, 1);

        LOG_DBG("IR Val 0x%x", ir);

        if (ir) {
          // Clear interrupt 
          w5500_spi_write(cfg, W5500_S0_IR, &ir, 1);

          // Service
          if (ir & S0_IR_SENDOK) {
            k_sem_give(&ctx->tx_sem);
            LOG_DBG("TX Done");
          }

          if (ir & S0_IR_RECV && rxcb)
          {
            struct LeanWiznet_packet *pkt = w5500_rx(cfg, ctx); // Only do the reception if there's a callback attached
            if (pkt)
            {
              k_queue_append(&ethRxQueue, pkt);
              rxcb();
            }
            else
            {
              LOG_ERR("Bad packet rx!");
            }
            LOG_DBG("RX Done");
          }
        }
      }
    }
  }
}

// TX thread
static void w5500_tx_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	const struct LeanWiznet_config *cfg = p1;
	const struct LeanWiznet_runtime *ctx = p2;

  while (true)
  {
    struct LeanWiznet_packet *pkt = k_queue_get(&ethTxQueue, K_FOREVER);

    LOG_DBG("TX Packet %d bytes", pkt->size);
    LOG_HEXDUMP_INF(pkt->payload, pkt->size, "TX THREAD");
    w5500_tx(cfg, ctx, pkt->payload, pkt->size);
    k_free(pkt);
  }
}

// Public fns //////////////////////////////////////
int LeanWiznet_Init(void)
{
	int err;
	uint8_t rtr[2];
	const struct LeanWiznet_config *config = &leanwiznet_0_config;
	struct LeanWiznet_runtime *ctx = &leanwiznet_0_runtime;

  if (!spi_is_ready_dt(&w5500_spi_handle)) // JON TODO ON MONDAY REPLACE ALL cfg->... with w5500s 
  {
    LOG_ERR("SPI master port not ready");
    return 1;
  }

	if (!gpio_is_ready_dt(&w5500_int_handle)) {
		LOG_ERR("GPIO port %s not ready", w5500_int_handle.port->name);
		return -EINVAL;
	}

	err = gpio_pin_configure_dt(&w5500_int_handle, GPIO_INPUT);
	if (err < 0) {
		LOG_ERR("Unable to configure GPIO pin %u", w5500_int_handle.pin);
		return err;
	}

	gpio_init_callback(&(ctx->gpio_cb), w5500_gpio_callback, BIT(w5500_int_handle.pin));
	err = gpio_add_callback(w5500_int_handle.port, &(ctx->gpio_cb));
	if (err < 0) {
		LOG_ERR("Unable to add GPIO callback %u", w5500_int_handle.pin);
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(&w5500_int_handle, GPIO_INT_EDGE_FALLING);
	if (err < 0) {
		LOG_ERR("Unable to enable GPIO INT %u", w5500_int_handle.pin);
		return err;
	}

	if (w5500_rst_handle.port != NULL) {
		if (!gpio_is_ready_dt(&w5500_rst_handle)) {
			LOG_ERR("GPIO port %s not ready", w5500_rst_handle.port->name);
			return -EINVAL;
		}

		err = gpio_pin_configure_dt(&w5500_rst_handle, GPIO_OUTPUT_INACTIVE);
		if (err < 0) {
			LOG_ERR("Unable to configure GPIO pin %u", w5500_rst_handle.pin);
			return err;
		}

		/* See Section 5.5.1 of the W5500 datasheet
		 * Trc = 500us
		 * Tpl = 1ms
		 */
		gpio_pin_set_dt(&w5500_rst_handle, 1);
		k_usleep(500);
		gpio_pin_set_dt(&w5500_rst_handle, 0);
		k_msleep(1);
	}

	err = w5500_soft_reset(&leanwiznet_0_config);
	if (err != 0) {
		LOG_ERR("Reset failed");
		return err;
	}
  LOG_INF("W5500 Reset");

  bool correctVersion = w5500_query_version(&leanwiznet_0_config);

  w5500_hw_start(&leanwiznet_0_config);

	w5500_memory_configure(&leanwiznet_0_config);

	k_thread_create(&ctx->rx_thread, ctx->rx_thread_stack,
			LEAN_WIZNET_THREAD_STACK_SIZE,
			w5500_rx_thread,
			&leanwiznet_0_config, &leanwiznet_0_runtime, NULL,
			K_PRIO_COOP(LEAN_WIZNET_THREAD_PRIO),
			0, K_NO_WAIT);
	k_thread_name_set(&ctx->rx_thread, "rx_w5500");

	k_thread_create(&ctx->tx_thread, ctx->tx_thread_stack,
			LEAN_WIZNET_THREAD_STACK_SIZE,
			w5500_tx_thread,
			&leanwiznet_0_config, &leanwiznet_0_runtime, NULL,
			K_PRIO_COOP(LEAN_WIZNET_THREAD_PRIO),
			0, K_NO_WAIT);
	k_thread_name_set(&ctx->tx_thread, "tx_w5500");
	
  LOG_INF("W5500 Lean Driver Initialized");

	return 0;
}

// This callback simply notifies a consumer that a rx happened and rx queue is not empty
void LeanWiznet_SetRxCallback(RxHappenedCallback cb)
{
  rxcb = cb;
}

