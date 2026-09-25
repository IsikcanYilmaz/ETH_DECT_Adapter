#include <zephyr/kernel.h>
#include <string.h>
#include <stdbool.h>
#include <nrf_modem_dect_phy.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include "lean_wiznet_driver.h"
#include "phy_main.h"
#include "pt.h"
#include "ft.h"

LOG_MODULE_REGISTER(dect_phy, LOG_LEVEL_WRN);

// #define CONFIG_CARRIER (1677) // from overlay-eu.conf
#define CONFIG_CARRIER (1657) // from overlay-eu.conf

extern struct k_queue ethTxQueue; // TODO JON There comes a point where we dont free these things
extern struct k_queue ethRxQueue;

K_QUEUE_DEFINE(inFlightQueue); // on transmission op completes, free these pointers

extern const struct gpio_dt_spec tp23Switch; // todo put these elsewhere
extern const struct gpio_dt_spec tp24Switch;
extern const struct gpio_dt_spec tp25Switch;
extern const struct gpio_dt_spec tp26Switch;
extern const struct gpio_dt_spec tp27Switch;

const struct gpio_dt_spec *beaconTxSwitch = &tp23Switch;
const struct gpio_dt_spec *beaconRxSwitch = &tp23Switch;

const struct gpio_dt_spec *dlSwitch = &tp24Switch;
const struct gpio_dt_spec *ulSwitch = &tp25Switch;

const struct gpio_dt_spec *ptDlSwitch = &tp25Switch;
const struct gpio_dt_spec *ptUlSwitch = &tp24Switch;

static const enum nrf_modem_dect_phy_radio_mode radioMode = NRF_MODEM_DECT_PHY_RADIO_MODE_LOW_LATENCY;

struct nrf_modem_dect_phy_latency_info latencyInfo;
uint32_t opTransitionLatency; 
uint32_t opStartupLatency;
uint32_t tx_idleToActiveLatency;
uint32_t tx_activeToIdleLatency;
uint32_t rx_idleToActiveLatency;
uint32_t rx_activeToIdleLatency;

uint32_t blockTicks; // 1 SLOT TICKS + 1 op Trans

volatile uint64_t lastBeaconCplt = 0;
volatile uint64_t lastTxCplt = 0;
volatile uint64_t lastRxCplt = 0;

volatile uint64_t beaconDelta = 0; // Counted at the ends of operations
volatile uint64_t txDelta = 0; // Counted at the ends of operations only when counter is > 1
volatile uint64_t rxDelta = 0;

DectBeaconMessage_t master_beacon;

volatile enum DectPtState_e ptState = PT_STATE_IDLE;
volatile enum DectFtState_e ftState = FT_STATE_IDLE;

int mcs_max = -1;

// STATISTICS
uint32_t numSentEthFrames = 0;
uint32_t numSentDatagrams = 0;
uint32_t numSentBytes = 0;
uint32_t numSentDataBytes = 0;
uint32_t numWastedBytes = 0;

void DectPhy_PrintStatistics(const struct shell *shell)
{
  shell_print(shell, "Dect Bridge status:\nSent eth frames: %d \nSent Datagrams: %d \nSent Bytes: %d \nSent SDU Bytes: %d \nWasted Bytes: %d \n", 
              numSentEthFrames, 
              numSentDatagrams, 
              numSentBytes,
              numSentDataBytes,
              numWastedBytes
              );
}

void DectPhy_ResetStatistics(void)
{
  numSentEthFrames = 0;
  numSentDatagrams = 0;
  numSentBytes = 0;
  numSentDataBytes = 0;
  numWastedBytes = 0;
}

// FRAGMENTATION
// Tx
struct LeanWiznet_Packet *currentTxDatagram = NULL;
uint16_t currentTxDatagramOffset = 0;
uint16_t currentTxDatagramRemainingBytes = 0;
uint16_t currentTxDatagramTag = 0;

// Rx
struct LeanWiznet_Packet *currentRxDatagram = NULL;
uint8_t currentRxDatagramTag = 0;
uint16_t currentRxDatagramOffset = 0;
uint16_t currentRxDatagramSize = 0;

// KNOBS
DectKnobs_t knobs = {
  .mcs = CONFIG_APP_MCS,
  .ops_per_beacon = DECT_OPS_PER_BEACON,
  .carrier = CONFIG_CARRIER,
};

// MCS to NUM BYTES PER SLOT. Indexed by MCS
// MCS:                      0   1   2   3   4
int mcsToBytesPerSlot[5] = {17, 37, 57, 77, 117};

inline uint64_t us_to_modem_ticks(uint64_t us)
{
  return (((uint64_t) us / 1000) * NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ);
}

inline bool is_rx_handle(uint32_t h)
{
  return ((h % 2) != 0);
}

static void set_all_tps(int set)
{
  gpio_pin_set_dt(&tp23Switch, set);
  gpio_pin_set_dt(&tp24Switch, set);
  gpio_pin_set_dt(&tp25Switch, set);
  gpio_pin_set_dt(&tp26Switch, set);
  gpio_pin_set_dt(&tp27Switch, set);
  // gpio_pin_set_dt(&tp03Switch, set);
  // gpio_pin_set_dt(&tp04Switch, set);
}

