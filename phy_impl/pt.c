#include "lean_wiznet_driver.h"
#include "phy_main.h"
#include "pt.h"
#include <zephyr/kernel.h>
#include <nrf_modem_dect_phy.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/gpio.h>

LOG_MODULE_REGISTER(dect_phy_pt, LOG_LEVEL_WRN);

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
}

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
      static char firstbuf[8] = {'f', 'i', 'r', 's', 't', 0x00, 0x00, 0x00};
      static char secondbuf[8] = {'s', 'e', 'c', 'o', 'n', 'd', 0x00, 0x00};

      beacon_time = modem_time;

      // We were waiting for a beacon and have received one. schedule 1 DLRX 1 ULTX
      DectBeaconMessage_t *beac = evt->data;
      uint32_t next_beacon_offset = beac->modem_ticks_until_next_beacon;
      uint64_t next_beacon_tick = modem_time + next_beacon_offset;
      next_beacon_tick -= (DECT_SLOT_DURATION_TICK); // JON TODO MAGIC NUMBER!!!! FIGURE OUT WHY THIS WORKED AND REMOVE ITTTTTTTT
      // next_beacon_tick -= (DECT_HALF_SLOT_DURATION_TICK); // JON TODO MAGIC NUMBER!!!! FIGURE OUT WHY THIS WORKED AND REMOVE ITTTTTTTT
      slotCounter = beac->ops_per_beacon;

      err = DectPhy_Receive(BEACON_LATCH_RX_HANDLE, DECT_SLOT_DURATION_TICK, next_beacon_tick);
      err = DectPhy_Receive(PT_RX_HANDLE + slotCounter, DECT_SLOT_DURATION_TICK, next_beacon_tick + DECT_SLOT_DURATION_TICK + opTransitionLatency + DECT_HEADROOM);
      err = DectPhy_Transmit(PT_TX_HANDLE + slotCounter, firstbuf, 8, next_beacon_tick + 2 * (DECT_SLOT_DURATION_TICK + opTransitionLatency + DECT_HEADROOM));

      slotCounter--;
      
      // err = DectPhy_ReceiveContinuous(PT_RX_HANDLE + 99, US_TO_MODEM_TICKS(1000000), next_beacon_tick + 4 * (DECT_SLOT_DURATION_TICK + opTransitionLatency));

      err = DectPhy_Receive(PT_RX_HANDLE + slotCounter, DECT_SLOT_DURATION_TICK, next_beacon_tick + 3 * (DECT_SLOT_DURATION_TICK + opTransitionLatency + DECT_HEADROOM));
      err = DectPhy_Transmit(PT_TX_HANDLE + slotCounter, secondbuf, 8, next_beacon_tick + 4 * (DECT_SLOT_DURATION_TICK + opTransitionLatency + DECT_HEADROOM));
      
      LOG_WRN("@ %llu FIRST BEACON RECEIVED. %d BYTES. %d OPS PER BEACON", modem_time, evt->len, slotCounter);
      LOG_WRN("EXPECTING LATCH BECAON IN %d TICKS, @ %llu", next_beacon_offset, modem_time + next_beacon_offset);
      LOG_HEXDUMP_WRN(evt->data, 16, "BEACON");
      LOG_WRN("NOW %llu LATCH SCH %llu DL SCH %llu UL SCH %llu", modem_time, next_beacon_tick, next_beacon_tick + DECT_SLOT_DURATION_TICK + opTransitionLatency + DECT_HEADROOM, next_beacon_tick + 2 * (DECT_SLOT_DURATION_TICK + opTransitionLatency + DECT_HEADROOM));

      gpio_pin_toggle_dt(beaconRxSwitch);
      ptState = PT_STATE_WAIT_FOR_LATCH_BEACON;

      expected_latch_beacon_time = next_beacon_tick;
    }
    else
    {
      err = DectPhy_Receive(BEACON_RX_HANDLE, US_TO_MODEM_TICKS(1000000000), 0); // Unexpected non beacon
      LOG_DBG("NON BEACON", err); 
    }
  }
  else if (ptState == PT_STATE_WAIT_FOR_LATCH_BEACON)
  {
    if (DectPhy_PktIsBeacon(evt->data))
    {
      LOG_WRN("LATCH BEACON RECEIVED @ %llu. PREV BEACON %llu DIFF %llu. EXPECTED @ %llu, EXPECTED DIFF %llu (- %llu)", modem_time, beacon_time, modem_time - beacon_time, expected_latch_beacon_time, modem_time - expected_latch_beacon_time, DECT_SLOT_DURATION_TICK);
      latch_beacon_time = modem_time;
      ptState = PT_STATE_SCHEDULED_DOWNLINK;
      gpio_pin_toggle_dt(beaconRxSwitch);
    }
    else 
    {
      // err = DectPhy_Receive(BEACON_RX_HANDLE, US_TO_MODEM_TICKS(1000000000), 0); // Unexpected non beacon // TODO maybe just do this in the op_complete part
      // ptState = PT_STATE_WAIT_FOR_BEACON;
      LOG_ERR("LATCH BEACON MISSED. GOING BACK TO PT_STATE_WAIT_FOR_BEACON");
    }
  }
  else if (ptState == PT_STATE_SCHEDULED_DOWNLINK)
  {
    // slotCounter--;
    // if (slotCounter)
    // {
    //   uint64_t nextRx = modem_time + DECT_SLOT_DURATION_TICK + 2 * opTransitionLatency;
    //   uint64_t nextTx = nextRx + DECT_SLOT_DURATION_TICK + opTransitionLatency;
    //   err = DectPhy_Receive(PT_RX_HANDLE + slotCounter, DECT_SLOT_DURATION_TICK, nextRx);
    //   // err = DectPhy_TransmitHeadOfQueue(PT_TX_HANDLE + slotCounter, nextTx);
    //   err = DectPhy_Transmit(PT_TX_HANDLE + slotCounter, "SECOND", 6, nextTx);
    //   LOG_WRN("@ %llu Scheduled rx to %llu and tx to %llu. slot %d", modem_time, nextRx, nextTx, slotCounter);
    // }
    // gpio_pin_toggle_dt(ptDlSwitch);
    // ptState = PT_STATE_SCHEDULED_UPLINK; // TODO UNCOMMENT AFTER TESTING
    ptState = PT_STATE_FRAME_DONE;
    LOG_WRN("DOWNLINK DATA RECEIVED @ %llu from handle %d", modem_time, evt->handle);
    LOG_HEXDUMP_WRN(evt->data, 16, "DL");
  }

  // if (evt->handle == PT_RX_HANDLE+99) // TODO TEST CODE REMOVE
  // {
  //   LOG_HEXDUMP_WRN(evt->data, 16, "RX");
  //   gpio_pin_toggle_dt(ptDlSwitch);
  // }
}

