#include "lean_wiznet_driver.h"
#include "phy_main.h"
#include <zephyr/kernel.h>
#include <nrf_modem_dect_phy.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/gpio.h>
#include "ft.h"

LOG_MODULE_REGISTER(dect_phy_ft, LOG_LEVEL_WRN);

// FT Timing related. TODO maybe move elsewhere? 
uint64_t base;
uint64_t beaconSchedule;
uint64_t dlSchedule;
uint64_t ulSchedule;

static void on_pcc_ft(const struct nrf_modem_dect_phy_pcc_event *evt)
{
	LOG_INF("PCC Received header from device ID %d", evt->hdr.hdr_type_1.transmitter_id_hi << 8 | evt->hdr.hdr_type_1.transmitter_id_lo);
  lastPccModemTick = modem_time;
}

///////////////////////////////

static void mock_pdc(const struct nrf_modem_dect_phy_pdc_event *evt) 
{
  LOG_DBG("%s %d handle, %d bytes", __FUNCTION__, evt->handle, evt->len);
  LOG_HEXDUMP_DBG(evt->data, evt->len, "RX");
  if (!DectPhy_PktIsNone(evt->data))
  {
    DectPhy_HandleIncomingPacketFragment(evt->data, evt->len);
  }
}

static void mock_pcc(const struct nrf_modem_dect_phy_pcc_event *evt) 
{
  if (evt->header_status)
  {
    LOG_ERR("PCC %d @ %llu", evt->header_status, modem_time);
  }
  LOG_DBG("PCC %d @ %llu", evt->header_status, modem_time);
}

static void mock_complete(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
  int err;
  int64_t diff;

  if (evt->err) /////////////////////////////////////// MODEM ERROR //////////////////////
  {
    LOG_ERR("%s ERROR %x HANDLE %d @ MT %llu", __FUNCTION__, evt->err, evt->handle, modem_time);
    return;
  }

  if (evt->handle == BEACON_TX_HANDLE) /////////////////////////////////////// BEAC //
  {
    gpio_pin_toggle_dt(beaconTxSwitch);

    beaconDelta = modem_time - lastBeaconCplt; 
    lastBeaconCplt = modem_time;
  }
  else if (IS_TX_HANDLE(evt->handle)) /////////////////////////////////////// TX //
  {
    gpio_pin_toggle_dt(dlSwitch);

    slotCounter--;
    if (slotCounter) // OPS PER BEACON NOT DONE 
    {

      //     we are here. The next UL is scheduled already. We want to schedule the DL after that
      //       v
      // [DL  ]x[UL  ] [DL  ][UL  ] [DL][UL] [DL][UL] ...
      //              ^      ^
      //           nextTx    |
      //                     |
      //                  nextRx
      
      txDelta = modem_time - lastTxCplt;
      lastTxCplt = modem_time;
      dlSchedule = modem_time + opTransitionLatency + DECT_HEADROOM + DECT_SLOT_DURATION_TICK + opTransitionLatency + DECT_HALF_HEADROOM;
      ulSchedule = modem_time + opTransitionLatency + DECT_HEADROOM + DECT_SLOT_DURATION_TICK + opTransitionLatency + DECT_SLOT_DURATION_TICK + DECT_HEADROOM + opTransitionLatency;
      err |= DectPhy_TransmitHeadOfQueue(FT_TX_HANDLE + slotCounter, dlSchedule);
      err |= DectPhy_Receive(FT_RX_HANDLE + slotCounter, genericRxDuration, ulSchedule);
    }
    else // OPS PER BEACON DONE
    {
      slotCounter = DECT_OPS_PER_BEACON;
      beaconSchedule = modem_time + opTransitionLatency + DECT_HEADROOM + DECT_SLOT_DURATION_TICK + DECT_HEADROOM + opTransitionLatency;
      dlSchedule = beaconSchedule + DECT_SLOT_DURATION_TICK + DECT_HEADROOM + opTransitionLatency + DECT_HALF_HEADROOM;
      ulSchedule = beaconSchedule + DECT_SLOT_DURATION_TICK + DECT_HEADROOM + opTransitionLatency + DECT_SLOT_DURATION_TICK + DECT_HEADROOM + opTransitionLatency;

      err = DectPhy_TransmitBeacon(beaconSchedule);
      err |= DectPhy_TransmitHeadOfQueue(FT_TX_HANDLE + slotCounter, dlSchedule);
      err |= DectPhy_Receive(FT_RX_HANDLE + slotCounter, genericRxDuration, ulSchedule);
    }
    DectPhy_InFlightCompleted();
  }
  else if (IS_RX_HANDLE(evt->handle)) /////////////////////////////////////// RX ///////////////////////////////////////
  {
    gpio_pin_toggle_dt(ulSwitch);
    rxDelta = modem_time - lastRxCplt;
    lastRxCplt = modem_time;
  }

  k_sem_give(&operation_sem);
}