static bool exit;
static uint16_t device_id;
volatile bool warmedUp = false;
volatile uint64_t modem_time;

uint32_t numSlotsInFrame = 0;
uint32_t slotCounter = 0;
uint32_t frameCounter = 0;

uint64_t lastPccModemTick = 0;
uint64_t lastPdcModemTick = 0;
uint64_t lastBeaconModemTick = 0;
uint64_t lastLoopModemTick = 0;
uint64_t pccPdcDiff = 0;

uint32_t lastBeaconTs = 0;

uint64_t genericBeaconScheduleOffset;
uint64_t genericRxScheduleOffset;
uint64_t genericRxDuration;
uint64_t genericRelativeRxSchedule;

static volatile bool iAmFt;

/* Header type 1, due to endianness the order is different than in the specification. */
struct phy_ctrl_field_common {
	uint32_t packet_length : 4;
	uint32_t packet_length_type : 1;
	uint32_t header_format : 3;
	uint32_t short_network_id : 8;
	uint32_t transmitter_id_hi : 8;
	uint32_t transmitter_id_lo : 8;
	uint32_t df_mcs : 3;
	uint32_t reserved : 1;
	uint32_t transmit_power : 4;
	uint32_t pad : 24;
};

/* Dect PHY config parameters. */
static struct nrf_modem_dect_phy_config_params dect_phy_config_params = {
	.band_group_index = ((CONFIG_CARRIER >= 525 && CONFIG_CARRIER <= 551)) ? 1 : 0, // TODO if we ever use a carrier between 525 and 551 this bit needs to be 1. seems like we wont
	.harq_rx_process_count = 4, // JON can i lower this since i dont use harq
	.harq_rx_expiry_time_us = 5000000,
};

K_SEM_DEFINE(operation_sem, 0, 1); 
K_SEM_DEFINE(cancel_sem, 0, 1);
K_SEM_DEFINE(rx_done_sem, 0, 1);
K_SEM_DEFINE(tx_done_sem, 0, 1);
K_SEM_DEFINE(time_sem, 0, 1);
K_SEM_DEFINE(done_sem, 0, 1);
K_SEM_DEFINE(resync_sem, 0, 1);

bool DectPhy_PktIsBeacon(char *pkt)
{
  DectBeaconMessage_t *beac = pkt;
  return (strncmp(beac->magic, DECT_BEACON_MAGIC_STRING, 4) == 0);
}

bool DectPhy_PktIsNone(char *pkt) 
{
  if (strncmp(pkt, "NONE", 4) == 0)
  {
    return true;
  }
  return false;
}

int DectPhy_ReceiveContinuous(uint32_t handle, uint32_t durationTicks, uint64_t start_time)
{
  int err;

	struct nrf_modem_dect_phy_rx_params rx_op_params = {
		.start_time = start_time,
		.handle = handle,
		.network_id = CONFIG_APP_NETWORK_ID,
		.mode = NRF_MODEM_DECT_PHY_RX_MODE_CONTINUOUS,
		.rssi_interval = NRF_MODEM_DECT_PHY_RSSI_INTERVAL_OFF,
		.link_id = NRF_MODEM_DECT_PHY_LINK_UNSPECIFIED,
		.rssi_level = -60,
		.carrier = knobs.carrier,
		.duration = durationTicks,
		.filter.short_network_id = CONFIG_APP_NETWORK_ID & 0xff,
		.filter.is_short_network_id_used = 1,
		/* listen for everything (broadcast mode used) */
		.filter.receiver_identity = 0,
	};

	err = nrf_modem_dect_phy_rx(&rx_op_params);
	if (err != 0) 
  {
		return err;
	}

	return 0;
}

int DectPhy_Receive(uint32_t handle, uint32_t durationTicks, uint64_t start_time)
{
  int err;

	struct nrf_modem_dect_phy_rx_params rx_op_params = {
		.start_time = start_time,
		.handle = handle,
		.network_id = CONFIG_APP_NETWORK_ID,
		.mode = NRF_MODEM_DECT_PHY_RX_MODE_SINGLE_SHOT,
		.rssi_interval = NRF_MODEM_DECT_PHY_RSSI_INTERVAL_OFF,
		.link_id = NRF_MODEM_DECT_PHY_LINK_UNSPECIFIED,
		.rssi_level = -60,
		.carrier = knobs.carrier,
		.duration = durationTicks,
		.filter.short_network_id = CONFIG_APP_NETWORK_ID & 0xff,
		.filter.is_short_network_id_used = 1,
		/* listen for everything (broadcast mode used) */
		.filter.receiver_identity = 0,
	};

	err = nrf_modem_dect_phy_rx(&rx_op_params);
	if (err != 0) 
  {
		return err;
	}

	return 0;
}

// Upon Tx Complete, call this to mark the top of our inFlightQueue as complete. This MUST be done! 
// TODO find a better way lol
void DectPhy_InFlightCompleted(void)
{
  // TODO depricating the inflight business
  // // Tx just completed. Free the pointer of what just got tx'd. 
  // if (k_queue_is_empty(&inFlightQueue))
  // {
  //   LOG_ERR("TX COMPLETE BUT IN FLIGHT QUEUE EMPTY!!!");
  // }
  // else
  // {
  //   struct DectInFlightPktStub_s *pktToFree = k_queue_get(&inFlightQueue, K_FOREVER);
  //   if (pktToFree)
  //   {
  //     if (pktToFree->ptr)
  //       LOG_DBG("IN FLIGHT PKT FROM HANDLE %d DONE. 0x%08x. FREEING", pktToFree->handle, pktToFree->ptr);
  //     k_free(pktToFree->ptr);
  //     k_free(pktToFree);
  //   }
  // }
}

