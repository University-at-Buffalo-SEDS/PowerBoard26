#include "board_health.h"
#include "board_temperature.h"
#include "board_uptime.h"
#include "board_watchdog.h"
#include "power_monitor_retry.h"
#include "telemetry.h"
#include "main.h"
#include <stdint.h>
#include <stdbool.h>
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "Board diagnostic words require little-endian encoding"
#endif
#ifdef TELEMETRY_USE_TLSF
extern volatile uint32_t g_telemetry_tlsf_live_bytes, g_telemetry_tlsf_peak_bytes, g_telemetry_tlsf_region_bytes, g_telemetry_tlsf_failures;
#endif
extern uint32_t board_health_can_rx_drops(void);
extern volatile uint32_t g_telemetry_service_faults;
extern volatile uint32_t g_power_ltc2990_present, g_power_ltc2990_retry_delay_ms, g_power_ltc2990_retries;
extern int telemetry_router_enter(void);
extern int telemetry_router_leave(void);

void board_health_poll(SedsRouter *router)
{
#ifdef TELEMETRY_ENABLED
  static uint32_t last_report_ms;
  static uint32_t failures;
  if (!router) return;
  const uint64_t uptime = board_uptime_ticks();
  const uint32_t now = (uint32_t)uptime;
  if ((uint32_t)(now - last_report_ms) < 5000U) return;
  last_report_ms = now;
  board_temperature_poll();

  uint32_t words[24] = {1U, 2U, (uint32_t)uptime, (uint32_t)(uptime >> 32U), BOARD_UPTIME_FREQUENCY_HZ};
  const uint32_t saved = __get_PRIMASK(); __disable_irq();
  words[5] = g_watchdog_reset_flags;
  /* Required watchdog tasks overdue (no check-in for BOARD_WATCHDOG_OVERDUE_MS)
   * in bits 0..15; optional router services that failed to start (telemetry.c
   * TELEMETRY_SERVICE_*) in bits 16..23. */
  words[6] = (board_watchdog_overdue_mask() & 0xFFFFU) |
             ((g_telemetry_service_faults & 0xFFU) << 16U);
#ifdef TELEMETRY_USE_TLSF
  /* Exact existing occupied-byte counters. Avoid interrupt-disabled heap
   * walks, and do not mislabel occupied headroom as contiguous free space. */
  words[1] |= 1U;
  words[7] = g_telemetry_tlsf_live_bytes; words[8] = g_telemetry_tlsf_peak_bytes;
  words[9] = g_telemetry_tlsf_region_bytes; words[10] = g_telemetry_tlsf_failures;
#endif
  words[12] = board_health_can_rx_drops();
  __set_PRIMASK(saved);
  words[11] = (uint32_t)seds_memory_admission_rejected();
  words[23] = failures;
  words[1] |= board_temperature_health_flags();
  /* Power monitors: answering mask, retry delay, retries (power_monitor_retry.h). */
  words[16] = POWER_MONITOR_HEALTH_TAG; words[17] = g_power_ltc2990_present;
  words[18] = g_power_ltc2990_retry_delay_ms; words[19] = g_power_ltc2990_retries;
  if (!telemetry_router_enter()) { ++failures; return; }
  /* Network time quality in word 6 bits 24..31 (SEDSnet health byte). */
  {
    SedsTimeSyncStatus ts;
    if (seds_router_get_timesync_status(router, &ts) == SEDS_OK) {
      words[6] |= (uint32_t)ts.health_byte << 24U;
      /* Word 22 "SY": stratum (1 = the GS, 2 = RF re-serving it over CAN)
       * in bits 8..15 beside the same health byte, as on the FC. */
      words[22] = (0x5359U << 16U) | ((uint32_t)ts.stratum << 8U) | ts.health_byte;
    }
  }
  /* Static UInt32 schema + exact little-endian bytes: use the existing
   * bounded byte path without pulling every generic numeric converter in. */
  const SedsResult result = seds_router_log_bytes_ex(router, SEDS_DT_BOARD_HEALTH,
      (const uint8_t *)words, sizeof(words), NULL, true);
  const int released = telemetry_router_leave();
  if (result != SEDS_OK || !released) ++failures;
#else
  (void)router;
#endif
}
