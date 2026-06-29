#include "eth_uart.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include "utils.h"
#include "debug_log.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "pico/time.h"
#include <string.h>

static EthUartFrameHeader_t header = { .magic = ETH_UART_MAGIC };

static uint8_t ethUartRxBuffer[ETH_UART_RX_BUFFER_SIZE_BYTES];
static uint16_t ethUartRxHead = 0; // You read from here
static uint16_t ethUartRxTail = 0; // DMA writes here

static int dmaChannelA; // Fills RX buffer[0:half-1]
static int dmaChannelB; // Fills RX buffer[half:full-1]

typedef enum 
{
  MAGIC0,
  MAGIC1,
  MAGIC2,
  MAGIC3,
  GET_LENGTH0,
  GET_LENGTH1,
  COPY_BYTES,
  FRAME_RECEIVED
} ReceiveFrameState_e;

// Frame reception state machine stuff
static ReceiveFrameState_e frameReceiveState = MAGIC0;
static uint32_t frameReceiveStateTimeout = 0;
static uint16_t frameLength = 0;
static uint16_t frameByteIndex = 0;
static uint8_t frameReceiveBuffer[FRAME_RX_BUFFER_SIZE_BYTES]; // This is only the payload. header added by the picos wont be here

static void (*EthUart_FrameReceivedCallback)(uint8_t *, uint16_t) = NULL;

// Static fns
static void EthUart_FrameReceiveCheckTimeout(void)
{
  if (frameReceiveStateTimeout && time_reached(frameReceiveStateTimeout))
  {
    WARN("%lu PACKET RECEIVE TIMED OUT!!!", frameReceiveStateTimeout);
    WARN("EXPECTED %d BYTES. RECEIVED %d", frameLength, frameByteIndex);
    Utils_Hexdump(frameReceiveBuffer,frameLength);
    memset(frameReceiveBuffer, 0x00, FRAME_RX_BUFFER_SIZE_BYTES);
    ethUartRxHead = ethUartRxTail;
    frameLength = 0;
    frameByteIndex = 0;
    frameReceiveState = MAGIC0;
    frameReceiveStateTimeout = 0;
  }
}

// Receive frame state machine. first, byte by byte goes thru the magic number. if and only if each byte of the magic
// number matches, then it picks up the next 2 bytes as the frame size and then copies over that many bytes as 
// the phayload
// TODO implement timeout scheme that takes us back to the first state
static void EthUart_ReceiveFrame(void)
{
  DBG("(%lu) FRAME %d - %d vvvvvvvv", time_us_32(), ethUartRxHead, ethUartRxTail);

  while(ethUartRxHead != ethUartRxTail)
  {
    // debug_log_printf("0x%02x ", ethUartRxBuffer[ethUartRxHead]);
    uint8_t currUartRxByte = ethUartRxBuffer[ethUartRxHead];
    switch(frameReceiveState)
    {
      case MAGIC0:
      {
        DBG("RECEIVE STATE M0 ");
        if (currUartRxByte == 0xC0) // TODO make these generic
        {
          frameReceiveState++;

          // Also start the timer
          frameReceiveStateTimeout = make_timeout_time_us(FRAME_RX_MID_PKT_TIMEOUT_US);
        }
        break;
      }
      case MAGIC1:
      {
        DBG("RECEIVE STATE M1 ");
        if (currUartRxByte == 0xC4) // TODO make these generic
        {
          frameReceiveState++;
          frameReceiveStateTimeout = make_timeout_time_us(FRAME_RX_MID_PKT_TIMEOUT_US);
        }
        else
        {
          frameReceiveState = MAGIC0;
        }
        break;
      }
      case MAGIC2:
      {
        DBG("RECEIVE STATE M2 ");
        if (currUartRxByte == 0xFA) // TODO make these generic
        {
          frameReceiveState++;
          frameReceiveStateTimeout = make_timeout_time_us(FRAME_RX_MID_PKT_TIMEOUT_US);
        }
        else
        {
          frameReceiveState = MAGIC0;
        }
        break;
      }
      case MAGIC3:
      {
        DBG("RECEIVE STATE M3 ");
        if (currUartRxByte == 0xCE) // TODO make these generic
        {
          frameReceiveState++;
          frameReceiveStateTimeout = make_timeout_time_us(FRAME_RX_MID_PKT_TIMEOUT_US);
        }
        else
        {
          frameReceiveState = MAGIC0;
        }
        break;
      }
      case GET_LENGTH0:
      {
        frameLength |= currUartRxByte; 
        frameReceiveState++;
        frameReceiveStateTimeout = make_timeout_time_us(FRAME_RX_MID_PKT_TIMEOUT_US);
        break;
      }
      case GET_LENGTH1:
      {
        frameLength |= currUartRxByte << 8; 
        frameReceiveState++;
        frameReceiveStateTimeout = make_timeout_time_us(FRAME_RX_MID_PKT_TIMEOUT_US);
        DBG("LENGTH RECEIVED %d 0x%04x", frameLength, frameLength);
        break;
      }
      case COPY_BYTES:
      {
        // TODO we can memcpy here but need to be mindful about the end of the uart rx ring buffer
        // DBG("COPYING %d th byte of the frame", frameByteIndex);
        frameReceiveBuffer[frameByteIndex] = currUartRxByte;
        frameByteIndex++;
        frameReceiveStateTimeout = make_timeout_time_us(FRAME_RX_MID_PKT_TIMEOUT_US);
        if (frameByteIndex == frameLength)
        {
          frameReceiveState++;
          // FALL THRU
        }
        else
        {
          break;
        }
      }
      case FRAME_RECEIVED:
      {
        INFO("DECT->PICO FRAME_RECEIVED %d BYTES", frameByteIndex+1);
        if (EthUart_FrameReceivedCallback)
        {
          EthUart_FrameReceivedCallback(frameReceiveBuffer, frameLength);
        }
        frameReceiveState = MAGIC0;
        frameByteIndex = 0;
        frameReceiveStateTimeout = 0;
        frameLength = 0;
        break;
      }
      default:
      {
        DBG("RECEIVE STATE DEFAULT %d", frameReceiveState);
        frameReceiveState = MAGIC0;
        frameByteIndex = 0;
        frameReceiveStateTimeout = 0;
        frameLength = 0;
      }
    }
  
    ethUartRxHead = (ethUartRxHead + 1) % ETH_UART_RX_BUFFER_SIZE_BYTES;
  }
  // debug_log_printf("\n");
  DBG("FRAME PROCESS END %d %d STATE %d ^^^^^^^^", ethUartRxHead, ethUartRxTail, frameReceiveState);
}