int DectPhy_Transmit(uint32_t handle, void *data, size_t data_len, uint64_t start_time)
{
  int err;

  struct phy_ctrl_field_common header = {
    .header_format = 0x0,
    .packet_length_type = DECT_PACKET_LENGTH_SLOT,
    .packet_length = 0x00,
    .short_network_id = (CONFIG_APP_NETWORK_ID & 0xff),
    .transmitter_id_hi = (device_id >> 8),
    .transmitter_id_lo = (device_id & 0xff),
    .transmit_power = CONFIG_APP_TX_POWER,
    .reserved = 0,
    .df_mcs = knobs.mcs,
  };

  struct nrf_modem_dect_phy_tx_params tx_op_params = {
    .start_time = start_time,
    .handle = handle,
    .network_id = CONFIG_APP_NETWORK_ID,
    .phy_type = 0,
    .lbt_rssi_threshold_max = 0,
    .carrier = knobs.carrier,
    .lbt_period = 0,
    .phy_header = (union nrf_modem_dect_phy_hdr *) &header,
    .data = data,
    .data_size = data_len,
  };

  LOG_DBG("Transmitting %d bytes", data_len);

  err = nrf_modem_dect_phy_tx(&tx_op_params);
	if (err != 0) {
		return err;
	}

	return 0;
}

// This is the funnel that should handle all higher layer data to be then passed along to the Wiznet shield
int DectPhy_HandleSDUFragment(char *data, size_t len)
{
  int err = 0;
  DectPacket_t *dectPkt = (DectPacket_t *) data;

  bool isDataPacket = ((dectPkt->flags & (1 << DECT_DATA_PACKET_FLAG_BIT)) > 0);
  bool isFragmented = ((dectPkt->flags & (1 << DECT_FRAG_HEADER_EXISTS_FLAG_BIT)) > 0);
  bool reassemblyComplete = (!isFragmented);
  DectFragmentationHeader_t *fragHeader = (isFragmented) ? (DectFragmentationHeader_t *) (dectPkt->payload) : NULL;
  char *sduPayload = (isFragmented) ? ((char *) dectPkt->payload) + (sizeof(DectFragmentationHeader_t)) : ((char *) dectPkt->payload);
  size_t sduSize = dectPkt->payloadSize;
  
  LOG_DBG("RECEIVED SDU DATA %d B. %s %s", sduSize, (isDataPacket) ? "[DATA]":"", (isFragmented) ? "[FRAG]":"");
  LOG_HEXDUMP_DBG(data, len, "FUNNEL");

  // We just received some SDU data. now we either
  // 1) were expecting any data which is the best case
  // 2) were expecting the next fragment of a packet we've been reassembling 
  //
  // The data we got is either
  // a) is fragmented
  // b) is a full datagram
  //
  // we could notify this layer if we ever miss a slot or something. maybe that's the way to go
  
  // Figure out the datagram size

  // If we have nothing being reassembled or received, malloc something for it.
  if (currentRxDatagram == NULL)
  {
    currentRxDatagramSize = (isFragmented) ? fragHeader->datagramSize : sduSize;
    currentRxDatagramOffset = 0;
    currentRxDatagram = (struct LeanWiznet_Packet *) k_malloc(sizeof(struct LeanWiznet_Packet) + currentRxDatagramSize);
    if (currentRxDatagram == NULL)
    {
      LOG_ERR("%s:%d OOM", __FUNCTION__, __LINE__);
      return 1;
    }
    LOG_DBG("%sNEW PACKET. DATAGRAM SIZE %d", (isFragmented) ? "ASSEMBLING " : "", currentRxDatagramSize);
  }

  if (isFragmented) 
  {
    // If the SDU we got is a fragment AND we're already reassembling another packet
    if (sduSize < sizeof(DectFragmentationHeader_t)) // Sanity check
    {
      LOG_ERR("%s:%d Bad SDU Size %d!", __FUNCTION__, __LINE__, sduSize);
      LOG_HEXDUMP_ERR(data, len, "BAD PKT");
      k_free(currentRxDatagram);
      currentRxDatagram = NULL;
      return 1;
    }
    sduSize -= sizeof(DectFragmentationHeader_t);
    LOG_DBG("FRAGMENT: OFFSET %d, SIZE %d, FRAGMENT PL SIZE %d", fragHeader->datagramOffset, fragHeader->datagramSize, sduSize);

    // Sanity check: if the size of the data portion of the current fragment packet 
    if (fragHeader->datagramOffset + sduSize <= currentRxDatagramSize) 
    {
      memcpy((char *) (currentRxDatagram->payload) + fragHeader->datagramOffset, sduPayload, sduSize);
    }
    else
    {
      LOG_ERR("%s:%d bad datagram offset! %d", __FUNCTION__, __LINE__, fragHeader->datagramOffset);
      LOG_ERR("Datagram offset %d, sdu size %d, currentRxDatagramSize %d", fragHeader->datagramOffset, sduSize, currentRxDatagramSize);
      LOG_HEXDUMP_ERR(data, len, "RAW"); 
      LOG_HEXDUMP_ERR(currentRxDatagram, sduSize, "TO BE SENT TO W5500");
      LOG_ERR("---");
      k_free(currentRxDatagram);
      currentRxDatagram = NULL;
      return 1;
    }

    LOG_HEXDUMP_DBG(currentRxDatagram->payload, currentRxDatagramSize, "REASSEMBLY IN PROGRESS");

    currentRxDatagramOffset += sduSize;
    if (currentRxDatagramOffset >= currentRxDatagramSize) // should match exactly actually
    {
      reassemblyComplete = true;
    }
  }
  else
  {
    // JON TODO POTENTIAL BUG: When implementing frame packing note that here: if we get a nonfragmented tx, we assume we can fill up the entire datagram buffer with it. 
    memcpy((char *) (currentRxDatagram->payload), sduPayload, sduSize);
  }

  if (reassemblyComplete)
  {
    LOG_DBG("REASSEMBLY COMPLETE. SENDING OFF %d BYTES TO ETH", currentRxDatagramSize);
    LOG_HEXDUMP_DBG(currentRxDatagram->payload, currentRxDatagramSize, "REASSEMBLY COMPLETE");
    
    currentRxDatagram->size = currentRxDatagramSize;
    DectPhy_EnqueueEthTx(currentRxDatagram); // This pointer will be freed by LeanWiznet driver
    currentRxDatagram = NULL;
    currentRxDatagramSize = 0;
    currentRxDatagramOffset = 0;
    currentRxDatagramTag = 0;
  }

  return err;
}

