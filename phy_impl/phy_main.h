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

void DectPhy_Main(bool iAmMaster);
bool DectPhy_WiznetAlert(void); // TODO better way of doing this
