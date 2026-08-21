#include "lean_wiznet_driver.h"
#include "phy_main.h"
#include "pt.h"
#include <zephyr/kernel.h>
#include <nrf_modem_dect_phy.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/gpio.h>

LOG_MODULE_REGISTER(dect_phy_pt, LOG_LEVEL_WRN);

static void on_pdc_pt(const struct nrf_modem_dect_phy_pdc_event *evt) // TODO make this part as lean as possible. just copy over the bytes and let a thread do processing
{
  int err;
  switch(ptState)
  {
    case PT_STATE_WAIT_FOR_BEACON: // BEACON RECEIVED
      {
        // We received data while waiting for the beacon. Check if it is a beacon. If so, kick off the current frame's states
        if (DectPhy_PktIsBeacon((char *) evt->data))
        {
          err = DectPhy_Receive(PT_RX_HANDLE + slotCounter, 100 * DECT_SLOT_DURATION_TICK + 2 * opTransitionLatency, 0);
          if (err)
          {
            LOG_ERR("%s ERROR SCHEDULING RECEIVE", err);
            break;
          }
          lastBeaconModemTick = modem_time;
          lastBeaconTs = k_uptime_get();
          gpio_pin_toggle_dt(beaconRxSwitch);
          slotCounter = 1;
          ptState = PT_STATE_SCHEDULED_DOWNLINK;
          LOG_DBG("BEACON RECEIVED. STATE -> SCHEDULED DOWNLINK");
        }
        else
        {
          LOG_DBG("NON BEACON DURING PT WAITING FOR BEACON");
        }
        break;
      }
    case PT_STATE_SCHEDULED_DOWNLINK: // DOWNLINK TX RECEIVED
      {
        err = DectPhy_TransmitHeadOfQueue(PT_TX_HANDLE, modem_time + 2 * opTransitionLatency);
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
        slotCounter++;
        gpio_pin_toggle_dt(ptDlSwitch);
        ptState = PT_STATE_SCHEDULED_UPLINK;
        break;
      }
    case PT_STATE_SCHEDULED_UPLINK:
      {
        // err = receive(pt_rx_handle + slotCounter, 4 * DECT_SLOT_DURATION_TICK, 0);
        // slotCounter++;
        // PtState = (slotCounter < 6) ? PT_STATE_SCHEDULED_DOWNLINK : PT_STATE_WAIT_FOR_BEACON;
        // gpio_pin_toggle_dt(ulSwitch);
        break;
      }
    case PT_STATE_FRAME_DONE:
      {
        break;
      }
    default:
    break;
  }

  // k_sem_give(&rx_done_sem);
}

static void on_op_complete_pt(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
  // LOG_WRN("%s", __FUNCTION__);
  
  if (evt->handle == PT_TX_HANDLE) // Previous Uplink slot ended. schedule next downlink slot
  {
    int err = DectPhy_Receive(PT_RX_HANDLE + slotCounter, 100 * DECT_SLOT_DURATION_TICK + 2 * opTransitionLatency, 0); // schedule next rx slot // REACTIVE
    slotCounter++;

    if (slotCounter < knobs.ops_per_beacon)
    {
      ptState = PT_STATE_SCHEDULED_DOWNLINK;
    }
    else 
    {
      ptState = PT_STATE_WAIT_FOR_BEACON;
    }

    // gpio_pin_toggle_dt(ptUlSwitch);
    // gpio_pin_toggle_dt(ptUlSwitch);
    DectPhy_InFlightCompleted();
  }

  k_sem_give(&operation_sem);
}

static void on_time_get_pt(const struct nrf_modem_dect_phy_time_get_event *evt)
{
	LOG_DBG("time_get cb time %"PRIu64" status %x", modem_time, evt->err);
  warmedUp = true;
  k_sem_give(&time_sem);
}

static void on_pcc_pt(const struct nrf_modem_dect_phy_pcc_event *evt)
{
	LOG_INF("PCC Received header from device ID %d", evt->hdr.hdr_type_1.transmitter_id_hi << 8 | evt->hdr.hdr_type_1.transmitter_id_lo);
}



static void mock_pcc(const struct nrf_modem_dect_phy_pcc_event *evt)
{
  // if (ptState == PT_STATE_WAIT_FOR_BEACON)
  // {
  //   // int err = DectPhy_Receive(PT_RX_HANDLE + slotCounter, 
  //   //                           10 * DECT_SLOT_DURATION_TICK + 2 * opTransitionLatency, 
  //   //                           modem_time + (2 * opTransitionLatency) + (1 * DECT_SLOT_DURATION_TICK)); 
  // }
  // else if (ptState == PT_STATE_SCHEDULED_DOWNLINK)
  // {
  //   int err = DectPhy_TransmitHeadOfQueue(PT_TX_HANDLE + slotCounter, modem_time + (2 * opTransitionLatency) + (2 * DECT_SLOT_DURATION_TICK)); 
  //   // ptState = PT_STATE_SCHEDULED_UPLINK;
  //   // gpio_pin_toggle_dt(dlSwitch);
  // }
  // else if (ptState == PT_STATE_SCHEDULED_UPLINK)
  // {
  //
  // }



  if (ptState == PT_STATE_SCHEDULED_DOWNLINK) // Expecting downlink. 
  {
    
  }
}

static char test[4];
static volatile int testctr = DECT_OPS_PER_BEACON; 

