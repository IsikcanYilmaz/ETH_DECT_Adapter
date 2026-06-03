#include "measurements.h"

typedef int (*meas_fn_t)(struct nrf_modem_dect_phy_hdr_type_1*, int);
typedef struct {
    const uint8_t *values;
    size_t count;
} sweep_param_t;

void sweep(size_t num_params,
           const sweep_param_t *params,
           uint16_t reps,
           meas_fn_t meas)
{
    size_t indices[num_params];

    for (size_t i = 0; i < num_params; i++) {
        indices[i] = 0;
    }

    while (1) {
        uint8_t mcs = params[0].values[indices[0]];
        uint8_t ss  = params[1].values[indices[1]];
        uint8_t pow = params[2].values[indices[2]];

        struct nrf_modem_dect_phy_hdr_type_1 header = {
            .packet_length      = ss,
            .packet_length_type = 0x00,
            .header_format      = 0x0,
            .short_network_id   = (CONFIG_NETWORK_ID & 0xff),
            .transmitter_id_hi  = (device_id >> 8) & 0xff,
            .transmitter_id_lo  = (device_id & 0xff),
            .df_mcs             = mcs,
            .reserved           = 0,
            .transmit_power     = pow,
        };

        int payload_len = get_bit_count(mcs, ss) / 8;
        if (payload_len > 0) {
            for (uint16_t r = 0; r < reps; r++) {
                meas(&header, payload_len);
            }
        }

        for (int i = num_params - 1; i >= 0; i--) {
            indices[i]++;
            if (indices[i] < params[i].count) {
                break;
            } else {
                indices[i] = 0;
                if (i == 0) {
                    return; // done
                }
            }
        }
    }
}