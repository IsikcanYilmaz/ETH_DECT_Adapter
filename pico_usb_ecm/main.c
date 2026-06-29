#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"
#include "debug_log.h"
#include "pico/cyw43_arch.h"
#include "eth_uart.h"
#include "utils.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "tusb.h"

#define WATCHDOG_PERIOD_MS 10000

uint8_t tud_network_mac_address[6] = {0xCA, 0xFE, 0x00, 0x00, 0x00, 0x01};

static struct netif netif_data;

static const ip4_addr_t ipaddr  = IPADDR4_INIT_BYTES(192, 168, 7, 1);
static const ip4_addr_t netmask = IPADDR4_INIT_BYTES(255, 255, 255, 0);
static const ip4_addr_t gateway = IPADDR4_INIT_BYTES(0, 0, 0, 0);

static struct pbuf *received_frame;

static err_t linkoutput_fn(struct netif *netif, struct pbuf *p) {
  (void) netif;
  for (;;) {
    if (!tud_ready()) return ERR_USE;
    if (tud_network_can_xmit(p->tot_len)) {
      tud_network_xmit(p, 0);
      return ERR_OK;
    }
    tud_task();
  }
}

static err_t output_fn(struct netif *netif, struct pbuf *p, const ip4_addr_t *addr) {
  return etharp_output(netif, p, addr);
}

static err_t netif_init_cb(struct netif *netif) {
  netif->mtu = CFG_TUD_NET_MTU;
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
  netif->name[0] = 'u';
  netif->name[1] = '0';
  netif->linkoutput = linkoutput_fn;
  netif->output = output_fn;
  return ERR_OK;
}

bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
  DBG("%s size: %d", __FUNCTION__, received_frame, size);
  
  if (received_frame) 
  {
    ERR("received while frame buffer not free!!!");
    return false; // if there's already a frame in the buffer, drop it
  }

  if (size) { // this block latches the frame into our buffer
    struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
    if (p) {
      memcpy(p->payload, src, size);
      received_frame = p;
      // Utils_SetLed(true);
    }
  }
  return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t len) {
  DBG("%s", __FUNCTION__);
  memcpy(dst, ref, len); // We need to put our bytes into tinyusbs buffer
  return len;
}

void tud_network_init_cb(void) {
  INFO("%s", __FUNCTION__);
  if (received_frame) {
    pbuf_free(received_frame);
    received_frame = NULL;
  }
}

const uint8_t tud_network_mac_address_storage[6] = {0xCA, 0xFE, 0x00, 0x00, 0x00, 0x01};

// ---- Optional: react to the host opening/closing the debug serial port ----
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) 
{
  (void) itf; (void) rts;
  if (dtr) {
    // Host opened the port — say hi
    INFO("Debug console connected");
  }
}

static void usb_to_pico(void)
{
  if (received_frame) {
    INFO("USB->PICO RECEIVED FRAME %d bytes", received_frame->len);
    // err_t err = ethernet_input(received_frame, &netif_data); // this calls pbuf_free
    // Utils_Hexdump(received_frame->payload, received_frame->len);
    Utils_SetLed(true);
    EthUart_SendFrame(received_frame);
    pbuf_free(received_frame);
    received_frame = NULL;
    Utils_SetLed(false);
    tud_network_recv_renew();
  }
  sys_check_timeouts();
}

static void test_usb_to_pico(void)
{
  const int testbuflen = 98;
  char testbuf[testbuflen];
  for (int i = 0; i < testbuflen; i++)
  {
    testbuf[i] = i;
  }
  struct pbuf testpbuf = {.payload = (void *) testbuf, .len = testbuflen};
  Utils_SetLed(true);
  EthUart_SendFrame(&testpbuf);
  Utils_SetLed(false);
}

void pico_to_usb(uint8_t *buf, uint16_t len)
{
  INFO("PICO->USB RECEIVED FRAME %d bytes", len);
  // Utils_Hexdump(buf, len);
  if (tud_network_can_xmit(len))
  {
    tud_network_xmit(buf, len);
  }
}

int main(void) {
  Utils_LedInit();
  stdio_init_all();
  lwip_init();
  EthUart_Init();
  EthUart_SetReceiveFrameCallback(pico_to_usb);
  Utils_SwInit();

  uint8_t logSw = Utils_GetLogEnSw();
  if (logSw)
  {
    debug_log_init();
  }

  // If gpio 28 (MAC_ADDR_SWITCH_GPIO_PIN) is high, we increment the last number ofg the mac addr
  uint8_t macSw = Utils_GetMacSw();
  INFO("SWITCH %d", macSw);
  if (macSw)
  {
    tud_network_mac_address[5] = 0x02;   
  }

  tusb_init();
  tud_network_recv_renew(); 

  netif_add(&netif_data, &ipaddr, &netmask, &gateway, NULL, netif_init_cb, ethernet_input);
  netif_data.hwaddr_len = 6;
  memcpy(netif_data.hwaddr, tud_network_mac_address, 6);
  netif_data.hwaddr[5] ^= 0x01;
  netif_set_default(&netif_data);

  tud_task();

  INFO("Pico ECM + Debug firmware starting");
  INFO("Network: 192.168.7.1/24 over CDC-ECM");

  if (watchdog_enable_caused_reboot())
  {
    ERR("REBOOT DUE TO WATCHDOG!!!");
  }

  debug_log_task();

  watchdog_enable(WATCHDOG_PERIOD_MS, 0);

  uint32_t tick = 0;
  absolute_time_t next_heartbeat = make_timeout_time_ms(1000);
  absolute_time_t next_test = make_timeout_time_ms(5000);
  while (true) {
    tud_task();
    debug_log_task();
    usb_to_pico(); // TODO if we could do this without polling thatd be great
    // if (time_reached(next_test))
    // {
    //   test_usb_to_pico();
    //   next_test = make_timeout_time_ms(5000);
    // }
    EthUart_DmaProcess(); // todo would be better if we could fire this from a irq
    watchdog_update();
    tight_loop_contents();
  }
  return 0;
}

