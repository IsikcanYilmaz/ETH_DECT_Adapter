#include <stdint.h>
#include <stdbool.h>

#define MAC_ADDR_SWITCH_GPIO_PIN (28)
#define LOG_ENABLE_SWITCH_GPIO_PIN (27)

void Utils_SwInit(void);
uint8_t Utils_GetMacSw(void);
uint8_t Utils_GetLogEnSw(void);
int Utils_LedInit(void);
void Utils_SetLed(bool led_on);
void Utils_Hexdump(uint8_t *buf, uint16_t len);
