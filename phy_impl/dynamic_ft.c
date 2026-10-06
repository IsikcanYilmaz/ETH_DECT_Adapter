#include "lean_wiznet_driver.h"
#include "phy_main.h"
#include <zephyr/kernel.h>
#include <nrf_modem_dect_phy.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/gpio.h>
#include "dynamic_ft.h"

LOG_MODULE_REGISTER(dect_phy_dft, LOG_LEVEL_WRN);

static void on_pcc_ft(const struct nrf_modem_dect_phy_pcc_event *evt)
{
	LOG_INF("PCC Received header from device ID %d", evt->hdr.hdr_type_1.transmitter_id_hi << 8 | evt->hdr.hdr_type_1.transmitter_id_lo);
  lastPccModemTick = modem_time;
}

///////////////////////////////

static void on_pdc(const struct nrf_modem_dect_phy_pdc_event *evt) 
{
  LOG_DBG("%s %d handle, %d bytes", __FUNCTION__, evt->handle, evt->len);
  LOG_HEXDUMP_DBG(evt->data, evt->len, "RX");
  if (!DectPhy_PktIsNone(evt->data))
  {
    // DectPhy_HandleIncomingPacketFragment(evt->data, evt->len);
    DectPhy_UnpackFrameAndProcessSDUs(evt->data, evt->len);
  }
}

static void on_pcc(const struct nrf_modem_dect_phy_pcc_event *evt) 
{
  if (evt->header_status)
  {
    LOG_ERR("PCC %d @ %llu", evt->header_status, modem_time);
  }
  LOG_DBG("PCC %d @ %llu", evt->header_status, modem_time);
}

static uint64_t lastOpEnding; // TODO move up
static void on_complete(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
  if (evt->err) /////////////////////////////////////// MODEM ERROR //////////////////////
  {
    LOG_ERR("%s ERROR %x HANDLE %d @ MT %llu", __FUNCTION__, evt->err, evt->handle, modem_time);
    return;
  }

  DectScheduleItem_t item;
  // k_msgq_get(&dectInFlightItemQueue, &item, K_NO_WAIT); // TODO Check for errors on these
  // LOG_WRN("ON COMPLETE. HANDLE %d (%d). @ %llu expected %llu. diff %lli %s", evt->handle, item.handle, modem_time, item.expectedEndTime, (int64_t)(item.expectedEndTime - modem_time), (modem_time > item.expectedEndTime) ? "EXPECTED EARLIER" : "EXPECTED LATER");

  LOG_DBG("ON COMPLETE @ %llu. HANDLE %d ", modem_time, evt->handle);

  if (evt->handle == BEACON_TX_HANDLE)
  {
    static bool sw = false;
    gpio_pin_toggle_dt(beaconTxSwitch);
    if (sw) // TODO TEST remove
      return;
    item.action = DECT_ACTION_BEACON_TX;
    item.followedByLast = true;
    item.numSlots = 1;
    k_msgq_put(&dectScheduleItemQueue, &item, K_NO_WAIT);
    // sw = true;
  }
  else if (IS_TX_HANDLE(evt->handle))
  {
    gpio_pin_toggle_dt(dlSwitch);
    item.action = DECT_ACTION_DATA_TX;
    item.followedByLast = true;
    item.numSlots = 1;
    item.handle = evt->handle;
    k_msgq_put(&dectScheduleItemQueue, &item, K_NO_WAIT);
  } 
  else if (IS_RX_HANDLE(evt->handle))
  {
    gpio_pin_toggle_dt(ulSwitch);
    item.action = DECT_ACTION_DATA_RX;
    item.followedByLast = true;
    item.numSlots = 1;
    item.handle = evt->handle;
    k_msgq_put(&dectScheduleItemQueue, &item, K_NO_WAIT);
  }

  k_sem_give(&operation_sem);
}


static void on_time_get(const struct nrf_modem_dect_phy_time_get_event *evt)
{ 
  gpio_pin_toggle_dt(beaconTxSwitch);
  gpio_pin_toggle_dt(dlSwitch);
  gpio_pin_toggle_dt(ulSwitch);

  if (!warmedUp)
  {
    uint64_t base = modem_time + US_TO_MODEM_TICKS(3000000);
    uint64_t start = base;
    uint64_t end = base + DECT_SLOT_DURATION_TICK + tx_activeToIdleLatency + dectScheduleOffset;
    uint8_t numSlots = 1;
    DectScheduleItem_t item;

    // item = (DectScheduleItem_t) {.action = DECT_ACTION_BEACON_TX, .numSlots = numSlots, .startTime = start, .expectedEndTime = end};
    // k_msgq_put(&dectScheduleItemQueue, &item, K_NO_WAIT); // first DL
    //
    // // Schedule the first frame right away
    // for (int i = 0; i < knobs.ops_per_beacon - 1; i++)
    // {
    //   if (i % 2 == 0) 
    //   {
    //     numSlots = 1;
    //     start = end + opTransitionLatency;
    //     end = start + (numSlots * DECT_SLOT_DURATION_TICK) + rx_activeToIdleLatency + dectScheduleOffset;
    //     item = (DectScheduleItem_t) {.action = DECT_ACTION_DATA_RX, .handle = FT_RX_HANDLE + i, .numSlots = numSlots, .startTime = start, .expectedEndTime = end};
    //     k_msgq_put(&dectScheduleItemQueue, &item, K_NO_WAIT);
    //   }
    //   else 
    //   {
    //     numSlots = 1;
    //     start = end + opTransitionLatency;
    //     end = start + (numSlots * DECT_SLOT_DURATION_TICK) + tx_activeToIdleLatency + dectScheduleOffset;
    //     item = (DectScheduleItem_t) {.action = DECT_ACTION_DATA_TX, .handle = FT_TX_HANDLE + i, .numSlots = numSlots, .startTime = start, .expectedEndTime = end};
    //     k_msgq_put(&dectScheduleItemQueue, &item, K_NO_WAIT);
    //   }
    // }

    // Lets rethink
    // numSlotsPerBeaconPeriod = (DL * DL Slots + UL * UL Slots)
    int numSlotsPerBeaconPeriod = 

    LOG_WRN("%llu", modem_time);

    LOG_WRN("lastOpEnding %llu", lastOpEnding);

    warmedUp = true;
    LOG_ERR("FT LOOP BEGIN");
    k_sem_give(&time_sem);
  }
}

///////////////////////////////

void DynamicFt_HandleEvent(const struct nrf_modem_dect_phy_event *evt)
{ 
  if (evt->id == NRF_MODEM_DECT_PHY_EVT_PDC)
  {
    // on_pdc(&evt->pdc);
  }
  else if (evt->id == NRF_MODEM_DECT_PHY_EVT_COMPLETED)
  {
    on_complete(&evt->op_complete);
  }
  else if (evt->id == NRF_MODEM_DECT_PHY_EVT_TIME)
  {
    on_time_get(&evt->time_get);
  }
  else if (evt->id == NRF_MODEM_DECT_PHY_EVT_PCC)
  {
    // on_pcc(&evt->pcc);
  }
  else
  {
    LOG_ERR("FT Unhandled event %d", evt->id);
  }
}

void DynamicFt_Init(void)
{
  slotCounter = DECT_OPS_PER_BEACON;
}

void DynamicFt_InfiniteLoop(void)
{
  while(true)
  {
    if (warmedUp)
    {
      k_sleep(K_MSEC(100));
    }
    else
    {
      k_sleep(K_MSEC(1000));
    }
  }
}
