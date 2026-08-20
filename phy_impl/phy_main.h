#ifndef PHYMAIN_H_
#define PHYMAIN_H_
#include <stdbool.h>

#define US_TO_MODEM_TICKS(us) (((uint64_t)(us)/1000)*NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ)
#define MODEM_TICKS_TO_MS(t) (t / NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ)

#define DECT_FRAME_DURATION_MS (10)
#define DECT_FRAME_DURATION_US (10000)
#define DECT_SLOTS_PER_FRAME (24)
#define DECT_SLOT_DURATION_US (417) // 416.67
#define DECT_SLOT_DURATION_TICK ((uint64_t)((DECT_SLOT_DURATION_US * NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ) / 1000))

#define DECT_GAP_US (5) // ?
#define DECT_GAP_TICK ((uint64_t) (DECT_GAP_US * NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ) / 1000)

#define DECT_GUARD_TIME (50 * DECT_GAP_TICK) // 5000 us

// #define DECT_OPS_PER_BEACON 8 //164
#define DECT_OPS_PER_BEACON 100 //164 // working

// #define DECT_MASTER_BEACON_PERIOD_TICK (10 * 24 * DECT_SLOT_DURATION_TICK) //(30 * 24 * DECT_SLOT_DURATION_TICK)
#define DECT_MASTER_BEACON_PERIOD_TICK (20 * 24 * DECT_SLOT_DURATION_TICK) // working

// #define DECT_OPS_PER_BEACON 24
// #define DECT_MASTER_BEACON_PERIOD_TICK (1 * 24 * DECT_SLOT_DURATION_TICK) 

#define IS_RX_HANDLE(x) (x == BEACON_RX_HANDLE || (x >= FT_RX_HANDLE && x < PT_TX_HANDLE) || (x >= PT_RX_HANDLE && x < TX_COMBO_HANDLE) || (x >= RX_COMBO_HANDLE && x < TEST_TX_HANDLE))
#define IS_TX_HANDLE(x) (x == BEACON_TX_HANDLE || (x >= FT_TX_HANDLE && x < FT_RX_HANDLE) || (x >= PT_TX_HANDLE && x < PT_RX_HANDLE) || (x >= TX_COMBO_HANDLE && x < RX_COMBO_HANDLE))
#define IS_COMBO_HANDLE(x) (x >= TX_COMBO_HANDLE && x < RX_COMBO_HANDLE)

#define DECT_BEACON_MAGIC_STRING ("BEAC")

// TODO decide what to do with these
typedef struct DectPacket_s
{
  
} DectPacket_t;

typedef struct DectBeaconMessage_s
{
  char magic[4]; // 4
  uint16_t ops_per_beacon; // 2
  uint16_t guard_time; // 2
} __attribute__((packed)) DectBeaconMessage_t;

typedef struct DectTimesyncMessage_s
{
  
} DectTimesyncMessage_t;

typedef struct DectKnobs_s
{
  uint32_t mcs;
  uint16_t ops_per_beacon;
} DectKnobs_t;

enum DectPtState_e
{
  PT_STATE_IDLE,
  PT_STATE_WAIT_FOR_BEACON,
  PT_STATE_SCHEDULED_DOWNLINK,
  PT_STATE_SCHEDULED_UPLINK,
  PT_STATE_FRAME_DONE,
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

extern DectKnobs_t knobs;

void DectPhy_Main(bool iAmMaster);
bool DectPhy_WiznetAlert(void); // TODO better way of doing this

// Below should only be used by ft.c and pt.c // TODO maybe make these such that they are accessible thru a struct that only pt/ft can
int DectPhy_Transmit(uint32_t handle, void *data, size_t data_len, uint64_t start_time);
int DectPhy_Receive(uint32_t handle, uint32_t durationTicks, uint64_t start_time);
int DectPhy_ReceiveContinuous(uint32_t handle, uint32_t durationTicks, uint64_t start_time); 
int DectPhy_TransmitHeadOfQueue(uint32_t handle, uint64_t start_time);
int DectPhy_TransmitHeadOfQueueAndReceive(uint32_t handle_offset, uint64_t start_time_tx, uint64_t start_time_rx, uint32_t rx_duration);
int DectPhy_TransmitBeacon(uint64_t start_time);

// Util
bool DectPhy_PktIsBeacon(char *pkt);
bool DectPhy_PktIsNone(char *pkt);
void DectPhy_EnqueueEthTx(void *pkt);
void DectPhy_InFlightCompleted(void);

#endif

