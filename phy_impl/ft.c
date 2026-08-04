#include "lean_wiznet_driver.h"
#include "phy_main.h"
#include <zephyr/kernel.h>
#include <nrf_modem_dect_phy.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/gpio.h>
#include "ft.h"

LOG_MODULE_REGISTER(dect_phy_ft, LOG_LEVEL_WRN);

extern struct k_sem *operation_sem;
extern struct k_sem *time_sem;

static void on_pdc_ft(const struct nrf_modem_dect_phy_pdc_event *evt) // TODO make this part as lean as possible. just copy over the bytes and let a thread do processing
{
  // LOG_HEXDUMP_WRN(evt->data, evt->len, "RX");
  LOG_DBG("%s", __FUNCTION__);
  if (!DectPhy_PktIsNone(evt->data))
  {
    LOG_DBG("%s %d handle, %d bytes", __FUNCTION__, evt->handle, evt->len);
    LOG_HEXDUMP_DBG(evt->data, evt->len, "RX");
    
    // We got a packet from the DECT connection. Enqueue it to wiznet's tx queue
    struct LeanWiznet_Packet *pkt = k_malloc(sizeof(struct LeanWiznet_Packet) + evt->len);
    memcpy(pkt->payload, evt->data, evt->len);
    pkt->size = evt->len;
    DectPhy_EnqueueEthTx(pkt);
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
      if (slotCounter < DECT_OPS_PER_BEACON)
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
    DectPhy_InFlightCompleted();
  }
  else if (evt->handle >= FT_RX_HANDLE && evt->handle < PT_TX_HANDLE) // RX GOT DONE. SCHEDULE TX
  {
    if (evt->err == 0)
    {
      gpio_pin_toggle_dt(ulSwitch);
      if (slotCounter < DECT_OPS_PER_BEACON)
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
    uint64_t downlinkScheduleTick = base + (3 * DECT_SLOT_DURATION_TICK) + opTransitionLatency; 
    
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

void Ft_HandleEvent(const struct nrf_modem_dect_phy_event *evt)
{ 
  if (evt->id == NRF_MODEM_DECT_PHY_EVT_PDC)
  {
    on_pdc_ft(&evt->pdc);
  }
  else if (evt->id == NRF_MODEM_DECT_PHY_EVT_COMPLETED)
  {
    on_op_complete_ft(&evt->op_complete);
  }
  else if (evt->id == NRF_MODEM_DECT_PHY_EVT_TIME)
  {
		on_time_get_ft(&evt->time_get);
  }
  else
  {
    LOG_ERR("FT Unhandled event %d", evt->id);
  }
}

void Ft_Init(void)
{

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
      LOG_ERR("FT LOOP RESTARTING"); 
      nrf_modem_dect_phy_time_get(); 
    }
  }
}
