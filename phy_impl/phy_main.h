#ifndef PHYMAIN_H_
#define PHYMAIN_H_
#include <stdbool.h>

#define US_TO_MODEM_TICKS(us) (((uint64_t)(us) * NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ) / 1000)
#define MODEM_TICKS_TO_MS(t) (t / NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ)

// Yanked from dect_shell sample code
#define DECT_RADIO_FRAME_DURATION_US		(10000)
#define DECT_RADIO_FRAME_DURATION_MS		(10)
#define DECT_RADIO_SLOT_DURATION_US		((double)DECT_RADIO_FRAME_DURATION_US / 24)
#define DECT_RADIO_SLOT_DURATION_IN_MODEM_TICKS (US_TO_MODEM_TICKS(DECT_RADIO_SLOT_DURATION_US))
#define DECT_RADIO_SUBSLOT_DURATION_IN_MODEM_TICKS ((DECT_RADIO_SLOT_DURATION_IN_MODEM_TICKS) / 2) /* Note: assumes that mu = 1 */

#define DECT_FRAME_DURATION_MS (10)
#define DECT_FRAME_DURATION_US (10000)
#define DECT_SLOTS_PER_FRAME (24)
#define DECT_SLOT_DURATION_US (417) // 416.67

#define DECT_SLOT_DURATION_TICK (28800)
#define DECT_HALF_SLOT_DURATION_TICK (14400)

#define DECT_HEADROOM (US_TO_MODEM_TICKS(300)) // 300 us
#define DECT_HALF_HEADROOM (DECT_HEADROOM/2) 
#define DECT_QUART_HEADROOM (DECT_HEADROOM/4)

// #define DECT_OPS_PER_BEACON 8 //164
#define DECT_OPS_PER_BEACON (DECT_SLOTS_PER_FRAME * 10) //164 // working

// #define DECT_MASTER_BEACON_PERIOD_TICK (10 * 24 * DECT_SLOT_DURATION_TICK) //(30 * 24 * DECT_SLOT_DURATION_TICK)
#define DECT_MASTER_BEACON_PERIOD_TICK (20 * 24 * DECT_SLOT_DURATION_TICK) // working

#define IS_RX_HANDLE(x) (x == BEACON_RX_HANDLE || (x >= FT_RX_HANDLE && x < PT_TX_HANDLE) || (x >= PT_RX_HANDLE && x < TX_COMBO_HANDLE) || (x >= RX_COMBO_HANDLE && x < TEST_TX_HANDLE))
#define IS_TX_HANDLE(x) (x == BEACON_TX_HANDLE || (x >= FT_TX_HANDLE && x < FT_RX_HANDLE) || (x >= PT_TX_HANDLE && x < PT_RX_HANDLE) || (x >= TX_COMBO_HANDLE && x < RX_COMBO_HANDLE))
#define IS_COMBO_HANDLE(x) (x >= TX_COMBO_HANDLE && x < RX_COMBO_HANDLE)

#define DECT_BEACON_MAGIC_STRING ("BEAC")

// DECT MAC Message structures
// This is the frame structure that we encapsulate every piece of data we send over DECT with
#define DECT_DATA_PACKET_FLAG_BIT 0
#define DECT_BEACON_FLAG_BIT 1
#define DECT_FRAG_HEADER_EXISTS_FLAG_BIT 2
typedef struct DectPacket_s
{
  uint8_t flags;
  uint8_t payloadSize; // This includes every header + payload data EXCEPT this header. so the size of the payload char array below
  char payload[];
} __attribute__((packed)) DectPacket_t;

// JON Follow
// Fragmentation. Multiplexing and assembly DECT 2020 MAC Layer, section 6.3.4 MAC multiplexing header 
// We currently dont feel the need to follow this, since the top MCS we can get is 4, and per slot we can pack 117 bytes
typedef struct DectFragmentationHeader_s // Following loosely the rfc4944 https://www.rfc-editor.org/info/rfc4944/#section-5.3
{
  uint16_t datagramSize;   // Size of the higher layer datagram (after IP fragmentation) 
  uint16_t datagramOffset; // Byte offset for this fragment
  // uint8_t datagramTag;    // A tag for the current datagram being transmitted / fragmented // We can take this in later
} __attribute__((packed)) DectFragmentationHeader_t;

typedef struct DectBeaconMessage_s
{
  char magic[4]; // 4
  uint16_t ops_per_beacon; // 2 // TODO either this or ticks until next should go
  uint64_t this_beacon_time; // 8
  uint32_t modem_ticks_until_next_beacon; // 4 // NOTE since 32bit, it supports 62.13 seconds max
                                          // tbh the pt could infer this by itself also but idk
} __attribute__((packed)) DectBeaconMessage_t;

typedef struct DectKnobs_s
{
  uint32_t mcs;
  uint16_t ops_per_beacon;
} DectKnobs_t;

enum DectPtState_e
{
  PT_STATE_IDLE,
  PT_STATE_WAIT_FOR_BEACON,
  PT_STATE_WAIT_FOR_LATCH_BEACON,
  PT_STATE_SCHEDULED_DOWNLINK,
  PT_STATE_SCHEDULED_UPLINK,
  PT_STATE_FRAME_DONE,
  PT_STATE_TEST,
  PT_STATE_MAX,
};

