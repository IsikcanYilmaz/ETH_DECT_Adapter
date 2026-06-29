#include "debug_log.h"
#include "tusb.h"
#include <stdio.h>
#include <string.h>

#if DEBUG_LOG_ENABLED

// CDC interface index for debug logs (only one CDC, so 0)
#define DEBUG_CDC_ITF   0

static volatile bool s_enabled = false;

void debug_log_init(void) {
    s_enabled = true;
}

void debug_log_set_enabled(bool enabled) {
    s_enabled = enabled;
}

bool debug_log_is_enabled(void) {
    return s_enabled;
}

// Service CDC: drain TX FIFO and discard any RX (keeps host happy)
void debug_log_task(void) {
    if (tud_cdc_n_available(DEBUG_CDC_ITF)) {
        uint8_t buf[64];
        tud_cdc_n_read(DEBUG_CDC_ITF, buf, sizeof(buf));
        // (Could parse commands here — e.g., toggle logging)
    }
    tud_cdc_n_write_flush(DEBUG_CDC_ITF);
}

void debug_log_printf(const char *fmt, ...) {
    if (!s_enabled) return;
    if (!tud_cdc_n_connected(DEBUG_CDC_ITF)) return;  // no terminal attached

    char line[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    if (n <= 0) return;
    if (n > (int)sizeof(line)) n = sizeof(line);

    // Write in chunks that fit in TinyUSB's FIFO
    const char *p = line;
    int remaining = n;
    while (remaining > 0) {
        uint32_t avail = tud_cdc_n_write_available(DEBUG_CDC_ITF);
        if (avail == 0) {
            tud_cdc_n_write_flush(DEBUG_CDC_ITF);
            break;  // drop rather than block — USB debug must never stall logic
        }
        uint32_t chunk = (uint32_t)remaining < avail ? (uint32_t)remaining : avail;
        tud_cdc_n_write(DEBUG_CDC_ITF, p, chunk);
        p += chunk;
        remaining -= (int)chunk;
    }
    tud_cdc_n_write_flush(DEBUG_CDC_ITF);
}

#else // DEBUG_LOG_ENABLED == 0
void debug_log_init(void) {}
void debug_log_set_enabled(bool e) { (void)e; }
bool debug_log_is_enabled(void) { return false; }
void debug_log_task(void) {}
void debug_log_printf(const char *fmt, ...) { (void)fmt; }

#endif