static void EthUart_DmaIrqHandler(void)
{
  // Channel A finished writing the first half
  if (dma_channel_get_irq0_status(dmaChannelA))
  {
    dma_channel_acknowledge_irq0(dmaChannelA);
    ethUartRxTail = ETH_UART_RX_BUFFER_HALF_BYTES;
    dma_channel_set_write_addr(dmaChannelA, ethUartRxBuffer, false);
    dma_channel_set_trans_count(dmaChannelA, ETH_UART_RX_BUFFER_HALF_BYTES, false);
    // debug_log_printf("%s first half\n", __FUNCTION__);
    // newBytesAvailable = true;
  }

  // // Channel B finished writing the second half
  if (dma_channel_get_irq0_status(dmaChannelB))
  {
    dma_channel_acknowledge_irq0(dmaChannelB);
    ethUartRxTail = 0; // wrap
    dma_channel_set_write_addr(dmaChannelB, ethUartRxBuffer + ETH_UART_RX_BUFFER_HALF_BYTES, false);
    dma_channel_set_trans_count(dmaChannelB, ETH_UART_RX_BUFFER_HALF_BYTES, false);
    // debug_log_printf("%s second half\n", __FUNCTION__);
    // newBytesAvailable = true;
  }
}

// Public fns
// This fn takes a network packet to be relayed, puts on the header (magic number + length) 
// and sends it to the DECT board over UART
void EthUart_SendFrame(struct pbuf *p)
{
  // TODO DMA based TX 
  INFO("PICO->DECT SENDING %d bytes", p->len);

  // First write the header 
  header.len = p->len;
  uart_write_blocking(ETH_UART_ID, (const uint8_t *) &header, sizeof(EthUartFrameHeader_t));

  // Then write the payload
  uart_write_blocking(ETH_UART_ID, p->payload, p->len);
}