// This is the funnel where the packets that we receive from the DECT channel should go to.
size_t DectPhy_UnpackFrameAndProcessSDUs(DectPacket_t *frame, size_t frameSize)
{
  DectPacket_t *currentSdu = frame;
  DectPacket_t *nextSdu = NULL;
  int remainingFrameSize = frameSize;
  int numSdusInFrame = 0;

  int cnt = 0;

  // Go through each header and SDU. If we fully reassemble a datagram, pass it to the Wiznet layer
  while(remainingFrameSize)
  {
    if (currentSdu->flags == 0x00 || currentSdu->flags == 0xff) //  TODO decide this
    {
      LOG_DBG("0 FLAGS. DONE");
      break;
    }
    bool isData = ((currentSdu->flags & (1 << DECT_DATA_PACKET_FLAG_BIT)) > 0);
    bool isFragment = ((currentSdu->flags & (1 << DECT_FRAG_HEADER_EXISTS_FLAG_BIT)) > 0);
    size_t headerPlusSduSize = sizeof(DectPacket_t) + (currentSdu->payloadSize) + ((isFragment) ? sizeof(DectFragmentationHeader_t) : 0);
    nextSdu = (uint8_t *) currentSdu + headerPlusSduSize;
    LOG_DBG("SDU %d. FLAGS 0x%x D%d F%d, PAYLOAD SIZE %d, TOTAL SIZE %d, FRAME SIZE %d", numSdusInFrame, currentSdu->flags, isData, isFragment, currentSdu->payloadSize, currentSdu->payloadSize + 2, frameSize);
    LOG_DBG("SDU 0x%x - 0x%x. REMAINING %d", currentSdu, nextSdu, remainingFrameSize);
    LOG_HEXDUMP_DBG(currentSdu, headerPlusSduSize, "HEADER PLUS SDU");

    // We dissected a fragment. Pass the pointer along
    DectPhy_HandleSDUFragment((char *) currentSdu, headerPlusSduSize);

    remainingFrameSize -= headerPlusSduSize;
    numSdusInFrame++;
    currentSdu = (DectPacket_t *) nextSdu;
  }
  
  LOG_DBG("UNPACKED %d SDUS. REMAINING FRAME SIZE %d", numSdusInFrame, remainingFrameSize);
  return 0;
}