enum DectFtState_e
{
  FT_STATE_IDLE,
  FT_STATE_SCHEDULED_BEACON,
  FT_STATE_SCHEDULED_DOWNLINK,
  FT_STATE_SCHEDULED_UPLINK,
  FT_STATE_MAX,
};

struct DectInFlightPktStub_s
{
  uint32_t reserved;
  int handle;
  void *ptr;
};

enum DectOperationHandle_e
{
  BEACON_TX_HANDLE = 0,
  BEACON_RX_HANDLE = 1,
  BEACON_LATCH_RX_HANDLE = 2,
  FT_TX_HANDLE = 2000,
  FT_RX_HANDLE = 3000,
  PT_TX_HANDLE = 4000,
  PT_RX_HANDLE = 5000,
  TX_COMBO_HANDLE = 6000,
  RX_COMBO_HANDLE = 7000, 
  TEST_TX_HANDLE = 9998,
  TEST_RX_HANDLE = 9999,
  GARBAGE_HANDLE = 10000,
  MAX_HANDLE = 0xffff
};

enum DectOperationHandleType_e
{
  BEACON_TX_HANDLE_TYPE,
  BEACON_RX_HANDLE_TYPE,
  TX_HANDLE_TYPE,
  RX_HANDLE_TYPE,
  MAX_HANDLE_TYPE,
};

enum DectPacketLengthType_e
{
  DECT_PACKET_LENGTH_SUBSLOT = 0,
  DECT_PACKET_LENGTH_SLOT = 1
};

// TODO find a better place to put these or just do something else. When we have better clarity about the hw/fw situation
extern volatile uint64_t modem_time;
extern volatile enum DectPtState_e ptState;
extern volatile enum DectFtState_e ftState;
extern volatile bool warmedUp;
extern uint32_t numSlotsInFrame;
extern uint32_t slotCounter;

extern uint32_t frameCounter;

extern uint64_t lastPccModemTick;
extern uint64_t lastPdcModemTick;
extern uint64_t lastBeaconModemTick;
extern uint64_t lastLoopModemTick;
extern uint64_t pccPdcDiff;

extern uint32_t lastBeaconTs;

extern struct nrf_modem_dect_phy_latency_info latencyInfo;
extern uint32_t opTransitionLatency; 
extern uint32_t opStartupLatency;
extern uint32_t tx_idleToActiveLatency;
extern uint32_t tx_activeToIdleLatency;
extern uint32_t rx_idleToActiveLatency;
extern uint32_t rx_activeToIdleLatency;

extern uint32_t blockTicks; // 1 SLOT TICKS + 1 op Trans

extern volatile uint64_t lastBeaconCplt;
extern volatile uint64_t lastTxCplt;
extern volatile uint64_t lastRxCplt;

extern volatile uint64_t beaconDelta; // Counted at the ends of operations
extern volatile uint64_t txDelta; // Counted at the ends of operations only when counter is > 1
extern volatile uint64_t rxDelta;

extern uint64_t genericBeaconScheduleOffset;
extern uint64_t genericTxScheduleOffset;
extern uint64_t genericRxScheduleOffset;
extern uint64_t genericRxDuration;
extern uint64_t genericRelativeRxSchedule;

extern const struct gpio_dt_spec *beaconTxSwitch;
extern const struct gpio_dt_spec *beaconRxSwitch;
extern const struct gpio_dt_spec *dlSwitch;
extern const struct gpio_dt_spec *ulSwitch;

extern const struct gpio_dt_spec *ptDlSwitch;
extern const struct gpio_dt_spec *ptUlSwitch;

extern struct k_sem operation_sem;
extern struct k_sem time_sem;
extern struct k_sem done_sem; 
extern struct k_sem resync_sem;

extern sys_slist_t ops_list;

extern DectKnobs_t knobs;

void DectPhy_Main(bool iAmFt);
bool DectPhy_WiznetAlert(void); // TODO better way of doing this

// Below should only be used by ft.c and pt.c // TODO maybe make these such that they are accessible thru a struct that only pt/ft can
int DectPhy_Transmit(uint32_t handle, void *data, size_t data_len, uint64_t start_time);
int DectPhy_Receive(uint32_t handle, uint32_t durationTicks, uint64_t start_time);
int DectPhy_ReceiveContinuous(uint32_t handle, uint32_t durationTicks, uint64_t start_time); 
int DectPhy_TransmitHeadOfQueue(uint32_t handle, uint64_t start_time);
int DectPhy_TransmitBeacon(uint64_t start_time);
int DectPhy_HandleIncomingPacketFragment(char *data, size_t len);

// Util
bool DectPhy_PktIsBeacon(char *pkt);
bool DectPhy_PktIsNone(char *pkt);
void DectPhy_EnqueueEthTx(void *pkt);
void DectPhy_InFlightCompleted(void);
int DectPhy_CancelAllPendingOps(void);

#endif

