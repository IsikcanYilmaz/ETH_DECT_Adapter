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
  LOG_WRN("RECEIVED DECT Pkt. HEADER 0x%04x. PL SIZE %d. %s" , 
          * (uint16_t *) &pkt->header,
          pkt->header.payloadSize,
          pkt->header.isBeacon ? "BEACON" : "NON BEACON"
          );
  LOG_HEXDUMP_WRN(evt->data, evt->len, "PDC");
  
  if (pkt->header.isBeacon)
  {
    DectBeaconMessage_t *beac = pkt->payload; 
    LOG_WRN("ops per beacon %d", beac->ops_per_beacon);
  }
  else
  {
    DectPhy_Receive(BEACON_RX_HANDLE, 0xffffffff, 0);
  }
}

static void mock_complete(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
  int err;
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

