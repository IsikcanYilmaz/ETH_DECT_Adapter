#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#define CFG_TUSB_MCU            OPT_MCU_RP2040
#define CFG_TUSB_OS             OPT_OS_PICO
#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE

#define CFG_TUD_ENDPOINT0_SIZE  64

// Classes
#define CFG_TUD_CDC             1      // <-- NEW: CDC-ACM debug serial
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0
#define CFG_TUD_ECM_RNDIS       1      // CDC-ECM network
#define CFG_TUD_NCM             0

// CDC FIFO sizes
#define CFG_TUD_CDC_RX_BUFSIZE  256
#define CFG_TUD_CDC_TX_BUFSIZE  512
#define CFG_TUD_CDC_EP_BUFSIZE  64

// Network buffers
#define CFG_TUD_NET_ENDPOINT_SIZE      64
#define CFG_TUD_NET_MTU                1514

#endif