void EthUart_SendTest(void)
{
  // char *buf = "AAAAAAAABBBBBBBB";
  char buf[] = {0xC0, 0xC4, 0xFA, 0xCE, 0x05, 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
  //            [       magic         ][   len    ][  payload                  ]
  // char buf[] = {0xCE, 0xFA, 0xC3, 0xC0, 0x05, 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}; // Bad packet test
  uart_write_blocking(ETH_UART_ID, buf, 11);
  INFO("SENDING TEST STRING");
}

void EthUart_Init(void)
{
  memset(ethUartRxBuffer, 0x00, ETH_UART_RX_BUFFER_SIZE_BYTES);
  memset(frameReceiveBuffer, 0x00, FRAME_RX_BUFFER_SIZE_BYTES);

  // Init uart
  uart_init(ETH_UART_ID, ETH_UART_BAUD_RATE);
  gpio_set_function(ETH_UART_TX_PIN, UART_FUNCSEL_NUM(ETH_UART_ID, ETH_UART_TX_PIN));
  gpio_set_function(ETH_UART_RX_PIN, UART_FUNCSEL_NUM(ETH_UART_ID, ETH_UART_RX_PIN));
  uart_set_hw_flow(ETH_UART_ID, false, false); // CTS/RTS not needed

  // Set our data format
  // uart_set_format(UART_ID, DATA_BITS, STOP_BITS, PARITY);

  // Turn off FIFO's - we want to do this character by character
  uart_set_fifo_enabled(ETH_UART_ID, true);
  
  // Flush uart because its writing a byte into the buffer upon init
  while(uart_is_readable(ETH_UART_ID))
  {
    uart_getc(ETH_UART_ID);
  }

  // Init dma channels //////////////////////////////////////////////

  // Set up a RX interrupt
  // We need to set up the handler first
  // Select correct interrupt for the UART we are using
  int ETH_UART_IRQ = (ETH_UART_ID == uart0) ? UART0_IRQ : UART1_IRQ;
  
  // And set up and enable the interrupt handlers. we want UARTRTINTR, its a receive timeout interrupt
  // uart_set_irq_enables(ETH_UART_ID, true, false);
  // hw_set_bits(&uart_get_hw(ETH_UART_ID)->imsc,
  //                 UART_UARTIMSC_RTIM_BITS // enable rt
  //                 );
  // irq_set_exclusive_handler(ETH_UART_IRQ, EthUart_IrqHandler);
  // irq_set_enabled(ETH_UART_IRQ, true);
  // uart_set_irq_enables(ETH_UART_ID, false, false);

  // Now enable the UART to send interrupts - RX only
  // uart_set_irq_enables(ETH_UART_ID, true, false);

  dmaChannelA = dma_claim_unused_channel(true);
  dmaChannelB = dma_claim_unused_channel(true);

  // Configure channel a
  dma_channel_config cfg_a = dma_channel_get_default_config(dmaChannelA);
  channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_8);
  channel_config_set_read_increment(&cfg_a, false);
  channel_config_set_write_increment(&cfg_a, true);
  // Pace transfers to the UART RX DREQ so DMA waits for each byte
  channel_config_set_dreq(&cfg_a, uart_get_dreq(ETH_UART_ID, false));
  channel_config_set_irq_quiet(&cfg_a, false);
  // when done automatically start channel B
  channel_config_set_chain_to(&cfg_a, dmaChannelB);
  dma_channel_configure(
    dmaChannelA,
    &cfg_a,
    ethUartRxBuffer,
    &uart_get_hw(ETH_UART_ID)->dr,
    ETH_UART_RX_BUFFER_HALF_BYTES,
    false
  );

  // Configure channel b
  dma_channel_config cfg_b = dma_channel_get_default_config(dmaChannelB);
  channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_8);
  channel_config_set_read_increment(&cfg_b, false);
  channel_config_set_write_increment(&cfg_b, true);
  // Pace transfers to the UART RX DREQ so DMA waits for each byte
  channel_config_set_dreq(&cfg_b, uart_get_dreq(ETH_UART_ID, false));
  channel_config_set_irq_quiet(&cfg_b, false);
  // when done automatically start channel B
  channel_config_set_chain_to(&cfg_b, dmaChannelA);
  dma_channel_configure(
    dmaChannelB,
    &cfg_b,
    ethUartRxBuffer + ETH_UART_RX_BUFFER_HALF_BYTES,
    &uart_get_hw(ETH_UART_ID)->dr,
    ETH_UART_RX_BUFFER_HALF_BYTES,
    false
  );

  // Enable irq for both channels so we can track write_pos
  dma_channel_set_irq0_enabled(dmaChannelA, true);
  dma_channel_set_irq0_enabled(dmaChannelB, true);
  irq_set_exclusive_handler(DMA_IRQ_0, EthUart_DmaIrqHandler);
  // irq_set_exclusive_handler(DMA_IRQ_1, EthUart_DmaIrqHandler);
  irq_set_enabled(DMA_IRQ_0, true);

  // Start channel A. chain takes care of the rest
  dma_channel_start(dmaChannelA);

  // Utils_Hexdump(ethUartRxBuffer,ETH_UART_RX_BUFFER_SIZE_BYTES);
  INFO("INITED DMA");
} 

// This callback gets called upon a frame reception
void EthUart_SetReceiveFrameCallback(void (*fn)(uint8_t *, uint16_t))
{
  EthUart_FrameReceivedCallback = fn;
}

// Poll. Not the best
void EthUart_DmaProcess(void)
{
  uint32_t *addr = NULL; // write addr of dma
  
  if (dma_channel_is_busy(dmaChannelA))
  {
    addr = (uint32_t *) dma_channel_hw_addr(dmaChannelA)->write_addr;
  }
  else if (dma_channel_is_busy(dmaChannelB))
  {
    addr = (uint32_t *) dma_channel_hw_addr(dmaChannelB)->write_addr;
  }

  ethUartRxTail = (uint32_t) addr - (uint32_t) ethUartRxBuffer;

  EthUart_FrameReceiveCheckTimeout();

  if (ethUartRxTail != ethUartRxHead)
  {
    // INFO("WRITE ADDR %08x %08x %08x", ethUartRxBuffer, ethUartRxHead, ethUartRxTail);
    EthUart_ReceiveFrame();
  }
  else 
  {
    return; 
  }
}