size_t DectPhy_PackFrame(DectPacket_t *frame, size_t frameSize)
{
  char *head = (char *) frame;
  char *tail = (char *) frame;
  size_t remainingFrameSize = frameSize;
  size_t numBytesPacked = 0;
  size_t numGoodBytesPacked = 0;

  int numSdusTouched = 0; // Bytes from this many SDUs are in this frame
  
  while (remainingFrameSize)
  {
    // TODO prioritize SDUs and IEs here
    
    // First, check if we dont have a loaded up datagram. If not, load it up
    if (currentTxDatagram == NULL && !k_queue_is_empty(&ethRxQueue))
    {
      currentTxDatagram = (struct LeanWiznet_Packet *) k_queue_get(&ethRxQueue, K_FOREVER);
      currentTxDatagramOffset = 0;
      currentTxDatagramRemainingBytes = currentTxDatagram->size;
      LOG_DBG("LOADED UP DATAGRAM %d. %d BYTES", currentTxDatagramTag, currentTxDatagram->size);
    }

    // At this point if we dont have a currentTxDatagram, there's nothing to be sent. can break out
    if (currentTxDatagram == NULL)
    {
      LOG_DBG("NO DATAGRAM IN THE QUEUE");
      break;
    }

    LOG_DBG("~~ PACK FRAME LOOP %d. REMAINING BYTES IN THE FRAME %d ~~" , numSdusTouched, remainingFrameSize);
    numSdusTouched++; // TODO rm
    if (numSdusTouched == 5)
    {
      LOG_ERR("INF LOOP. BREAKING");
      break;
    }

    // Here we must have a loaded up current Tx Datagram. Check if it needs or already is fragmenting
    bool needsFragmenting = (currentTxDatagramOffset || currentTxDatagram->size > (remainingFrameSize - sizeof(DectPacket_t)));
    size_t overheadHeaderSize = (needsFragmenting) ? (sizeof(DectFragmentationHeader_t) + sizeof(DectPacket_t)) : (sizeof(DectPacket_t));

    // Find the SDU size
    size_t sduSize;
    if (currentTxDatagramRemainingBytes <= (remainingFrameSize - overheadHeaderSize)) // If datagram's remaining bytes can fit into this current SDU
    {
      sduSize = currentTxDatagramRemainingBytes;
    }
    else // If it cannot fit
    {
      sduSize = (remainingFrameSize - overheadHeaderSize);
    }

    // Start filling out the headers
    DectPacket_t *subframe = head;
    subframe->flags = 0;
    subframe->flags |= (1 << DECT_DATA_PACKET_FLAG_BIT); // TODO only do this if this is a data SDU
    subframe->payloadSize = sduSize;
    
    // If this is a fragment, use up some of our real estate for the fragmentation header
    if (needsFragmenting)
    {
      subframe->flags |= (1 << DECT_FRAG_HEADER_EXISTS_FLAG_BIT); 
      DectFragmentationHeader_t *fragHeader = (DectFragmentationHeader_t *) subframe->payload;
      fragHeader->datagramSize = currentTxDatagram->size;
      fragHeader->datagramOffset = currentTxDatagramOffset;
      // fragHeader->datagramTag = currentTxDatagramTag;
    }

    LOG_DBG("DATAGRAM %d: OFFSET %d, %d BYTES PACKED", currentTxDatagramTag, currentTxDatagramOffset, sduSize);
    LOG_DBG("FRAME: SIZE %d, #SDU %d, HEADER SIZE %d SDU SIZE %d", frameSize, numSdusTouched, overheadHeaderSize, sduSize);
    // memset(((uint8_t *) head + overheadHeaderSize), (uint8_t) currentTxDatagramTag, sduSize); // TEST 
    memcpy(((uint8_t *) head + overheadHeaderSize), (uint8_t *) currentTxDatagram->payload + currentTxDatagramOffset, sduSize);
    head += (overheadHeaderSize + sduSize);

    // Book keep
    numBytesPacked += sduSize + overheadHeaderSize;
    numGoodBytesPacked += sduSize;
    remainingFrameSize -= (sduSize + overheadHeaderSize);

    currentTxDatagramRemainingBytes -= sduSize;
    currentTxDatagramOffset += sduSize;

    if (currentTxDatagramRemainingBytes == 0)
    {
      LOG_DBG("ALL FRAGMENTS OF TAG %d PACKED", currentTxDatagramTag);
      currentTxDatagramTag++;
      k_free(currentTxDatagram);
      currentTxDatagram = NULL;
      currentTxDatagramOffset = 0;
      numSentDatagrams++;
    }
  }

  if (remainingFrameSize) // If there's bytes remaining, make the first free byte 0xff
  {
    *((uint8_t *) frame + (frameSize - remainingFrameSize)) = DECT_MESSAGE_END_BYTE;
  }

  if (numBytesPacked)
  {
    LOG_DBG("PACKED %d GOOD BYTES, %d SDUS INTO %d BYTE FRAME", numGoodBytesPacked, numSdusTouched, frameSize);
    LOG_HEXDUMP_DBG(frame, frameSize, "FRAME");
    LOG_DBG("------------");
    LOG_DBG("FRAME 0x%x HEAD 0x%x REMAINING %d", (uint8_t *) frame, (uint8_t *) head, remainingFrameSize);
    // DectPhy_UnpackFrameAndProcessSDUs(frame, frameSize); // TODO TESTING
  }

  return numBytesPacked;
}

int DectPhy_TransmitHeadOfQueue(uint32_t handle, uint64_t start_time)
{
  int err;
  // If we have an outgoing datagram loaded up, keep sending it. 
  size_t effectivePayloadSize;
  size_t txSizePerMcs = mcsToBytesPerSlot[knobs.mcs];
  DectPacket_t *frameToTx = k_malloc(txSizePerMcs); 

  // memset(frameToTx, 0x00, txSizePerMcs); // TEST

  size_t numBytesToSend = DectPhy_PackFrame(frameToTx, txSizePerMcs);

  // TODO Handle OOM. PackFrame() now handles the loading and unloading of currentTxDatagram
  if (frameToTx == NULL) // JON TODO Here if an OOM happens we shoot a blank and drop the whole datagram. there's
  {
    LOG_ERR("%s:%d OOM", __FUNCTION__, __LINE__);
    return 1;
  }

  if (numBytesToSend)
  {
    // We have a MTU to send. Send it off
    LOG_DBG("MTU BYTES TO SEND %d", numBytesToSend);
    err = DectPhy_Transmit(handle, frameToTx, numBytesToSend, start_time);
  }
  else
  {
    // If we dont have a loaded up outgoing datagram, send a blank. TODO bad 
    LOG_DBG("NO PKT FROM ETH. SENDING BLANK TX");
    err = DectPhy_Transmit(handle, "NONE", 4, start_time); // TODO Change this to use the actual PDU 
  }

  k_free(frameToTx);
  return err;
}