static void mock_time_get(const struct nrf_modem_dect_phy_time_get_event *evt)
{ 
  gpio_pin_toggle_dt(beaconTxSwitch);
  gpio_pin_toggle_dt(dlSwitch);
  gpio_pin_toggle_dt(ulSwitch);

  if (!warmedUp)
  {
    int err;
    blockTicks = DECT_SLOT_DURATION_TICK + DECT_HEADROOM;

    base = modem_time + 1000 * DECT_SLOT_DURATION_TICK;

    genericRelativeRxSchedule = opTransitionLatency; 

    dlSchedule = base + DECT_SLOT_DURATION_TICK + DECT_HEADROOM + opTransitionLatency + DECT_HALF_HEADROOM;
    ulSchedule = base + DECT_SLOT_DURATION_TICK + DECT_HEADROOM + opTransitionLatency + DECT_SLOT_DURATION_TICK + DECT_HEADROOM + opTransitionLatency; 

    genericRxDuration = DECT_SLOT_DURATION_TICK + DECT_HEADROOM;

    LOG_WRN("dlSchedule: %llu\ngenericRxDuration: %llu\nheadroom: %llu\nblock: %llu\n", dlSchedule, genericRxDuration, DECT_HEADROOM, blockTicks);

    err = DectPhy_TransmitBeacon(base);
    err |= DectPhy_TransmitHeadOfQueue(FT_TX_HANDLE + slotCounter, dlSchedule);
    err |= DectPhy_Receive(FT_RX_HANDLE + slotCounter, genericRxDuration, ulSchedule);
    
    warmedUp = true;
    k_sem_give(&time_sem);
  }
}

///////////////////////////////

void Ft_HandleEvent(const struct nrf_modem_dect_phy_event *evt)
{ 
  if (evt->id == NRF_MODEM_DECT_PHY_EVT_PDC)
  {
    mock_pdc(&evt->pdc);
    // on_pdc_ft(&evt->pdc);
  }
  else if (evt->id == NRF_MODEM_DECT_PHY_EVT_COMPLETED)
  {
    mock_complete(&evt->op_complete);
    // on_op_complete_ft(&evt->op_complete);
  }
  else if (evt->id == NRF_MODEM_DECT_PHY_EVT_TIME)
  {
    mock_time_get(&evt->time_get);
		// on_time_get_ft(&evt->time_get);
  }
  else if (evt->id == NRF_MODEM_DECT_PHY_EVT_PCC)
  {
    mock_pcc(&evt->pcc);
   // on_pcc_ft(&evt->pcc);
  }
  else
  {
    LOG_ERR("FT Unhandled event %d", evt->id);
  }
}

void Ft_Init(void)
{
  slotCounter = DECT_OPS_PER_BEACON;
}

void Ft_InfiniteLoop(void)
{
  while(true)
  {
    if (warmedUp)
    {
      k_sleep(K_MSEC(100));
    }
    else
    {
      // TODO : Currently, if an error happens we simply restart this loop of
      // time_get() -> schedule beacon -> schedule DL -> UL -> ....
      // However it doesnt have to be this way. When you have time make this better, 
      // i.e. if an error happens, simply keep in the loop, dont break out.
      // LOG_ERR("FT LOOP RESTARTING"); 
      // nrf_modem_dect_phy_time_get(); 
      
      k_sleep(K_MSEC(1000));
    }
  }
}
