#ifndef POWER_MONITOR_RETRY_H
#define POWER_MONITOR_RETRY_H
#include <stdint.h>

/* Retry schedule for an LTC2990 that did not answer. Pure logic: no HAL or
 * RTOS calls. The first retry comes POWER_MONITOR_RETRY_MIN_MS after the
 * failure, then the delay doubles up to POWER_MONITOR_RETRY_MAX_MS. Each
 * monitor has its own schedule, so a missing voltage monitor never holds back
 * the current monitor (or the other way round). Same schedule as the gateway
 * board's power monitors. */
#define POWER_MONITOR_RETRY_MIN_MS 1000U
#define POWER_MONITOR_RETRY_MAX_MS 32000U

/* BOARD_HEALTH words 16..19: tag "LTC1", present mask (bit 0 voltage, bit 1
 * current; clear = absent, not trusted), longest current retry delay in ms
 * (0 healthy), init retries since boot. Same layout as the gateway board. */
#define POWER_MONITOR_HEALTH_TAG 0x3143544CU
#define POWER_MONITOR_VOLTAGE 1U
#define POWER_MONITOR_CURRENT 2U

typedef struct {
    uint32_t next_ms;
    uint32_t delay_ms;
    uint8_t ready;
} power_monitor_retry_t;

static inline int power_monitor_retry_due(const power_monitor_retry_t *r, uint32_t now_ms)
{
    /* delay_ms 0 while not ready: never tried yet, due at any clock value. */
    return !r->ready && (r->delay_ms == 0U || (int32_t)(now_ms - r->next_ms) >= 0);
}

static inline void power_monitor_retry_result(power_monitor_retry_t *r, uint32_t now_ms, int ok)
{
    if (ok) {
        r->ready = 1U;
        r->delay_ms = 0U;
        return;
    }
    r->ready = 0U;
    r->delay_ms = (r->delay_ms < POWER_MONITOR_RETRY_MIN_MS) ? POWER_MONITOR_RETRY_MIN_MS
                : (r->delay_ms >= POWER_MONITOR_RETRY_MAX_MS / 2U) ? POWER_MONITOR_RETRY_MAX_MS
                : r->delay_ms * 2U;
    r->next_ms = now_ms + r->delay_ms;
}

#endif
