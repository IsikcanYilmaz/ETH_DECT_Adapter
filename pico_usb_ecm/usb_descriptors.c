#include "tusb.h"
#include "device/usbd.h"
#include "hardware/gpio.h"
#include <string.h>

#define USBD_VID 0xCafe
#define USBD_PID 0x4002   // bumped so the host re-enumerates cleanly

// TODO generalize
static char mac_address_string_1[] = "CAFE00000001";
static char mac_address_string_2[] = "CAFE00000002";

// ---- Device descriptor ----
tusb_desc_device_t const desc_device = {
  .bLength            = sizeof(tusb_desc_device_t),
  .bDescriptorType    = TUSB_DESC_DEVICE,
  .bcdUSB             = 0x0200,
  .bDeviceClass       = TUSB_CLASS_MISC,
  .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
  .bDeviceProtocol    = MISC_PROTOCOL_IAD,
  .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
  .idVendor           = USBD_VID,
  .idProduct          = USBD_PID,
  .bcdDevice          = 0x0101,
  .iManufacturer      = 0x01,
  .iProduct           = 0x02,
  .iSerialNumber      = 0x03,
  .bNumConfigurations = 0x01
};

uint8_t const * tud_descriptor_device_cb(void) {
  return (uint8_t const *) &desc_device;
}

// ---- Interface numbers ----
enum {
  ITF_NUM_ECM = 0,        // ECM control
  ITF_NUM_ECM_DATA,       // ECM data
  ITF_NUM_CDC_CTRL,       // CDC-ACM control
  ITF_NUM_CDC_DATA,       // CDC-ACM data
  ITF_NUM_TOTAL
};

// ---- Endpoints (all unique) ----
#define EPNUM_NET_NOTIF     0x81
#define EPNUM_NET_OUT       0x02
#define EPNUM_NET_IN        0x82

#define EPNUM_CDC_NOTIF     0x83
#define EPNUM_CDC_OUT       0x04
#define EPNUM_CDC_IN        0x84

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN \
  + TUD_CDC_ECM_DESC_LEN \
  + TUD_CDC_DESC_LEN)

uint8_t const desc_configuration[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),

  // CDC-ECM (network)
  TUD_CDC_ECM_DESCRIPTOR(ITF_NUM_ECM, 4, 5,
                         EPNUM_NET_NOTIF, 64,
                         EPNUM_NET_OUT, EPNUM_NET_IN, 64,
                         CFG_TUD_NET_MTU),

  // CDC-ACM (debug serial)
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_CTRL, 6,
                     EPNUM_CDC_NOTIF, 8,
                     EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const * tud_descriptor_configuration_cb(uint8_t index) {
  (void) index;
  return desc_configuration;
}

// ---- String descriptors ----
char const* string_desc_arr[] = {
  (const char[]) { 0x09, 0x04 },   // 0: English (US)
  "Raspberry Pi",                  // 1: Manufacturer
  "Pico ECM + Debug",              // 2: Product
  "123456",                        // 3: Serial
  "ECM Interface",                 // 4: ECM interface label
  mac_address_string_1,            // 5: MAC address
  "Pico Debug Console",            // 6: CDC-ACM interface label
};

static uint16_t _desc_str[32];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void) langid;
  uint8_t chr_count;

  // If GPIO 18 is high, increment the last byte of the MAC addr
  uint8_t sw = Utils_GetMacSw();
  if (sw)
  {
    string_desc_arr[5] = mac_address_string_2;
  }

  if (index == 0) {
    memcpy(&_desc_str[1], string_desc_arr[0], 2);
    chr_count = 1;
  } else {
    if (index >= sizeof(string_desc_arr)/sizeof(string_desc_arr[0])) return NULL;
    const char *str = string_desc_arr[index];
    chr_count = strlen(str);
    if (chr_count > 31) chr_count = 31;
    for (uint8_t i = 0; i < chr_count; i++) {
      _desc_str[1+i] = str[i];
    }
  }
  _desc_str[0] = (TUSB_DESC_STRING << 8) | (2*chr_count + 2);
  return _desc_str;
}

