#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>

// ---- COMPILE-TIME SWITCH ----
// Set to 0 to completely remove debug logging from the binary
// (no CDC traffic, no printf, zero overhead).
#define DBG_LOG_ENABLED 1
#ifndef DBG_LOG_ENABLED
#define DBG_LOG_ENABLED 1
#endif

#define DBG1_LOG_ENABLED 1
#ifndef DBG1_LOG_ENABLED
#define DBG1_LOG_ENABLED 1
#endif

#define INFO_LOG_ENABLED 1
#ifndef INFO_LOG_ENABLED
#define INFO_LOG_ENABLED 1
#endif


// ---- RUNTIME SWITCH ----
// Toggle at runtime without recompiling.
void debug_log_set_enabled(bool enabled);
bool debug_log_is_enabled(void);

// Call once from main()
void debug_log_init(void);

// Call from the main loop (services the CDC FIFO)
void debug_log_task(void);

// Printf-style logging
void debug_log_printf(const char *fmt, ...);

// Convenience macros — zero-cost when DBG_LOG_ENABLED=0
#if DBG_LOG_ENABLED
    #define DBG(fmt, ...)   debug_log_printf("[DBG] " fmt "\r\n", ##__VA_ARGS__)
#else
    #define DBG(fmt, ...)   ((void)0)
#endif

// Convenience macros — zero-cost when DBG_LOG_ENABLED=0
#if DBG1_LOG_ENABLED
    #define DBG1(fmt, ...)   debug_log_printf("[DBG1] " fmt "\r\n", ##__VA_ARGS__)
#else
    #define DBG1(fmt, ...)   ((void)0)
#endif

#if INFO_LOG_ENABLED
    #define INFO(fmt, ...)  debug_log_printf("[INF] " fmt "\r\n", ##__VA_ARGS__)
    #define WARN(fmt, ...)  debug_log_printf("[WRN] " fmt "\r\n", ##__VA_ARGS__)
    #define ERR(fmt, ...)   debug_log_printf("[ERR] " fmt "\r\n", ##__VA_ARGS__)
#else
    #define INFO(fmt, ...)  ((void)0)
    #define WARN(fmt, ...)  ((void)0)
    #define ERR(fmt, ...)   ((void)0)
#endif



#endif

