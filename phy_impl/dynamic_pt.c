#include "lean_wiznet_driver.h"
#include "phy_main.h"
#include "dynamic_pt.h"
#include <zephyr/kernel.h>
#include <nrf_modem_dect_phy.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/reboot.h>

#include <zephyr/sys/__assert.h>
LOG_MODULE_REGISTER(dect_phy_dpt, LOG_LEVEL_WRN);
// ZASSERT_MODULE(dect_phy_dpt);

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
}

static void mock_pdc(const struct nrf_modem_dect_phy_pdc_event *evt) // TODO make this part as lean as possible. just copy over the bytes and let a thread do processing
{
  int err;
  DectPacket_t *pkt = (DectPacket_t *) evt->data;
  bool isBeacon = pkt->header.isBeacon;

  // if (!isBeacon)
  {
    // LOG_WRN("RECEIVED DECT Pkt. HANDLE %d HEADER 0x%04x. PL SIZE %d. %s" , 
    //         evt->handle,
    //         * (uint16_t *) &pkt->header,
    //         pkt->header.payloadSize,
    //         pkt->header.isBeacon ? "BEACON" : "NON BEACON"
    //         );
    // if (pkt->header.payloadSize)
      // LOG_HEXDUMP_WRN(pkt->payload, pkt->header.payloadSize, "PDC");
  }

  LOG_DBG("HANDLE %d RECEIVED", evt->handle);

  static bool sw = false;

  if (pkt->header.isBeacon)
  {
    gpio_pin_toggle_dt(beaconTxSwitch);
  }
  if (IS_RX_HANDLE(evt->handle) || evt->handle == BEACON_LATCH_RX_HANDLE)
  {
    gpio_pin_toggle_dt(ptDlSwitch);
  }
  
  if (pkt->header.isBeacon)
  {
    // gpio_pin_toggle_dt(beaconTxSwitch);
    DectClusterBeaconMessage_t *beac = pkt->payload;

    __ASSERT((beac->resourceAlloc.downlink + beac->resourceAlloc.uplink) > 0, "beac->dl + ul was 0, expected > 0. dl %d ul %d", beac->resourceAlloc.downlink, beac->resourceAlloc.uplink);

    LOG_DBG("BEACON. SFN 0x%x, PERIOD 0x%x, DL %d, UL %d", beac->systemFrameNumber, beac->period.clusterBeaconPeriod, beac->resourceAlloc.downlink, beac->resourceAlloc.uplink);
    int slotsUntilNextBeacon = slotsPerClusterPeriod[beac->period.clusterBeaconPeriod] - beac->resourceAlloc.downlink;
    uint64_t dlEstimatedTimeTaken = (beac->resourceAlloc.downlink * DECT_SLOT_DURATION_TICK) + (2 * DECT_HEADROOM);
    uint64_t ulEstimatedTimeTaken = (beac->resourceAlloc.uplink * DECT_SLOT_DURATION_TICK) + (2 * DECT_HEADROOM);
    int dlUlPairsUntilBeacon = slotsPerClusterPeriod[beac->period.clusterBeaconPeriod] / (beac->resourceAlloc.downlink + beac->resourceAlloc.uplink);

    uint64_t ticksUntilNextBeacon = (dlUlPairsUntilBeacon * (dlEstimatedTimeTaken + ulEstimatedTimeTaken)) - dlEstimatedTimeTaken;
    LOG_DBG("THIS MUST MEAN THERE IS %d SLOTS UNTIL NEXT BEACON. %llu - %llu", slotsUntilNextBeacon, ticksUntilNextBeacon, ticksUntilNextBeacon + dlEstimatedTimeTaken);
    
    knobs.uplink = beac->resourceAlloc.uplink;
    knobs.downlink = beac->resourceAlloc.downlink;
    knobs.beaconPeriod = beac->period.clusterBeaconPeriod;
    knobs.slotsPerBeacon = slotsPerClusterPeriod[knobs.beaconPeriod];

    // Next Beacon
    DectScheduleItem_t schItem = (DectScheduleItem_t) {
      .action = DECT_ACTION_BEACON_RX, 
      .handle = BEACON_LATCH_RX_HANDLE, // + beac->systemFrameNumber, 
      .numSlots = knobs.downlink,
      .effectiveStartTime = modem_time + ticksUntilNextBeacon,
      .followedByLast = false,
      .headroom = DECT_HEADROOM,
    };

    k_msgq_put(&dectScheduleItemQueue, &schItem, K_NO_WAIT);

    int cnt = 0; // TODO rm
    int i = knobs.downlink;
    while (i < knobs.slotsPerBeacon)
    {
      // DOWNLINK RX
      if (i % (knobs.downlink + knobs.uplink) == 0) 
      {
        schItem = (DectScheduleItem_t) {
          .action = DECT_ACTION_DATA_RX, 
          .handle = PT_RX_HANDLE + beac->systemFrameNumber + i, 
          .numSlots = knobs.downlink,
          .followedByLast = true,
          .headroom = DECT_HEADROOM 
        };
        i += knobs.downlink;
      }
      else
      {
        // UPLINK TX 
        schItem = (DectScheduleItem_t) {
          .action = DECT_ACTION_DATA_TX, 
          .handle = PT_TX_HANDLE + beac->systemFrameNumber + i, 
          .numSlots = knobs.uplink,
          .followedByLast = true,
          .headroom = DECT_HEADROOM 
        };
        i += knobs.uplink;
      }

      // if (cnt < 1) // TODO rm
      k_msgq_put(&dectScheduleItemQueue, &schItem, K_NO_WAIT);

      cnt++;

      //   LOG_DBG("ENQUEUED ITEM. %d/%d slots left", knobs.downlink, knobs.slotsPerBeacon);
    }

    ptState = PT_STATE_WAIT_FOR_LATCH_BEACON;
    sw = true;
  }
  else
  {
    if (ptState == PT_STATE_WAIT_FOR_BEACON)
    {
      DectPhy_Receive(BEACON_RX_HANDLE, 0xffffffff, 0);
    }
  }

  if (ptState > PT_STATE_WAIT_FOR_BEACON)
  {
    // if (pkt->header.isData || pkt->header.isBeacon)
    //   LOG_HEXDUMP_WRN(evt->data, evt->len, (pkt->header.isBeacon ? "PDC + B" : "PDC"));
  }
}

static void mock_complete(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
  int err;
  LOG_DBG("HANDLE %d COMPLETE", evt->handle);
  if (evt->err) /////////////////////////////////////// MODEM ERROR //////////////////////
  {
    LOG_ERR("%s ERROR %x HANDLE %d @ MT %llu", __FUNCTION__, evt->err, evt->handle, modem_time);
  }

  if (IS_TX_HANDLE(evt->handle))
  {
    gpio_pin_toggle_dt(ptUlSwitch);
    // gpio_pin_toggle_dt(ptUlSwitch);
  }

  if (ptState >= PT_STATE_WAIT_FOR_LATCH_BEACON)
  {
    DectScheduleItem_t it = (DectScheduleItem_t) {
      .action = (IS_TX_HANDLE(evt->handle)) ? DECT_ACTION_DATA_TX : DECT_ACTION_DATA_RX,
      .followedByLast = true,
      .handle = evt->handle,
      .headroom = DECT_HEADROOM,
      .numSlots = (IS_TX_HANDLE(evt->handle)) ? knobs.uplink : knobs.downlink,
    };
    // k_msgq_put(&dectScheduleItemQueue, &it, K_NO_WAIT);
  }

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

