#ifndef ETH_UART_H
#define ETH_UART_H

#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include <stdint.h>

#define ETH_UART_TX_PIN 17
#define ETH_UART_RX_PIN 16

#define ETH_UART_ID uart0
#define ETH_UART_BAUD_RATE 1000000 //921600 

#define ETH_UART_MAGIC 0xCEFAC4C0 //0xC0C4FACE
#define ETH_UART_MAGIC_NUM_LEN (4)

#define ETH_UART_RX_BUFFER_SIZE_BYTES (2048)
#define ETH_UART_RX_BUFFER_HALF_BYTES (ETH_UART_RX_BUFFER_SIZE_BYTES/2)

#define FRAME_RX_BUFFER_SIZE_BYTES (2048)
#define FRAME_RX_MID_PKT_TIMEOUT_US (100000)

typedef struct EthUartFrameHeader_s
{
  uint32_t magic; // coc4face
  uint16_t len; // Length of payload
  // uint8_t pl[];
} __attribute__((packed)) EthUartFrameHeader_t;

void EthUart_Init(void);
void EthUart_SendFrame(struct pbuf *p);
void EthUart_SendTest(void);
void EthUart_DmaProcess(void);
void EthUart_SetReceiveFrameCallback(void (*fn)(uint8_t *, uint16_t));
#endif
