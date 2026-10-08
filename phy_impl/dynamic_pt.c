#include "lean_wiznet_driver.h"
#include "phy_main.h"
#include "dynamic_pt.h"
#include <zephyr/kernel.h>
#include <nrf_modem_dect_phy.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(dect_phy_dpt, LOG_LEVEL_WRN);

static void on_time_get_pt(const struct nrf_modem_dect_phy_time_get_event *evt)
{
  warmedUp = true;
  DectPhy_Receive(BEACON_RX_HANDLE, 0xffffffff, 0);
  ptState = PT_STATE_WAIT_FOR_BEACON;
  LOG_WRN("WAITING FOR FIRST BEACON");
  k_sem_give(&time_sem);
}

static void mock_pcc(const struct nrf_modem_dect_phy_pcc_event *evt)
{
  int err;
  if (evt->header_status)
  {
    LOG_ERR("PCC %d @ %llu", evt->header_status, modem_time);
  }
}

static void mock_pdc(const struct nrf_modem_dect_phy_pdc_event *evt) // TODO make this part as lean as possible. just copy over the bytes and let a thread do processing
{
  int err;
  DectPacket_t *pkt = (DectPacket_t *) evt->data;
  bool isBeacon = pkt->header.isBeacon;
  // LOG_WRN("RECEIVED DECT Pkt. HANDLE %d HEADER 0x%04x. PL SIZE %d. %s" , 
  //         evt->handle,
  //         * (uint16_t *) &pkt->header,
  //         pkt->header.payloadSize,
  //         pkt->header.isBeacon ? "BEACON" : "NON BEACON"
  //         );
  // LOG_HEXDUMP_WRN(evt->data, evt->len, "PDC");

  LOG_WRN("HANDLE %d RECEIVED", evt->handle);

  static bool sw = false;
  
  if (pkt->header.isBeacon)
  {
    gpio_pin_toggle_dt(beaconTxSwitch);

    DectClusterBeaconMessage_t *beac = pkt->payload;
    LOG_WRN("BEACON. SFN 0x%x, PERIOD 0x%x, DL %d, UL %d", beac->systemFrameNumber, beac->period.clusterBeaconPeriod, beac->resourceAlloc.downlink, beac->resourceAlloc.uplink);
    int slotsUntilNextBeacon = slotsPerClusterPeriod[beac->period.clusterBeaconPeriod] - beac->resourceAlloc.downlink;
    uint64_t dlEstimatedTimeTaken = opTransitionLatency + ((beac->resourceAlloc.downlink * DECT_SLOT_DURATION_TICK) + tx_activeToIdleLatency + dectScheduleOffset);
    uint64_t ulEstimatedTimeTaken = opTransitionLatency + ((beac->resourceAlloc.downlink * DECT_SLOT_DURATION_TICK) + rx_activeToIdleLatency + dectScheduleOffset);
    int dlUlPairsUntilBeacon = slotsPerClusterPeriod[beac->period.clusterBeaconPeriod] / (beac->resourceAlloc.downlink + beac->resourceAlloc.uplink);

    uint64_t ticksUntilNextBeacon = (dlUlPairsUntilBeacon * (dlEstimatedTimeTaken + ulEstimatedTimeTaken)) - dlEstimatedTimeTaken;
    LOG_WRN("THIS MUST MEAN THERE IS %d SLOTS UNTIL NEXT BEACON. %llu", slotsUntilNextBeacon, ticksUntilNextBeacon);
    
    // DectPhy_Receive(BEACON_RX_HANDLE + beac->systemFrameNumber, DECT_SLOT_DURATION_TICK + DECT_HALF_HEADROOM, modem_time + ticksUntilNextBeacon);
    
    DectScheduleItem_t schItem = (DectScheduleItem_t) {
      .action = DECT_ACTION_DATA_RX, 
      .handle = BEACON_RX_HANDLE + beac->systemFrameNumber, 
      .numSlots = beac->resourceAlloc.downlink,
      .startTime = modem_time + ticksUntilNextBeacon,
      .expectedEndTime = modem_time + ticksUntilNextBeacon + DECT_SLOT_DURATION_TICK,
      .headroom = DECT_HALF_HEADROOM
    };

    k_msgq_put(&dectScheduleItemQueue, &schItem, K_NO_WAIT);

    LOG_WRN("ENQUEUED ITEM");

    sw = true;

  }
  else
  {
    if (sw)
      return;
    DectPhy_Receive(BEACON_RX_HANDLE, 0xffffffff, 0);
  }
}

static void mock_complete(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
  int err;
  LOG_WRN("HANDLE %d COMPLETE", evt->handle);
  if (evt->err) /////////////////////////////////////// MODEM ERROR //////////////////////
  {
    LOG_ERR("%s ERROR %x HANDLE %d @ MT %llu", __FUNCTION__, evt->err, evt->handle, modem_time);
  }
  // DectPhy_Receive(BEACON_RX_HANDLE, 0xffffffff, 0);
  k_sem_give(&operation_sem);
}

static void print_pdc(const struct nrf_modem_dect_phy_pdc_event *evt) // TODO make this part as lean as possible. just copy over the bytes and let a thread do processing
{
  LOG_WRN("PRINT PDC DATA LEN %d %s state %d", evt->len, evt->data, ptState);
}

// Currently we care for two kinds of events:
// Either we receive a packet and a PDC event happens
// or we do a transmit/receive and it completes and a EVT_COMPLETED happens
void DynamicPt_HandleEvent(const struct nrf_modem_dect_phy_event *evt)
{
  if (evt->id == NRF_MODEM_DECT_PHY_EVT_PDC)
  {
    mock_pdc(&evt->pdc);
  }
  else if (evt->id == NRF_MODEM_DECT_PHY_EVT_PCC)
  {
    mock_pcc(&evt->pcc);
  }
  else if (evt->id == NRF_MODEM_DECT_PHY_EVT_COMPLETED)
  {
    mock_complete(&evt->op_complete);
  }
  else if (evt->id == NRF_MODEM_DECT_PHY_EVT_TIME)
  {
		on_time_get_pt(&evt->time_get);
  }
  else
  {
    LOG_ERR("PT Unhandled event %d", evt->id);
  }
}

void DynamicPt_Init(void)
{
}

void DynamicPt_InfiniteLoop(void)
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

