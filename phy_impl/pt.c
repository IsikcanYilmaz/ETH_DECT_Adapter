#include "lean_wiznet_driver.h"
#include "phy_main.h"
#include "pt.h"
#include <zephyr/kernel.h>
#include <nrf_modem_dect_phy.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/gpio.h>

LOG_MODULE_REGISTER(dect_phy_pt, LOG_LEVEL_ERR);

static void on_time_get_pt(const struct nrf_modem_dect_phy_time_get_event *evt)
{
  int err;
	LOG_DBG("time_get cb time %"PRIu64" status %x", modem_time, evt->err);
  blockTicks = DECT_SLOT_DURATION_TICK + opTransitionLatency + DECT_HEADROOM;

  genericTxScheduleOffset = DECT_SLOT_DURATION_TICK + opTransitionLatency + DECT_HEADROOM;
  genericRelativeRxSchedule = opTransitionLatency;
  genericRxDuration = DECT_SLOT_DURATION_TICK + DECT_HEADROOM;

  LOG_WRN("genericTxScheduleOffset: %llu\ngenericRxDuration: %llu\nheadroom: %llu\nblock: %llu", genericTxScheduleOffset, genericRxDuration, DECT_HEADROOM, blockTicks);

  warmedUp = true;
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

static char firstbuf[8] = {'f', 'i', 'r', 's', 't', 0x00, 0x00, 0x00};
static char secondbuf[8] = {'s', 'e', 'c', 'o', 'n', 'd', 0x00, 0x00};
static void mock_pdc(const struct nrf_modem_dect_phy_pdc_event *evt) // TODO make this part as lean as possible. just copy over the bytes and let a thread do processing
{
  int err;

  static uint64_t beacon_time;
  static uint64_t latch_beacon_time;
  static uint64_t expected_latch_beacon_time;

  if (ptState == PT_STATE_WAIT_FOR_BEACON)
  {
    if (DectPhy_PktIsBeacon(evt->data)) 
    {
      beacon_time = modem_time;

      // We were waiting for a beacon and have received one. schedule 1 DLRX 1 ULTX
      DectBeaconMessage_t *beac = evt->data;
      uint32_t next_beacon_offset = beac->modem_ticks_until_next_beacon;
      uint64_t next_beacon_tick = modem_time + next_beacon_offset;
      next_beacon_tick -= (DECT_SLOT_DURATION_TICK); // JON TODO MAGIC NUMBER!!!! FIGURE OUT WHY THIS WORKED AND REMOVE ITTTTTTTT
      // next_beacon_tick -= (DECT_HALF_SLOT_DURATION_TICK); // JON TODO MAGIC NUMBER!!!! FIGURE OUT WHY THIS WORKED AND REMOVE ITTTTTTTT
      numSlotsInFrame = beac->ops_per_beacon;
      slotCounter = numSlotsInFrame;

      uint64_t dlRxDuration = DECT_SLOT_DURATION_TICK + DECT_HEADROOM;
      uint64_t dlRxStart = next_beacon_tick + opTransitionLatency + dlRxDuration + opTransitionLatency;
      uint64_t ulTxStart = dlRxStart + dlRxDuration + opTransitionLatency + (3*DECT_QUART_HEADROOM);

      firstbuf[5] = slotCounter;
      err = DectPhy_Receive(BEACON_LATCH_RX_HANDLE, dlRxDuration, next_beacon_tick); // SUCCESSFULLY RECEIVES
      err = DectPhy_Receive(PT_RX_HANDLE + slotCounter, dlRxDuration, dlRxStart); // SUCCESSFULLY RECEIVES 
      err = DectPhy_TransmitHeadOfQueue(PT_TX_HANDLE + slotCounter, ulTxStart); // SUCCESSFULLY TRANSMITS

      LOG_WRN("@ %llu FIRST BEACON RECEIVED. %d BYTES. %d OPS PER BEACON", modem_time, evt->len, slotCounter);
      LOG_WRN("EXPECTING LATCH BECAON IN %d TICKS, @ %llu", next_beacon_offset, modem_time + next_beacon_offset);
      LOG_HEXDUMP_WRN(evt->data, 16, "FIRST BEACON");
      LOG_WRN("NOW %llu LATCH SCH %llu DL SCH %llu UL SCH %llu", modem_time, next_beacon_tick, next_beacon_tick + DECT_SLOT_DURATION_TICK + opTransitionLatency + DECT_HEADROOM, next_beacon_tick + 2 * (DECT_SLOT_DURATION_TICK + opTransitionLatency + DECT_HEADROOM));

      gpio_pin_toggle_dt(beaconRxSwitch);
      ptState = PT_STATE_WAIT_FOR_LATCH_BEACON; 
      // ptState = PT_STATE_TEST;

      expected_latch_beacon_time = next_beacon_tick;
    }
    else
    {
      err = DectPhy_Receive(BEACON_RX_HANDLE, US_TO_MODEM_TICKS(1000000000), 0); // Unexpected non beacon
      LOG_DBG("NON BEACON", err); 
    }
  }
  else if (ptState == PT_STATE_TEST)
  {
    LOG_WRN("TEST HANDLE %d", evt->handle);
    LOG_HEXDUMP_WRN(evt->data, 16, "TEST");
  }
  else if (ptState == PT_STATE_WAIT_FOR_LATCH_BEACON)
  {
    if (DectPhy_PktIsBeacon(evt->data))
    {
      LOG_WRN("LATCH BEACON RECEIVED @ %llu", modem_time);
      latch_beacon_time = modem_time;
      ptState = PT_STATE_SCHEDULED_DOWNLINK;
      // ptState = PT_STATE_TEST;
      gpio_pin_toggle_dt(beaconRxSwitch);
    }
    else 
    {
      // TODO Should also cancel all modem ops here
      err = DectPhy_Receive(BEACON_RX_HANDLE, US_TO_MODEM_TICKS(1000000000), 0); // Unexpected non beacon // TODO maybe just do this in the op_complete part
      ptState = PT_STATE_WAIT_FOR_BEACON;
      LOG_ERR("LATCH BEACON MISSED. GOING BACK TO PT_STATE_WAIT_FOR_BEACON");
    }
  }
  else if (ptState == PT_STATE_SCHEDULED_DOWNLINK)
  {
    slotCounter--;
    uint64_t dlRxDuration = DECT_SLOT_DURATION_TICK + DECT_HEADROOM;
    uint64_t ulTxDuration = DECT_SLOT_DURATION_TICK;
    if (slotCounter)
    { 

      //     we are here
      //       v
      // [DL  ]x[UL  ] [DL  ][UL  ] [DL][UL] [DL][UL] ...
      //              ^      ^
      //           nextRx    |
      //                     |
      //                  nextTx

      uint64_t nextRx = modem_time + opTransitionLatency + dlRxDuration + opTransitionLatency;
      uint64_t nextTx = nextRx + dlRxDuration + opTransitionLatency + (3*DECT_QUART_HEADROOM);

      err = DectPhy_Receive(PT_RX_HANDLE + slotCounter, dlRxDuration, nextRx);
      err = DectPhy_TransmitHeadOfQueue(PT_TX_HANDLE + slotCounter, nextTx);

      LOG_DBG("@ %llu Scheduled rx to %llu and tx to %llu. slot %d. PCC-PDC DIFF %llu", modem_time, nextRx, nextTx, slotCounter, lastPdcModemTick - lastPccModemTick);

      ptState = PT_STATE_SCHEDULED_UPLINK; 
    }
    else
    {

      //     we are here, at the end of the frame
      //       v   
      // [DL  ]x[UL  ]|[B   ][DL  ][UL  ] ....
      //              ^      ^     ^
      //           nextBeaconRx    |
      //                     |     |
      //                  nextRx   nextTx

      uint64_t next_beacon_tick = modem_time + opTransitionLatency + dlRxDuration + opTransitionLatency; 
      uint64_t nextRx = next_beacon_tick + dlRxDuration + opTransitionLatency + (DECT_HEADROOM);
      uint64_t nextTx = nextRx + dlRxDuration + opTransitionLatency + (3*DECT_QUART_HEADROOM);

      err = DectPhy_Receive(BEACON_LATCH_RX_HANDLE, dlRxDuration, next_beacon_tick); // SUCCESSFULLY RECEIVES
      err = DectPhy_Receive(PT_RX_HANDLE + slotCounter, dlRxDuration, nextRx);
      err = DectPhy_TransmitHeadOfQueue(PT_TX_HANDLE + slotCounter, nextTx);

      LOG_WRN("EXPECTING LATCH @ %llu DL @ %llu UL @ %llu", next_beacon_tick, nextRx, nextTx);
      
      ptState = PT_STATE_SCHEDULED_UPLINK; 
    }

    if (!DectPhy_PktIsNone(evt->data))
    {
      LOG_DBG("PT RECEIVED %d BYTES FROM FT IN SLOT %d", evt->len, slotCounter);
      LOG_HEXDUMP_DBG(evt->data, evt->len, "RX");

      // We got a packet from the DECT connection. Enqueue it to wiznet's tx queue // TODO reduce code dupes
      struct LeanWiznet_Packet *pkt = k_malloc(sizeof(struct LeanWiznet_Packet) + evt->len);
      if (pkt == NULL)
      {
        LOG_ERR("%s:%d out of memory! cannot malloc %d bytes", __FUNCTION__, __LINE__, evt->len + sizeof(struct LeanWiznet_Packet));
      }
      else
    {
        memcpy(pkt->payload, evt->data, evt->len);
        pkt->size = evt->len;
        DectPhy_EnqueueEthTx(pkt);
      }
    }

    gpio_pin_toggle_dt(ptDlSwitch);
    // LOG_WRN("DOWNLINK DATA RECEIVED @ %llu from handle %d", modem_time, evt->handle);
    LOG_HEXDUMP_WRN(evt->data, 16, "DL");
  }
}

static void mock_complete(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
  int err;

  if (evt->err)
  {
    LOG_ERR("op_complete %s cb time %"PRIu64" status %x handle %d ", !IS_RX_HANDLE(evt->handle) ? "TX" : "RX", modem_time, evt->err, evt->handle);
  }

  if(ptState == PT_STATE_TEST)
  {
    LOG_WRN("JON %d COMPLETE", evt->handle);
  }
  // if (evt->handle == BEACON_LATCH_RX_HANDLE)
  // {
  //   gpio_pin_toggle_dt(beaconRxSwitch);
  // }

  if (evt->handle == BEACON_LATCH_RX_HANDLE && ptState == PT_STATE_WAIT_FOR_LATCH_BEACON) // WE MISSED THE LATCH BEACON! //////////////
  {
    err = DectPhy_Receive(BEACON_RX_HANDLE, US_TO_MODEM_TICKS(1000000000), 0); // GET BEACON 
    LOG_ERR("PT_STATE_WAIT_FOR_LATCH_BEACON TIMED OUT! FALLING BACK TO WAIT_FOR_BEACON");
    ptState = PT_STATE_WAIT_FOR_BEACON; 
    gpio_pin_toggle_dt(beaconRxSwitch);
    // k_sem_give(&done_sem);
  }

  if (ptState == PT_STATE_SCHEDULED_UPLINK && IS_TX_HANDLE(evt->handle)) // ULTx DONE. EITHER KEEP GOING, OR THIS IS THE END OF A FRAME
  {
    gpio_pin_toggle_dt(ptUlSwitch);
    DectPhy_InFlightCompleted();
    if (slotCounter)
    {
      ptState = PT_STATE_SCHEDULED_DOWNLINK;
    }
    else
    {
      LOG_WRN("LAST UPLINK IN FRAME DONE");
      ptState = PT_STATE_WAIT_FOR_LATCH_BEACON;
      slotCounter = numSlotsInFrame;
    }
    // k_sem_give(&done_sem);
  }

  if (ptState == PT_STATE_SCHEDULED_DOWNLINK && IS_RX_HANDLE(evt->handle))
  {
    gpio_pin_toggle_dt(ptDlSwitch);
    LOG_ERR("%d DOWNLINK SLOT COULDNT RECEIVE DATA", evt->handle);
  }

  if (ptState == PT_STATE_TEST) // TODO remove
  {
    k_sem_give(&done_sem);
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
void Pt_HandleEvent(const struct nrf_modem_dect_phy_event *evt)
{
  if (evt->id == NRF_MODEM_DECT_PHY_EVT_PDC)
  {
    // on_pdc_pt(&evt->pdc);
    mock_pdc(&evt->pdc);
    // print_pdc(&evt->pdc);
    // LOG_ERR("PDC @ MT %llu. pcc delta %llu", modem_time, lastPdcModemTick - lastPccModemTick); // TODO REMOVE
  }
  else if (evt->id == NRF_MODEM_DECT_PHY_EVT_PCC)
  {
    // on_pcc_pt(&evt->pcc);
    mock_pcc(&evt->pcc);
    // LOG_ERR("PCC @ MT %llu", modem_time); // TODO REMOVE
  }
  else if (evt->id == NRF_MODEM_DECT_PHY_EVT_COMPLETED)
  {
    // on_op_complete_pt(&evt->op_complete);
    mock_complete(&evt->op_complete);
    // LOG_ERR("EVT CMP @ MT %llu handle %d", modem_time, &evt->op_complete.handle); // TODO REMOVE 
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

void Pt_Init(void)
{
}

void Pt_InfiniteLoop(void)
{
  while(true)
  { 
    if (!warmedUp)
    {
      k_sem_take(&time_sem, K_FOREVER);
    }

    ptState = PT_STATE_WAIT_FOR_BEACON;
    
    int err = 0;

    LOG_ERR("PT LOOP BEGIN");

    err = DectPhy_Receive(BEACON_RX_HANDLE, US_TO_MODEM_TICKS(1000000000), 0); // GET FAKE BEACON
    // DectPhy_ReceiveContinuous(PT_RX_HANDLE, US_TO_MODEM_TICKS(100000000), 0); // Get first beacon

    k_sem_take(&done_sem, K_FOREVER); // spin here forever unless an error happens // todo bad design
    
    LOG_ERR("DONE");
    while(true) // goodnight sweet prince
    {
      k_sleep(K_MSEC(1000));
    }
  }
}