int DectPhy_TransmitBeacon(uint64_t start_time)
{
  int err; 
  // TODO more in depth logic
  master_beacon.this_beacon_time = start_time;
  uint32_t expected_next_beacon_offset = (beaconDelta) ? (uint32_t) beaconDelta : (uint32_t) ((2*knobs.ops_per_beacon) * (DECT_SLOT_DURATION_TICK + DECT_HEADROOM + opTransitionLatency)); // TODO bad solution but will do. basically we're just sending the delta in ticks, between this xmit and the previous one. it worked
  master_beacon.modem_ticks_until_next_beacon = expected_next_beacon_offset; // TODO the very first beacon does not have a correct number of $modem_ticks_until_next_beacon. as a result the pt can only latch 2 beacon later. not a showstopper but we should fix this

  // TODO TESTING
  // static bool first = false;
  // if (!first)
  // {
  //   if (master_beacon.modem_ticks_until_next_beacon == 0)
  //   {
  //     LOG_ERR("BEACON Tix until next is 0. Making assumptions... ops per beacon %d", knobs.ops_per_beacon);
  //     LOG_ERR("Assuming %llu ticks", (uint32_t) ((2*knobs.ops_per_beacon + 1) * (DECT_SLOT_DURATION_TICK + DECT_HEADROOM + opTransitionLatency)));
  //   }
  //   else
  //   {
  //     LOG_ERR("FIRST tix until next %d, beacondelta %llu", master_beacon.modem_ticks_until_next_beacon, beaconDelta);
  //     first = true;
  //   }
  // }

  // TODO make these generic, reuse
  struct phy_ctrl_field_common header = {
    .header_format = 0x0,
    .packet_length_type = DECT_PACKET_LENGTH_SLOT,
    .packet_length = 0x00,
    .short_network_id = (CONFIG_APP_NETWORK_ID & 0xff),
    .transmitter_id_hi = (device_id >> 8),
    .transmitter_id_lo = (device_id & 0xff),
    .transmit_power = CONFIG_APP_TX_POWER,
    .reserved = 0,
    .df_mcs = knobs.mcs,
  };

  struct nrf_modem_dect_phy_tx_params beacon_op_params = {
    .start_time = start_time,
    .handle = BEACON_TX_HANDLE,
    .network_id = CONFIG_APP_NETWORK_ID,
    .phy_type = 0,
    .lbt_rssi_threshold_max = 0,
    .carrier = knobs.carrier,
    .lbt_period = 0,// NRF_MODEM_DECT_LBT_PERIOD_MAX, // JON EXPERIMENTAL
    .phy_header = (union nrf_modem_dect_phy_hdr *) &header,
    .data = &master_beacon,
    .data_size = sizeof(DectBeaconMessage_t),
  };

  err = nrf_modem_dect_phy_tx(&beacon_op_params);
	if (err != 0) {
		return err;
	}

  return 0;
}

// enqueue packets here to send them over the ethernet connection
void DectPhy_EnqueueEthTx(void *pkt)
{
  k_queue_append(&ethTxQueue, pkt);
}

int DectPhy_CancelAllPendingOps(void)
{
  return nrf_modem_dect_phy_cancel(NRF_MODEM_DECT_PHY_HANDLE_CANCEL_ALL);
}

/* Callback after init operation. */
static void on_init(const struct nrf_modem_dect_phy_init_event *evt)
{
	if (evt->err) {
		LOG_ERR("Init failed, err %d", evt->err);
		exit = true;
		return;
	}
	k_sem_give(&operation_sem);
}

static void on_configure(const struct nrf_modem_dect_phy_configure_event *evt)
{
	if (evt->err) {
		LOG_ERR("Configure failed, err %d", evt->err);
		return;
	}
	k_sem_give(&operation_sem);
}

static void on_activate(const struct nrf_modem_dect_phy_activate_event *evt)
{
	if (evt->err) {
		LOG_ERR("Activate failed, err %d", evt->err);
		exit = true;
		return;
	}
	k_sem_give(&operation_sem);
}

static void on_capability_get(const struct nrf_modem_dect_phy_capability_get_event *evt)
{
  if (evt->err)
  {
    LOG_ERR("capability_get cb time %"PRIu64" status %x", modem_time, evt->err);
  }
  else 
  {
    LOG_ERR("capability_get cb time %"PRIu64" status %x", modem_time, evt->err);
    struct nrf_modem_dect_phy_capability *capa = evt->capability;
    LOG_ERR("rx spatial streams: %d\n\
            mcs max:             %d\n\
            current mcs:         %d\n\
            mu:                  %d\n\
            beta:                %d\n\
            bytes per slot:      %d\n", 
            capa->variant[0].rx_spatial_streams, capa->variant[0].mcs_max, knobs.mcs, capa->variant[0].mu, capa->variant[0].mcs_max, mcsToBytesPerSlot[knobs.mcs]);

    mcs_max = capa->variant[0].mcs_max;
  }
	k_sem_give(&operation_sem);
}

