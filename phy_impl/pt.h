#ifndef PT_H_
#define PT_H_

#include <nrf_modem_dect_phy.h>

void Pt_Init(void);
void Pt_HandleEvent(const struct nrf_modem_dect_phy_event *evt);
void Pt_InfiniteLoop(void);

#endif
