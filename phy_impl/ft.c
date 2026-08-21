#include "lean_wiznet_driver.h"
#include "phy_main.h"
#include <zephyr/kernel.h>
#include <nrf_modem_dect_phy.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/gpio.h>
#include "ft.h"

LOG_MODULE_REGISTER(dect_phy_ft, LOG_LEVEL_WRN);

static void on_pdc_ft(const struct nrf_modem_dect_phy_pdc_event *evt) // TODO make this part as lean as possible. just copy over the bytes and let a thread do processing
{
  // LOG_HEXDUMP_WRN(evt->data, evt->len, "RX");
  LOG_DBG("%s", __FUNCTION__);
  if (!DectPhy_PktIsNone(evt->data))
  {
    LOG_ERR("JON GOT NONE PKT");
    LOG_DBG("%s %d handle, %d bytes", __FUNCTION__, evt->handle, evt->len);
    LOG_HEXDUMP_DBG(evt->data, evt->len, "RX");
    
    // We got a packet from the DECT connection. Enqueue it to wiznet's tx queue
    struct LeanWiznet_Packet *pkt = k_malloc(sizeof(struct LeanWiznet_Packet) + evt->len);

    if (pkt == NULL)
    {
      LOG_ERR("%s: oom, cannot malloc", __FUNCTION__);
    }
    else
    {
      memcpy(pkt->payload, evt->data, evt->len);
      pkt->size = evt->len;
      DectPhy_EnqueueEthTx(pkt);
    }
  }
  // k_sem_give(&rx_done_sem);
}

static void on_op_complete_ft(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
  int err;
  slotCounter++;

  if (evt->handle == BEACON_TX_HANDLE) // BEACON TX DONE. SCHEDULE NEXT BEACON AND SCHEDULE DOWNLINK TX
  {
    if (evt->err == 0)
    {
      err = DectPhy_Transmit(BEACON_TX_HANDLE, "BEAC", 4, modem_time + DECT_MASTER_BEACON_PERIOD_TICK); // Next beacon
      err = DectPhy_TransmitHeadOfQueue(FT_TX_HANDLE, modem_time + (1) * (2 * opTransitionLatency));

      slotCounter = 0;
      gpio_pin_toggle_dt(beaconTxSwitch);
      if (err)
      {
        LOG_ERR("Error scheduling tx %d", err);
      }

      ftState = FT_STATE_SCHEDULED_DOWNLINK;
    }
  }
  else if (evt->handle >= FT_TX_HANDLE && evt->handle < FT_RX_HANDLE) // TX GOT DONE. SCHEDULE RX
  {
    if (evt->err == 0)
    {
      if (slotCounter < knobs.ops_per_beacon)
      {
        err = DectPhy_Receive(FT_RX_HANDLE + slotCounter, 2 * DECT_SLOT_DURATION_TICK + (2 * opTransitionLatency), modem_time + (2 * opTransitionLatency));
        if (err)
        {
          LOG_ERR("Error scheduling rx %d", err);
        }

        ftState = FT_STATE_SCHEDULED_UPLINK;
      }
    }

    gpio_pin_toggle_dt(dlSwitch);
    gpio_pin_toggle_dt(dlSwitch);
    DectPhy_InFlightCompleted();
  }
  else if (evt->handle >= FT_RX_HANDLE && evt->handle < PT_TX_HANDLE) // RX GOT DONE. SCHEDULE TX
  {
    if (evt->err == 0)
    {
      gpio_pin_toggle_dt(ulSwitch);
      gpio_pin_toggle_dt(ulSwitch);
      if (slotCounter < knobs.ops_per_beacon)
      {
        err = DectPhy_TransmitHeadOfQueue(FT_TX_HANDLE + slotCounter, modem_time + (1) * (2 * opTransitionLatency));
        if (err)
        {
          LOG_ERR("Error scheduling tx %d", err);
        }

        ftState = FT_STATE_SCHEDULED_DOWNLINK;
      }
    }
  }

  if (evt->err)
  {
    LOG_ERR("op_complete %s cb time %"PRIu64" status %x handle %d slotCounter %d", !IS_RX_HANDLE(evt->handle) ? "TX" : "RX", modem_time, evt->err, evt->handle, slotCounter);
  
    ftState = FT_STATE_IDLE;
    warmedUp = false;
  }

  k_sem_give(&operation_sem);
}

// This kicks off FTs loop
static void on_time_get_ft(const struct nrf_modem_dect_phy_time_get_event *evt)
{
	LOG_DBG("time_get cb time %"PRIu64" status %x", modem_time, evt->err);
  if (!warmedUp)
  {
    int err;
    uint64_t base = modem_time + 50 * DECT_SLOT_DURATION_TICK;
    
    LOG_WRN("Modem time %llu, Base %llu, beacon tx at %llu, DL at %llu", modem_time, base, base, base + (2 * DECT_SLOT_DURATION_TICK));

    ftState = FT_STATE_SCHEDULED_BEACON;
    err = DectPhy_TransmitBeacon(base);

    if (err)
    {
      LOG_ERR("%s ERR %x", __FUNCTION__, err);
    }
  }
  warmedUp = true;
  k_sem_give(&time_sem);
}

