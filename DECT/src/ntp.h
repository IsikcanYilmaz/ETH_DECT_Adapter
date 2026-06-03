#ifndef NTP
#define NTP

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/drivers/hwinfo.h>
#include <string.h>

#include "dect_phy.h"
#include "dect_phy_events.h"

#define TAG_NTP_REQ 1
#define TAG_NTP_RPL 2

#define NTP_ERROR 1
/** 
 * DB receives at T2 and sends T2 and T3 back at T3.
 * DA receives T2, T3 at time T4 and calculates offset.
 * Time sync can be done while communicating with a device through TDMA, but first sync needs button press.
 * Dev 1 needs to store T1 to avoid retransmission.
 * Since NTP is happening parallel to communication it does not need internal timeouts etc.
 * Since Time sync will have an error, reapeating time sync is needed.
 */

/* Modem time when NTP init was sent */
//uint64_t ntp_init_time; 
typedef struct {
    uint8_t tag;
    uint64_t t1;
    uint64_t t2;
} ntp;

/* Will be count down : OVERFLOW handling needed */
//uint64_t countdown;

//int ntp_init(struct ntp ntp_pkg);

#endif 