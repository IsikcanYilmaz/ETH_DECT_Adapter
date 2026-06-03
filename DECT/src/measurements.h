#ifndef MEASUREMENTS
#define MEASUREMENTS

#include <stdint.h>
#include <stddef.h>
#include <nrf_modem_dect_phy.h>

typedef int (*meas_fn_t)(struct nrf_modem_dect_phy_hdr_type_1*, int);

typedef struct {
    const uint8_t *values;
    size_t count;
} sweep_param_t;

void sweep(size_t num_params,
           const sweep_param_t *params,
           uint16_t reps,
           meas_fn_t meas);

#endif
