#include <stdint.h>
#include <stdbool.h>

#define APP_DATA_LEN_MAX 256
#define APP_FLOW_ID 1
#define APP_POLL_DELAY_MS 100
#define PT_BEACON_TABLE_SIZE 20

enum app_mode {
  APP_MODE_IDLE = 0,
  APP_MODE_FT,
  APP_MODE_PT,
};

enum wait_reason {
  WAIT_NONE = 0,
  WAIT_SYSTEMMODE,
  WAIT_CONFIGURE,
  WAIT_FUNCTIONAL,
  WAIT_CLUSTER_CONFIGURE,
  WAIT_NETWORK_BEACON_CONFIGURE,
  WAIT_NETWORK_SCAN,
  WAIT_RSSI_SCAN,
  WAIT_DLC_TX,
  WAIT_NETWORK_SCAN_STOP,
  WAIT_CLUSTER_BEACON_RECEIVE_STOP,
};

enum app_event_type {
  APP_EVT_NETWORK_BEACON = 0,
  APP_EVT_CLUSTER_BEACON,
  APP_EVT_ASSOCIATION_IND,
  APP_EVT_ASSOCIATION_RELEASE,
  APP_EVT_DLC_RX,
  APP_EVT_OP_NETWORK_SCAN,
  APP_EVT_OP_CLUSTER_BEACON_RECEIVE,
  APP_EVT_OP_CLUSTER_BEACON_RECEIVE_STOP,
  APP_EVT_OP_NETWORK_SCAN_STOP,
  APP_EVT_NTF_ASSOCIATION,

  /* JON */
  APP_EVT_TX_AVAILABLE,
};

struct app_event {
  enum app_event_type type;
  union {
    struct {
      uint16_t channel;
      uint32_t network_id;
      uint32_t long_rd_id;
      uint32_t cluster_beacon_period_ms;
      int16_t rssi_dbm;
    } network_beacon;
    struct {
      uint16_t channel;
      uint32_t network_id;
      uint32_t long_rd_id;
      uint32_t cluster_beacon_period_ms;
      int16_t rssi_dbm;
    } cluster_beacon;
    struct {
      int status;
      uint32_t long_rd_id;
    } association_ind;
    struct {
      uint32_t long_rd_id;
    } association_release;
    struct {
      uint32_t long_rd_id;
      size_t len;
      char text[APP_DATA_LEN_MAX + 1];
    } dlc_rx;
    struct {
      int status;
    } op_network_scan;
    struct {
      int status;
    } op_cluster_beacon_receive;
    struct {
      int status;
    } op_cluster_beacon_receive_stop;
    struct {
      int status;
    } op_network_scan_stop;
    struct {
      int status;
      uint32_t long_rd_id;
    } ntf_association;
  };
};

/* Entry in the PT beacon discovery table. Populated by PT_SCAN. */
struct pt_beacon_entry {
  bool valid;
  uint16_t channel;
  uint32_t network_id;
  uint32_t long_rd_id;
  uint32_t cluster_beacon_period_ms;
  int16_t rssi_dbm;
};

#define MAC_MAGIC_NUMBER (0xC0C4FACE)
typedef struct DonglePktMetadata_s
{
  uint32_t magic;
  uint16_t payloadLen;
} __attribute__((packed)) DonglePktMetadata_t;

int Mac_main(bool iAmFt);
void Mac_TxReady(void);
int dect_send(enum app_mode source_mode, const char *buf, size_t len); // TODO better naming
enum app_mode Mac_Whatami(void);
bool Mac_CanTransmit(void);
