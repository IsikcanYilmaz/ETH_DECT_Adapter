#ifndef LED_H
#define LED_H

#define TX_LED_ID (0)
#define RX_LED_ID (1)

#define MASTER_LED_ID (2)
#define SLAVE_LED_ID (3)

int led_init(void);
int led_set(int lednum, bool on);
#endif