static void on_pcc_ft(const struct nrf_modem_dect_phy_pcc_event *evt)
{
	LOG_INF("PCC Received header from device ID %d", evt->hdr.hdr_type_1.transmitter_id_hi << 8 | evt->hdr.hdr_type_1.transmitter_id_lo);
  lastPccModemTick = modem_time;
}

///////////////////////////////

static void mock_pdc(const struct nrf_modem_dect_phy_pdc_event *evt) 
{
  LOG_WRN("PDC %s @ %llu", evt->data, modem_time);
}

static void mock_pcc(const struct nrf_modem_dect_phy_pcc_event *evt) 
{
  LOG_WRN("PCC %d @ %llu", evt->header_status, modem_time);
}



// TODO move this up
uint64_t base;
uint64_t beaconScheduleOffset;
uint64_t beaconSchedule;
uint64_t beaconExpEnding;

uint64_t dlSchedule;
uint64_t dlScheduleOffset;
uint64_t dlExpEnding;

uint64_t ulSchedule;
uint64_t ulScheduleOffset;
uint64_t ulExpEnding;

uint64_t relativeUlSchedule; // relative to the tx prior
uint64_t rxDuration;

uint64_t nextDlTxModemTick;
uint64_t nextUlRxModemTick;

static volatile int testctr = DECT_OPS_PER_BEACON;

static void mock_complete(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
  int err;
  int64_t diff;

  if (evt->err) /////////////////////////////////////// MODEM ERROR //////////////////////
  {
    LOG_ERR("%s ERROR %x HANDLE %d @ MT %llu", __FUNCTION__, evt->err, evt->handle, modem_time);
    return;
  }

  
  if (evt->handle == 0)
  {
    DectPhy_TransmitHeadOfQueue(2, modem_time + DECT_SLOT_DURATION_TICK + 2 * opTransitionLatency);
  }
  if (evt->handle == 1)
  {
    DectPhy_Receive(3, DECT_SLOT_DURATION_TICK, modem_time + DECT_SLOT_DURATION_TICK + 2 * opTransitionLatency);
  }
  if (evt->handle == 2)
  {
    
  }


  LOG_WRN("OP COMPLETE @ %llu", modem_time);


  return; // TODO REMOVE

  if (evt->handle == BEACON_TX_HANDLE) /////////////////////////////////////// BEAC ///////////////////////////////////////
  {
    gpio_pin_toggle_dt(beaconTxSwitch);

    // LOG_WRN("%s @ MT: %llu HANDLE BEACON (%lli)", __FUNCTION__, modem_time, beaconExpEnding - modem_time);
  }
  else if (IS_TX_HANDLE(evt->handle)) /////////////////////////////////////// TX ///////////////////////////////////////
  {
    gpio_pin_toggle_dt(dlSwitch);

    testctr--;
    if (testctr) // OPS PER BEACON NOT DONE 
    {
// tx_activeToIdleLatency + rx_idleToActiveLatency + DECT_SLOT_DURATION_TICK + rx_activeToIdleLatency
      err = DectPhy_TransmitHeadOfQueueAndReceive(testctr, 
                                                  modem_time + genericTxScheduleOffset,
                                                  genericRelativeRxSchedule, 
                                                  genericRxDuration);
    }
    else // OPS PER BEACON DONE
    {
      return; // TODO Remove 
      testctr = DECT_OPS_PER_BEACON;
      beaconSchedule = modem_time + genericTxScheduleOffset;
      beaconExpEnding = beaconSchedule + DECT_SLOT_DURATION_TICK;
      err = DectPhy_TransmitBeacon(beaconSchedule);
      err = DectPhy_TransmitHeadOfQueueAndReceive(testctr, 
                                                  beaconSchedule + genericTxScheduleOffset + DECT_SLOT_DURATION_TICK, 
                                                  genericRelativeRxSchedule, 
                                                  genericRxDuration);
    }
    // // LOG_ERR("%s @ MT: %llu HANDLE DLTX (%lli). %llu - %llu", __FUNCTION__, modem_time, modem_time - dlExpEnding, nextDlTxModemTick, nextDlTxModemTick + DECT_SLOT_DURATION_TICK);
  }
  else if (IS_RX_HANDLE(evt->handle)) /////////////////////////////////////// RX ///////////////////////////////////////
  {
    gpio_pin_toggle_dt(ulSwitch);
    // LOG_ERR("%s @ MT: %llu HANDLE ULRX (%lli). %llu - %llu", __FUNCTION__, modem_time, diff, nextUlRxModemTick, nextUlRxModemTick + DECT_SLOT_DURATION_TICK);
  }

  k_sem_give(&operation_sem);
}