static void mock_pdc(const struct nrf_modem_dect_phy_pdc_event *evt) // TODO make this part as lean as possible. just copy over the bytes and let a thread do processing
{
  int err;
  if (ptState == PT_STATE_WAIT_FOR_BEACON)
  {
    if (DectPhy_PktIsBeacon(evt->data)) // We were waiting for a beacon and have received one. schedule 1 DLRX 1 ULTX
    {
      DectBeaconMessage_t *beac = evt->data;
      uint16_t ops_per_beacon = beac->ops_per_beacon;
      uint64_t next_beacon_tick = modem_time + ((2 * ops_per_beacon + 1) * opTransitionLatency) + (2 * ops_per_beacon * DECT_SLOT_DURATION_TICK) ;
      uint64_t next_dl_tick = next_beacon_tick + DECT_SLOT_DURATION_TICK + opTransitionLatency;
      uint64_t next_ul_tick = next_dl_tick + DECT_SLOT_DURATION_TICK + opTransitionLatency;
      
      /*
       * we should latch to the next beacon
       */

      err = DectPhy_Receive(BEACON_LATCH_RX_HANDLE, DECT_SLOT_DURATION_TICK, next_beacon_tick);
      err = DectPhy_Receive(PT_RX_HANDLE, DECT_SLOT_DURATION_TICK, next_dl_tick);
      gpio_pin_toggle_dt(beaconRxSwitch);
      ptState = PT_STATE_WAIT_FOR_LATCH_BEACON;
      LOG_WRN("BEACON RECEIVED WAITING FOR LATCH BEACON AFTER %d OPS", 2 * ops_per_beacon);
    }
    else
    {
      err = DectPhy_Receive(BEACON_RX_HANDLE, US_TO_MODEM_TICKS(1000000000), 0); // Unexpected non beacon
      // LOG_WRN("NON BEACON %d", err);
    }
  }
  else if (ptState == PT_STATE_WAIT_FOR_LATCH_BEACON)
  {
    if (DectPhy_PktIsBeacon(evt->data))
    {
      LOG_WRN("LATCH BEACON RECEIVED");
      ptState = PT_STATE_SCHEDULED_DOWNLINK;
      gpio_pin_toggle_dt(beaconRxSwitch);
      // k_sem_give(&done_sem);
    }
    else 
    {
      err = DectPhy_Receive(BEACON_RX_HANDLE, US_TO_MODEM_TICKS(1000000000), 0); // Unexpected non beacon // TODO maybe just do this in the op_complete part
      ptState = PT_STATE_WAIT_FOR_BEACON;
      LOG_ERR("LATCH BEACON MISSED. GOING BACK TO PT_STATE_WAIT_FOR_BEACON");
    }
  }
  else if (ptState == PT_STATE_SCHEDULED_DOWNLINK)
  {
    // err = DectPhy_TransmitHeadOfQueue(PT_TX_HANDLE, modem_time + 500 * DECT_GAP_TICK);
    gpio_pin_toggle_dt(ptDlSwitch);
    LOG_WRN("DOWNLINK DATA RECEIVED");
    LOG_HEXDUMP_WRN(evt->data, 16, "DLRX");
    ptState = PT_STATE_SCHEDULED_UPLINK;

    // k_sem_give(&done_sem);
  }

}

static void mock_complete(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
  int err;

  if (evt->err)
  {
    LOG_ERR("op_complete %s cb time %"PRIu64" status %x handle %d ", !IS_RX_HANDLE(evt->handle) ? "TX" : "RX", modem_time, evt->err, evt->handle);
  }

  if (evt->handle == BEACON_LATCH_RX_HANDLE && ptState == PT_STATE_WAIT_FOR_LATCH_BEACON)
  {
    LOG_ERR("PT_STATE_WAIT_FOR_LATCH_BEACON TIMED OUT! FALLING BACK TO WAIT_FOR_BEACON");
    ptState = PT_STATE_WAIT_FOR_BEACON; 
    gpio_pin_toggle_dt(beaconRxSwitch);
    k_sem_give(&done_sem);
  }

  // if (evt->handle == PT_RX_HANDLE)
  // {
  //   LOG_WRN("RX TIMED OUT");
  //   gpio_pin_toggle_dt(ptDlSwitch);
  // }
  // k_sem_give(&done_sem);

  if (ptState == PT_STATE_SCHEDULED_UPLINK && evt->handle == PT_TX_HANDLE)
  {
    gpio_pin_toggle_dt(ptUlSwitch);
    ptState = PT_STATE_FRAME_DONE;
    k_sem_give(&done_sem);
  }

  // if (ptState == PT_STATE_SCHEDULED_UPLINK)
  // {
  //   gpio_pin_toggle_dt(ptUlSwitch);
  //   if (testctr)
  //   {
  //     // err = DectPhy_Receive(PT_RX_HANDLE, US_TO_MODEM_TICKS(10000000), 0); // RECEIVE DOWNLINK TRANSMISSION
  //     ptState = PT_STATE_SCHEDULED_DOWNLINK;
  //     testctr--;
  //     LOG_WRN("UPLINK COMPLETE @ %llu. UPLINK -> DOWNLINK %d", modem_time, testctr);
  //
  //     k_sem_give(&done_sem);
  //   }
  //   else
  //   {
  //     ptState = PT_STATE_WAIT_FOR_BEACON;
  //     LOG_WRN("UPLINK COMPLETE @ %llu. UPLINK -> WAIT_FOR_BEACON %d", modem_time, testctr);
  //   }
  // }

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
    k_sem_take(&done_sem, K_FOREVER);
    
    LOG_ERR("DONE");
    while(true) // goodnight sweet prince
    {
      k_sleep(K_MSEC(1000));
      // LOG_ERR("DONE");
      // gpio_pin_toggle_dt(beaconRxSwitch);
      // gpio_pin_toggle_dt(dlSwitch);
      // gpio_pin_toggle_dt(ptUlSwitch);
    }
  }
}