static void mock_complete(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
  int err;

  if (evt->err)
  {
    LOG_ERR("op_complete %s cb time %"PRIu64" status %x handle %d ", !IS_RX_HANDLE(evt->handle) ? "TX" : "RX", modem_time, evt->err, evt->handle);
  }

  LOG_WRN("JON %d COMPLETE", evt->handle);
  // if (evt->handle == BEACON_LATCH_RX_HANDLE)
  // {
  //   gpio_pin_toggle_dt(beaconRxSwitch);
  // }

  if (ptState != PT_STATE_WAIT_FOR_BEACON && ptState != PT_STATE_WAIT_FOR_LATCH_BEACON)
  {
    if (IS_TX_HANDLE(evt->handle))
    {
      gpio_pin_toggle_dt(ptUlSwitch);
    }
    if (IS_RX_HANDLE(evt->handle))
    {
      // gpio_pin_toggle_dt(ptDlSwitch);
    }
  }
  

  if (evt->handle == BEACON_LATCH_RX_HANDLE && ptState == PT_STATE_WAIT_FOR_LATCH_BEACON) ////////// WE MISSED THE LATCH BEACON! //////////////
  {
    // err = DectPhy_Receive(BEACON_RX_HANDLE, US_TO_MODEM_TICKS(1000000000), 0); // GET BEACON 
    LOG_ERR("PT_STATE_WAIT_FOR_LATCH_BEACON TIMED OUT! FALLING BACK TO WAIT_FOR_BEACON");
    ptState = PT_STATE_WAIT_FOR_BEACON; 
    gpio_pin_toggle_dt(beaconRxSwitch);
    // k_sem_give(&done_sem);
  }

  if (ptState == PT_STATE_SCHEDULED_UPLINK && IS_TX_HANDLE(evt->handle)) ////////// 
  {
    gpio_pin_toggle_dt(ptUlSwitch);
    ptState = PT_STATE_SCHEDULED_DOWNLINK;
    // k_sem_give(&done_sem);
  }

  if (ptState == PT_STATE_SCHEDULED_DOWNLINK && IS_RX_HANDLE(evt->handle))
  {
    gpio_pin_toggle_dt(ptDlSwitch);
    LOG_ERR("%d DOWNLINK SLOT COULDNT RECEIVE DATA", evt->handle);
  }

  if (ptState == PT_STATE_TEST)
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

