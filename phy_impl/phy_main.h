#include <stdbool.h>

// TODO decide what to do with these
typedef struct DectPacket_s
{
  
} DectPacket_t;

typedef struct DectBeaconMessage_s
{
   
} DectBeaconMessage_t;

typedef struct DectTimesyncMessage_s
{
  
} DectTimesyncMessage_t;

enum DectPtStateMachine_e
{
  PT_STATE_WAIT_FOR_BEACON,
  PT_STATE_SCHEDULED_DOWNLINK,
  PT_STATE_SCHEDULED_UPLINK,
  PT_STATE_FRAME_DONE,
  PT_STATE_MAX,
};

struct DectInFlightPktStub_s
{
  uint32_t reserved;
  int handle;
  void *ptr;
};

void DectPhy_Main(bool iAmMaster);
bool DectPhy_WiznetAlert(void); // TODO better way of doing this
