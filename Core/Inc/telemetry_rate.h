#ifndef PB_TELEMETRY_RATE_H
#define PB_TELEMETRY_RATE_H
#include <stdint.h>

/* Board-local build setting; defaults to one sample every five seconds.
 * Override here or with -DPB_TELEMETRY_RATE_HZ=... and rebuild/reflash.
 * Fractional Hz is supported. This is not a network variable. */
#ifndef PB_TELEMETRY_RATE_HZ
#define PB_TELEMETRY_RATE_HZ 0.2
#endif
_Static_assert(PB_TELEMETRY_RATE_HZ >= 0.001 && PB_TELEMETRY_RATE_HZ <= 1000,
               "PB_TELEMETRY_RATE_HZ must be between 0.001 and 1000");

static inline uint32_t pb_telemetry_period_ms(void)
{
    return (uint32_t)(1000.0 / PB_TELEMETRY_RATE_HZ + 0.5);
}

static inline uint32_t pb_telemetry_period_ticks(uint32_t ticks_per_second)
{
    uint32_t ticks = (uint32_t)(((uint64_t)pb_telemetry_period_ms() *
                                ticks_per_second + 999U) / 1000U);
    return ticks ? ticks : 1U;
}
#endif