static void on_pcc(const struct nrf_modem_dect_phy_pcc_event *evt)
{
	LOG_DBG("PCC Received header from device ID %d", evt->hdr.hdr_type_1.transmitter_id_hi << 8 | evt->hdr.hdr_type_1.transmitter_id_lo);
}

static void on_pcc_crc_err(const struct nrf_modem_dect_phy_pcc_crc_failure_event *evt)
{
	LOG_ERR("pcc_crc_err cb time %"PRIu64"", modem_time);
}

static void on_pdc_crc_err(const struct nrf_modem_dect_phy_pdc_crc_failure_event *evt)
{
	LOG_ERR("pdc_crc_err cb time %"PRIu64"", modem_time);
}

static void on_latency_info_get(const struct nrf_modem_dect_phy_latency_info_event *evt)
{
	LOG_WRN("latency_info_get cb status %x", evt->err);
  if (evt->err == 0)
  {
    memcpy(&latencyInfo, evt->latency_info, sizeof(struct nrf_modem_dect_phy_latency_info));
    opTransitionLatency = latencyInfo.radio_mode[radioMode].scheduled_operation_transition;
    opStartupLatency = latencyInfo.radio_mode[radioMode].scheduled_operation_startup;
    tx_idleToActiveLatency = latencyInfo.operation.transmit.idle_to_active;
    tx_activeToIdleLatency = latencyInfo.operation.transmit.active_to_idle;
    rx_idleToActiveLatency = latencyInfo.operation.receive.idle_to_active;
    rx_activeToIdleLatency = latencyInfo.operation.receive.active_to_idle_rx;
    LOG_WRN("Latency info: \n\
            slot_ticks:              %llu\n\
            headroom_ticks:          %llu\n\
            scheduled_op_transition: %d\n\
            op_startup:              %d\n\
            tx_idleToActiveLatency:  %d\n\
            tx_activeToIdleLatency:  %d\n\
            rx_idleToActiveLatency:  %d\n\
            rx_activeToIdleLatency:  %d\n\
            current modem_time:      %llu\n\
            current uptime ticks:    %llu\n\
            modem ticks per ms:      %llu\n\
            host ticks per ms:       %llu\n", 
            (uint64_t) DECT_SLOT_DURATION_TICK, 
            (uint64_t) DECT_HEADROOM, 
            opTransitionLatency, opStartupLatency, tx_idleToActiveLatency, tx_activeToIdleLatency, rx_idleToActiveLatency, rx_activeToIdleLatency,
            modem_time, k_uptime_ticks(), (uint64_t)(NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ), (uint64_t) (CONFIG_SYS_CLOCK_TICKS_PER_SEC / 1000));
  }
  k_sem_give(&operation_sem);
}

static void on_cancel(const struct nrf_modem_dect_phy_cancel_event *evt)
{
  LOG_ERR("CANCEL EVENT %d", evt->err);
  k_sem_give(&cancel_sem);
}

static void dect_phy_event_handler(const struct nrf_modem_dect_phy_event *evt)
{
  modem_time = evt->time;
  // LOG_DBG("%s modem_time %llu Event %d", __FUNCTION__, evt->time, evt->id);
	switch (evt->id) {
	case NRF_MODEM_DECT_PHY_EVT_INIT:
		on_init(&evt->init);
		break;
	case NRF_MODEM_DECT_PHY_EVT_DEINIT:
		// on_deinit(&evt->deinit);
		break;
	case NRF_MODEM_DECT_PHY_EVT_ACTIVATE:
		on_activate(&evt->activate);
		break;
	case NRF_MODEM_DECT_PHY_EVT_DEACTIVATE:
		// on_deactivate(&evt->deactivate);
		break;
	case NRF_MODEM_DECT_PHY_EVT_CONFIGURE:
		on_configure(&evt->configure);
		break;
	case NRF_MODEM_DECT_PHY_EVT_RADIO_CONFIG:
		// on_radio_config(&evt->radio_config);
		break;
	case NRF_MODEM_DECT_PHY_EVT_COMPLETED:
    if (iAmFt)
    {
      Ft_HandleEvent(evt);
    }
    else
    {
      Pt_HandleEvent(evt);
    }
		break;
	case NRF_MODEM_DECT_PHY_EVT_CANCELED:
		on_cancel(&evt->cancel);
		break;
	case NRF_MODEM_DECT_PHY_EVT_RSSI:
		// on_rssi(&evt->rssi);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PCC:
		// on_pcc(&evt->pcc);
    lastPccModemTick = modem_time;
    if (iAmFt)
    {
      Ft_HandleEvent(evt);
    }
    else
    {
      Pt_HandleEvent(evt);
    }
		break;
	case NRF_MODEM_DECT_PHY_EVT_PCC_ERROR:
		on_pcc_crc_err(&evt->pcc_crc_err);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PDC:
    lastPdcModemTick = modem_time;
    pccPdcDiff = lastPdcModemTick - lastPccModemTick;
    if (iAmFt)
    {
      Ft_HandleEvent(evt);
    }
    else 
    {
      Pt_HandleEvent(evt);
    }
		break;
	case NRF_MODEM_DECT_PHY_EVT_PDC_ERROR:
		on_pdc_crc_err(&evt->pdc_crc_err);
		break;
	case NRF_MODEM_DECT_PHY_EVT_TIME:
    if (iAmFt)
    {
      Ft_HandleEvent(evt);
    }
    else 
    {
      Pt_HandleEvent(evt);
    }
		break;
	case NRF_MODEM_DECT_PHY_EVT_CAPABILITY:
		on_capability_get(&evt->capability_get);
		break;
	case NRF_MODEM_DECT_PHY_EVT_BANDS:
		// on_bands_get(&evt->band_get);
		break;
	case NRF_MODEM_DECT_PHY_EVT_LATENCY:
		on_latency_info_get(&evt->latency_get);
		break;
	case NRF_MODEM_DECT_PHY_EVT_LINK_CONFIG:
		// on_link_config(&evt->link_config);
		break;
	case NRF_MODEM_DECT_PHY_EVT_STF_CONFIG:
		// on_stf_cover_seq_control(&evt->stf_cover_seq_control);
		break;
	case NRF_MODEM_DECT_PHY_EVT_TEST_RF_TX_CW_CONTROL_CONFIG:
		// on_test_rf_tx_cw_ctrl(&evt->test_rf_tx_cw_control);
		break;
	}
}

