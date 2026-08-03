#ifndef FT_H_
#define FT_H_

#include <nrf_modem_dect_phy.h>

void Ft_Init(void);
void Ft_HandleEvent(const struct nrf_modem_dect_phy_event *evt);
void Ft_InfiniteLoop(void);

#endif
