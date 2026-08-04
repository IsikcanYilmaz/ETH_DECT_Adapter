#include "lean_wiznet_driver.h"
#include "phy_main.h"
#include "pt.h"
#include <zephyr/kernel.h>
#include <nrf_modem_dect_phy.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/gpio.h>

LOG_MODULE_REGISTER(dect_phy_pt, LOG_LEVEL_WRN);

extern struct k_sem *operation_sem;
extern struct k_sem *time_sem;

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
        gpio_pin_toggle_dt(dlSwitch);
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
    ptState = (slotCounter < DECT_OPS_PER_BEACON) ? PT_STATE_SCHEDULED_DOWNLINK : PT_STATE_WAIT_FOR_BEACON;
    gpio_pin_toggle_dt(ulSwitch);
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

// Currently we care for two kinds of events:
// Either we receive a packet and a PDC event happens
// or we do a transmit/receive and it completes and a EVT_COMPLETED happens
void Pt_HandleEvent(const struct nrf_modem_dect_phy_event *evt)
{
  if (evt->id == NRF_MODEM_DECT_PHY_EVT_PDC)
  {
    on_pdc_pt(&evt->pdc);
  }
  else if (evt->id == NRF_MODEM_DECT_PHY_EVT_COMPLETED)
  {
    on_op_complete_pt(&evt->op_complete);
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

    LOG_DBG("PT LOOP BEGIN");

    int err = DectPhy_Receive(BEACON_RX_HANDLE, US_TO_MODEM_TICKS(1000000), 0); // GET FAKE BEACON
    k_sem_take(&operation_sem, K_FOREVER);

    if (ptState == PT_STATE_WAIT_FOR_BEACON)
    {
      // LOG_WRN("PT BEACON NOT RECEIVED");
      continue;
    }

    LOG_DBG("PT BEACON RECEIVED");

    while (ptState > PT_STATE_WAIT_FOR_BEACON && k_uptime_get() - lastBeaconTs < MODEM_TICKS_TO_MS(DECT_MASTER_BEACON_PERIOD_TICK))
    {
      k_sleep(K_USEC(1));
    }

    LOG_DBG("PT UNLATCHED");
  }
}