// Public fns
int DectPhy_Init(void)
{
  int err;

  err = nrf_modem_lib_init();
  if (err) {
		LOG_ERR("modem init failed, err %d", err);
		return err;
	}

  err = nrf_modem_dect_phy_event_handler_set(dect_phy_event_handler);
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_event_handler_set failed, err %d", err);
		return err;
	}

  err = nrf_modem_dect_phy_init();
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_init failed, err %d", err);
		return err;
	}

	k_sem_take(&operation_sem, K_FOREVER);
	if (exit) {
		return -EIO;
	}
  ////////////////////////////////////// on_init will release the semaphore

	err = nrf_modem_dect_phy_configure(&dect_phy_config_params);
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_configure failed, err %d", err);
		return err;
	}

	k_sem_take(&operation_sem, K_FOREVER);
	if (exit) {
		return -EIO;
	}

  nrf_modem_dect_phy_latency_get();
  k_sem_take(&operation_sem, K_FOREVER);

  nrf_modem_dect_phy_capability_get();
  k_sem_take(&operation_sem, K_FOREVER);

  ////////////////////////////////////// on_configure will release the semaphore
  
	err = nrf_modem_dect_phy_activate(radioMode);
	if (err) {
		LOG_ERR("nrf_modem_dect_phy_activate failed, err %d", err);
		return err;
	}

	k_sem_take(&operation_sem, K_FOREVER);
	if (exit) {
		return -EIO;
	}
  ///////////////////////////////////// on_activate will release the semaphore
	
  hwinfo_get_device_id((void *)&device_id, sizeof(device_id));
	
  LOG_ERR("Dect NR+ PHY initialized, device ID: %d", device_id);

  return 0;
}

bool DectPhy_WiznetAlert(void) // TODO better way of doing this
{
  return true;
}

static int cmd_bridge(const struct shell *shell, size_t argc, char **argv)
{
  // If ran without args it will print the status
  if (argc == 1)
  {
    shell_print(shell, "Dect Bridge status:");
    shell_print(shell, "I am : %s", (iAmFt) ? "FT" : "PT");
    shell_print(shell, "State: %d", (iAmFt) ? ftState : ptState);
    shell_print(shell, "Knobs: Mcs: %d, carrier: %d, ops: %d", knobs.mcs, knobs.carrier, knobs.ops_per_beacon);
    DectPhy_PrintStatistics(shell);
    return 0;
  }

  // These require args
  if (strncmp(argv[1], "mcs", 3) == 0)
  {
    if (argc == 2)
    {
      shell_print(shell, "Current mcs: %d", knobs.mcs);
    }
    else if (argc >= 3)
    {
      uint32_t new_mcs = atoi(argv[2]);
      if (new_mcs >= 0 && new_mcs <= mcs_max)
      {
        knobs.mcs = new_mcs;
        shell_print(shell, "Current mcs: %d", knobs.mcs);
      }
    }
  }
  if (strncmp(argv[1], "resetstats", 10) == 0)
  {
    DectPhy_ResetStatistics();
  }
  if (strncmp(argv[1], "reboot", 6) == 0)
  {
    sys_reboot(SYS_REBOOT_COLD);
  }
  shell_print(shell, "DONE");
  return 0;
}

SHELL_CMD_ARG_REGISTER(bridge, NULL, "bridge <subcommand>", cmd_bridge, 1, 32);

void DectPhy_Main(bool master)
{	
  set_all_tps(0);

  DectPhy_Init();
  int err;
  iAmFt = master;

  sprintf(master_beacon.magic, "BEAC"); 
  master_beacon.ops_per_beacon = knobs.ops_per_beacon;

  if (iAmFt) // TODO currently these dont do anythying
  {
    Ft_Init();
  }
  else
  {
    Pt_Init();
  }

  nrf_modem_dect_phy_time_get(); 
  k_sem_take(&time_sem, K_FOREVER);

  if (iAmFt)
  {
    Ft_InfiniteLoop();
  }
  else
  {
    Pt_InfiniteLoop();
  }
}