static void mock_time_get(const struct nrf_modem_dect_phy_time_get_event *evt)
{ 
  gpio_pin_toggle_dt(beaconTxSwitch);
  gpio_pin_toggle_dt(dlSwitch);
  gpio_pin_toggle_dt(ulSwitch);

  // QUESTION: What's the closest 2 txs I can put next to one another?
  // ANSWER: Once you make sure you were sending 1 slot, then its 1 slot + op trans
  // QUESTION: What about 2 rxs?
  // ANSWER: Same.
  // QUESTION: What about 1 TX 1 RX? 
  // A: FUCKING SAME
  // IMPORTANT QUESTION: What about JIT scheduling? would it now work?
  uint64_t base = modem_time + 1000 * DECT_GAP_TICK;

  uint64_t finishOffset = DECT_SLOT_DURATION_TICK + opTransitionLatency;

  uint64_t nextOffset = finishOffset;

  LOG_WRN("TEST: base %llu next %llu", base, nextOffset);
  // DectPhy_TransmitBeacon(base);
  DectPhy_TransmitHeadOfQueue(0, base);
  // DectPhy_TransmitHeadOfQueue(1, base + nextOffset);
  DectPhy_Receive(1, DECT_SLOT_DURATION_TICK, base + nextOffset);
  // DectPhy_TransmitHeadOfQueue(FT_TX_HANDLE + 1, base + 2 * nextOffset);
  // DectPhy_TransmitHeadOfQueue(FT_TX_HANDLE + 2, base + 3 * nextOffset);
  // DectPhy_TransmitHeadOfQueue(FT_TX_HANDLE + 3, base + 4 * nextOffset);
  // DectPhy_TransmitHeadOfQueue(FT_TX_HANDLE + 4, base + 5 * nextOffset);
  //
  // DectPhy_Receive(FT_RX_HANDLE+1, DECT_SLOT_DURATION_TICK, base + nextOffset);
  // DectPhy_TransmitHeadOfQueue(FT_TX_HANDLE+2, base + 2*nextOffset);
  // DectPhy_Receive(FT_RX_HANDLE+3, DECT_SLOT_DURATION_TICK, base + 3*nextOffset);
  // DectPhy_TransmitHeadOfQueue(FT_TX_HANDLE+4, base + 4*nextOffset);
  // DectPhy_Receive(FT_RX_HANDLE + 1, DECT_SLOT_DURATION_TICK, base + nextOffset);
  



  // if (!warmedUp)
  // {
  //   int err;
  //
  //   base = modem_time + 50 * DECT_SLOT_DURATION_TICK;
  //
  //   beaconScheduleOffset = DECT_MASTER_BEACON_PERIOD_TICK; // - (24 * DECT_GAP_TICK);
  //   beaconSchedule = base;
  //   beaconExpEnding = beaconSchedule + tx_idleToActiveLatency + DECT_SLOT_DURATION_TICK + (10 * DECT_GAP_TICK);
  //
  //   dlScheduleOffset = tx_activeToIdleLatency + rx_idleToActiveLatency + DECT_SLOT_DURATION_TICK + rx_activeToIdleLatency + (10 * DECT_GAP_TICK);
  //   dlSchedule = beaconExpEnding + opTransitionLatency + DECT_GUARD_TIME;
  //   dlExpEnding = dlSchedule + tx_idleToActiveLatency + DECT_SLOT_DURATION_TICK + DECT_GAP_TICK;
  //
  //   ulScheduleOffset = rx_activeToIdleLatency + tx_idleToActiveLatency + DECT_SLOT_DURATION_TICK + tx_activeToIdleLatency + (65 * DECT_GAP_TICK);
  //   ulSchedule = dlExpEnding + tx_activeToIdleLatency + rx_idleToActiveLatency;
  //   ulExpEnding = ulSchedule + DECT_SLOT_DURATION_TICK + rx_activeToIdleLatency;
  //
  //   relativeUlSchedule = opTransitionLatency + 3 * DECT_GAP_TICK;
  //
  //   rxDuration = DECT_SLOT_DURATION_TICK + 24 * DECT_GAP_TICK + DECT_GUARD_TIME;
  //
  //   dlScheduleOffset += DECT_GUARD_TIME;
  //   relativeUlSchedule += DECT_GUARD_TIME;
  //
  //   err = DectPhy_TransmitBeacon(beaconSchedule);
  //   err = DectPhy_TransmitHeadOfQueueAndReceive(testctr, dlSchedule, genericRelativeRxSchedule, genericRxDuration); // note that here and only here we use the non generic dlSchedule time. just to start things off
  // }
  // warmedUp = true;
  // k_sem_give(&time_sem);
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
  // DectPhy_Transmit(GARBAGE_HANDLE, "GARBAGE", 7, 0); // TODO For some reason, the Very first transmission contains garbage data. maybe the buffer needs flushing somehow. this does that. awful solution replace it
  // k_sem_take(&operation_sem, K_FOREVER);
 

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
