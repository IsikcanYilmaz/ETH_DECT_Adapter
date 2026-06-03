#include <stdint.h>
#include <stdbool.h>

#define SWITCH_GPIO_PIN (28)

void Utils_SwInit(void);
uint8_t Utils_GetSw(void);
int Utils_LedInit(void);
void Utils_SetLed(bool led_on);
void Utils_Hexdump(uint8_t *buf, uint16_t len);
