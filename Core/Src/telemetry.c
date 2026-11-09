#include "board_commissioning.h"

#ifdef TELEMETRY_ENABLED
extern int telemetry_router_enter(void);
extern int telemetry_router_leave(void);
#endif
#include "can_tx_queue.h"
#include "board_health.h"
// telemetry.c
#include "telemetry.h"
#include "av_bay_underglow.h"
#include "flight_state_cache.h"
#include "board_recovery_transport.h"
#include "sim_network_probe.h"
#include "board_can_priority.h"
#include "ota_stream.h"

#include "app_threadx.h"
#include "can_bus.h"
#include "main.h"
#include "sedsnet_config.h"
#include "board_packet_store.h"
#ifdef TELEMETRY_USE_TLSF
#include "telemetry_tlsf.h"
#endif
#include "stm32g4xx_hal.h"
#include "stm32g4xx_hal_gpio.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef TELEMETRY_ENABLED
static void print_data_no_telem(void *data, size_t len) {
  (void)data;
  (void)len;
}
#endif

#if defined(__GNUC__) || defined(__clang__)
#define UNUSED_FUNCTION __attribute__((unused))
#else
#define UNUSED_FUNCTION
#endif

#ifndef TELEMETRY_TIMESYNC_MASTER_PRIO
#define TELEMETRY_TIMESYNC_MASTER_PRIO (-1)
#endif

#ifndef TELEMETRY_TIMESYNC_SOURCE_TIMEOUT_MS
#define TELEMETRY_TIMESYNC_SOURCE_TIMEOUT_MS 5000U
#endif

#ifndef TELEMETRY_TIMESYNC_ANNOUNCE_INTERVAL_MS
#define TELEMETRY_TIMESYNC_ANNOUNCE_INTERVAL_MS 2000U
#endif

#ifndef TELEMETRY_TIMESYNC_REQUEST_INTERVAL_MS
#define TELEMETRY_TIMESYNC_REQUEST_INTERVAL_MS 2000U
#endif

#ifndef TX_TIMER_TICKS_PER_SECOND
#error "TX_TIMER_TICKS_PER_SECOND must be defined by ThreadX."
#endif

#define TELEMETRY_TIMESYNC_ROLE_CONSUMER 0U
#define TELEMETRY_TIMESYNC_ROLE_SOURCE 1U

static uint8_t g_can_rx_subscribed = 0U;
static int32_t g_can_side_id = -1;
#define BOARD_CAN_MAX_FRAME_BYTES CAN_TX_ALIGNED_FRAME_BYTES
#define BOARD_SIDE_TRANSPORT_TEMPLATES 4U
static uint8_t g_local_unix_valid = 0U;
static uint64_t g_local_unix_ms = 0ULL;

static uint8_t g_discovery_schema_announced = 0U;
static uint64_t g_discovery_schema_retry_ms = 0ULL;

#define TELEMETRY_ROUTER_RETRY_MS 500U
#define TELEMETRY_SERVICE_RETRY_MS 5000U
#define TELEMETRY_SERVICE_TIMESYNC 0x1U
#define TELEMETRY_SERVICE_OTA 0x2U
#define TELEMETRY_SERVICE_UNDERGLOW 0x4U
#define TELEMETRY_SERVICE_FLIGHT_STATE 0x8U
#define TELEMETRY_SERVICE_ALL 0xFU
#ifdef TELEMETRY_ENABLED
static uint32_t g_router_retry_after_ms;
#endif
RouterState g_router = {.r = NULL, .created = 0U, .start_time = 0ULL};

/* Exported simulator/HIL health signals. A linked-bay test requires both a
 * remote discovery topology change and a valid SEDSNet network clock. */
volatile uint32_t g_telemetry_discovery_seen = 0U;
volatile uint32_t g_telemetry_timesync_valid = 0U;
/* Network clock restarts after it disagreed with the station clock by >60 s. */
volatile uint32_t g_network_clock_resyncs = 0U;
/* A restart cannot fix a source that disagrees with the GS; repeating it on
 * every GS packet kept the clock torn down. Monotonic ticks pace it. */
#define NETWORK_CLOCK_RESYNC_HOLDOFF_TICKS (30U * (ULONG)TX_TIMER_TICKS_PER_SECOND)
static ULONG g_network_clock_resync_tick;
volatile uint32_t g_telemetry_network_ready = 0U;
/* Optional router services that failed to start (TELEMETRY_SERVICE_* bits).
 * The router, CAN side, discovery and BOARD_HEALTH keep running; failed
 * services are retried on a bounded schedule and reported in BOARD_HEALTH. */
volatile uint32_t g_telemetry_service_faults = 0U;
volatile int32_t g_telemetry_service_last_error = 0;
#ifdef TELEMETRY_ENABLED
static uint32_t g_telemetry_service_retry_ms = 0U;
static void telemetry_retry_services_locked(void);
#endif
volatile uint32_t g_telemetry_peer_mask = 0U;
volatile uint32_t g_sim_heartbeat_attempts = 0U;
volatile uint32_t g_sim_heartbeat_ok = 0U;
volatile uint32_t g_sim_heartbeat_fail = 0U;
volatile uint32_t g_sim_heartbeat_wire_tx = 0U;

static uint64_t tx_raw_now_ms_locked(void) {
  const uint32_t ticks32 = (uint32_t)tx_time_get();
  return ((uint64_t)ticks32 * 1000ULL) / (uint64_t)TX_TIMER_TICKS_PER_SECOND;
}

static UNUSED_FUNCTION uint64_t tx_raw_now_ms(void *user) {
  (void)user;
  return tx_raw_now_ms_locked();
}

static uint8_t telemetry_timesync_is_source(void) {
  return (TELEMETRY_TIMESYNC_MASTER_PRIO >= 0) ? 1U : 0U;
}

static uint64_t telemetry_timesync_priority(void) {
  // Holdover consumers must yield when RF (1) or GroundStation (50) returns.
  return telemetry_timesync_is_source() ? (uint64_t)TELEMETRY_TIMESYNC_MASTER_PRIO : 100ULL;
}

static uint32_t telemetry_timesync_role(void) {
  return telemetry_timesync_is_source() ? TELEMETRY_TIMESYNC_ROLE_SOURCE
                                        : TELEMETRY_TIMESYNC_ROLE_CONSUMER;
}

static bool telemetry_unix_ms_to_utc(uint64_t unix_ms, int32_t *year, uint8_t *month,
                                     uint8_t *day, uint8_t *hour, uint8_t *minute,
                                     uint8_t *second, uint16_t *millisecond) {
  static const uint16_t days_before_month[12] = {0U,   31U,  59U,  90U,  120U, 151U,
                                                 181U, 212U, 243U, 273U, 304U, 334U};
  uint64_t whole_seconds = unix_ms / 1000ULL;
  const uint64_t days_since_epoch = whole_seconds / 86400ULL;
  uint32_t seconds_of_day = (uint32_t)(whole_seconds % 86400ULL);
  int32_t y = 1970;
  uint64_t days = days_since_epoch;

  if (!year || !month || !day || !hour || !minute || !second || !millisecond) {
    return false;
  }

  while (1) {
    const uint32_t y_u32 = (uint32_t)y;
    const uint8_t leap =
        ((y_u32 % 4U) == 0U && ((y_u32 % 100U) != 0U || (y_u32 % 400U) == 0U)) ? 1U : 0U;
    const uint32_t days_in_year = leap ? 366U : 365U;
    if (days < days_in_year) {
      uint8_t m = 1U;
      uint32_t day_of_year = (uint32_t)days;
      for (; m <= 12U; ++m) {
        uint32_t month_start = days_before_month[m - 1U];
        uint32_t month_end =
            (m < 12U) ? days_before_month[m] : (uint32_t)(leap ? 366U : 365U);
        if (leap && m > 2U) {
          month_start += 1U;
          month_end += 1U;
        }
        if (day_of_year < month_end) {
          *year = y;
          *month = m;
          *day = (uint8_t)(day_of_year - month_start + 1U);
          *hour = (uint8_t)(seconds_of_day / 3600U);
          *minute = (uint8_t)((seconds_of_day % 3600U) / 60U);
          *second = (uint8_t)(seconds_of_day % 60U);
          *millisecond = (uint16_t)(unix_ms % 1000ULL);
          return true;
        }
      }
      return false;
    }
    days -= days_in_year;
    ++y;
  }
}

static SedsResult telemetry_apply_local_unix_time_locked(SedsRouter *router) {
  int32_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  uint16_t millisecond = 0;

  if (!router || !telemetry_timesync_is_source() || !g_local_unix_valid) {
    return SEDS_OK;
  }

  if (!telemetry_unix_ms_to_utc(g_local_unix_ms, &year, &month, &day, &hour, &minute,
                                &second, &millisecond)) {
    return SEDS_BAD_ARG;
  }

  return seds_router_set_local_network_datetime_millis(router, year, month, day, hour, minute,
                                                       second, millisecond);
}

static SedsResult telemetry_configure_timesync_locked(SedsRouter *router) {
  SedsResult result;

  if (!router) {
    return SEDS_BAD_ARG;
  }

  result = seds_router_configure_timesync(
      router, true, telemetry_timesync_role(), telemetry_timesync_priority(),
      (uint64_t)TELEMETRY_TIMESYNC_SOURCE_TIMEOUT_MS,
      (uint64_t)TELEMETRY_TIMESYNC_ANNOUNCE_INTERVAL_MS,
      (uint64_t)TELEMETRY_TIMESYNC_REQUEST_INTERVAL_MS);
  if (result != SEDS_OK) {
    return result;
  }

  return telemetry_apply_local_unix_time_locked(router);
}

uint64_t telemetry_now_ms(void) { return tx_raw_now_ms_locked(); }

static uint64_t telemetry_unix_ms_locked(void) {
#ifndef TELEMETRY_ENABLED
  return g_local_unix_valid ? g_local_unix_ms : 0ULL;
#else
  uint64_t unix_ms = 0ULL;

  if (g_router.r && seds_router_get_network_time_ms(g_router.r, &unix_ms) == SEDS_OK) {
    return unix_ms;
  }

  if (telemetry_timesync_is_source() && g_local_unix_valid) {
    return g_local_unix_ms;
  }

  return 0ULL;
#endif
}

uint64_t telemetry_unix_ms(void) {
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return 0;
#endif
  uint64_t result = telemetry_unix_ms_locked();
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_leave()) return 0;
#endif
  return result;
}


uint64_t telemetry_unix_s(void) { return telemetry_unix_ms() / 1000ULL; }

uint8_t telemetry_unix_is_valid(void) { return telemetry_unix_ms() != 0ULL ? 1U : 0U; }

static void telemetry_set_unix_time_ms_locked(uint64_t unix_ms) {
  g_local_unix_ms = unix_ms;
  g_local_unix_valid = (unix_ms != 0ULL) ? 1U : 0U;

#ifdef TELEMETRY_ENABLED
  if (g_router.r != NULL) {
    (void)telemetry_apply_local_unix_time_locked(g_router.r);
  }
#endif
}

void telemetry_set_unix_time_ms(uint64_t unix_ms) {
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return;
#endif
  telemetry_set_unix_time_ms_locked(unix_ms);
#ifdef TELEMETRY_ENABLED
  (void)telemetry_router_leave();
#endif
}


static uint64_t node_now_since_ms(void *user) {
  (void)user;
  const RouterState s = g_router;
  const uint64_t now = tx_raw_now_ms_locked();
  /* Construction and initial maintenance also need an advancing clock. */
  return now - s.start_time;
}

static SedsResult tx_send_with_priority(const uint8_t *bytes, size_t len,
                                        uint8_t priority, void *user) {
  (void)user;

  if (!bytes || len == 0U) {
    return SEDS_BAD_ARG;
  }
  sim_probe_observe_can_tx(bytes, len);
  HAL_GPIO_TogglePin(GREEN_LED_GPIO_Port, GREEN_LED_Pin);
  const uint32_t can_id =
      board_can_arbitration_id(bytes, len, priority,
          sim_probe_packed_data_type(bytes, len), (uint32_t)SEDS_DT_HEARTBEAT, 0x002U);
  return (can_bus_send_large(bytes, len, can_id) == HAL_OK) ? SEDS_OK : SEDS_IO;
}

SedsResult tx_send(const uint8_t *bytes, size_t len, void *user) {
  return tx_send_with_priority(bytes, len, 0U, user);
}


static void telemetry_can_rx(const uint8_t *data, size_t len, void *user) {
  (void)user;
  sim_probe_observe_packed(data, len);
  rx_asynchronous(data, len);
}

static SedsResult __attribute__((unused))
telemetry_sd_card_packet_handler(const SedsPacketView *pkt, void *user) {
  (void)user;

  if (!pkt) {
    return SEDS_BAD_ARG;
  }

  const int32_t needed = seds_pkt_to_string_len(pkt);
  if (needed <= 0) {
    return (SedsResult)needed;
  }

  char buf[(size_t)needed];
  const SedsResult result = seds_pkt_to_string(pkt, buf, sizeof(buf));
  if (result != SEDS_OK) {
    printf("Error: SD card packet stringify failed: %d\r\n", (int)result);
    return result;
  }

  printf("[SD_CARD] %s\r\n", buf);
  return SEDS_OK;
}

static void rx_asynchronous_locked(const uint8_t *bytes, size_t len) {
#ifndef TELEMETRY_ENABLED
  (void)bytes;
  (void)len;
  return;
#else
  if (!bytes || len == 0U) {
    return;
  }

  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return;
  }

  if (g_can_side_id >= 0) {
    (void)seds_router_receive_packed_from_side(
        g_router.r, (uint32_t)g_can_side_id, bytes, len);
  } else {
    (void)seds_router_receive_packed(g_router.r, bytes, len);
  }
  g_telemetry_discovery_seen = 1U;
#endif
}

void rx_asynchronous(const uint8_t *bytes, size_t len) {
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return;
#endif
  rx_asynchronous_locked(bytes, len);
#ifdef TELEMETRY_ENABLED
  (void)telemetry_router_leave();
#endif
}


static UNUSED_FUNCTION void rx_synchronous_locked(const uint8_t *bytes, size_t len) {
#ifndef TELEMETRY_ENABLED
  (void)bytes;
  (void)len;
  return;
#else
  if (!bytes || len == 0U) {
    return;
  }

  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return;
  }

  if (g_can_side_id >= 0) {
    (void)seds_router_receive_packed_from_side(g_router.r, (uint32_t)g_can_side_id, bytes,
                                                   len);
  } else {
    (void)seds_router_receive_packed(g_router.r, bytes, len);
  }
#endif
}

static UNUSED_FUNCTION void rx_synchronous(const uint8_t *bytes, size_t len) {
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return;
#endif
  rx_synchronous_locked(bytes, len);
#ifdef TELEMETRY_ENABLED
  (void)telemetry_router_leave();
#endif
}


static void telemetry_update_network_health(SedsRouter *router) {
  uint64_t network_time_ms = 0ULL;
  const ULONG now_ticks = tx_time_get();
  if (seds_router_get_network_time_ms(router, &network_time_ms) == SEDS_OK) {
    g_telemetry_timesync_valid = 1U;
    if ((g_network_clock_resyncs == 0U ||
         (ULONG)(now_ticks - g_network_clock_resync_tick) >= NETWORK_CLOCK_RESYNC_HOLDOFF_TICKS) &&
        board_recovery_network_clock_resync_needed(network_time_ms) &&
        telemetry_configure_timesync_locked(router) == SEDS_OK) {
      g_network_clock_resyncs++;
      g_network_clock_resync_tick = now_ticks;
      network_time_ms = 0ULL;
    }
  }
  if (g_telemetry_discovery_seen != 0U &&
      g_telemetry_timesync_valid != 0U) {
    g_telemetry_network_ready = 1U;
  }
}

static SedsResult telemetry_poll_timesync_locked(void) {
#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
  if (init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  const SedsResult result = seds_router_poll_timesync(g_router.r, NULL);
  telemetry_update_network_health(g_router.r);
  return result;
#endif
}

SedsResult telemetry_poll_timesync(void) {
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return SEDS_BAD_ARG;
#endif
  SedsResult result = telemetry_poll_timesync_locked();
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_leave() && result == SEDS_OK) return SEDS_ERR;
#endif
  return result;
}


static SedsResult telemetry_announce_discovery_locked(void) {
#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
  if (init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_announce_discovery(g_router.r);
#endif
}

SedsResult telemetry_announce_discovery(void) {
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return SEDS_BAD_ARG;
#endif
  SedsResult result = telemetry_announce_discovery_locked();
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_leave() && result == SEDS_OK) return SEDS_ERR;
#endif
  return result;
}


static SedsResult telemetry_poll_discovery_locked(void) {
#ifdef TELEMETRY_ENABLED
  if (g_router.r) board_health_poll(g_router.r);
#endif

#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
  if (init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  bool did_queue = false;
  /* Cadence polling advertises routes, not schema. Publish the complete local
   * catalog once after startup so existing relays learn newly added types.
   * Leave queue processing running between bounded retries on queue pressure. */
  const uint64_t now = tx_raw_now_ms_locked();
  if (now >= g_discovery_schema_retry_ms) {
    if (!g_discovery_schema_announced) {
      g_discovery_schema_retry_ms = now + 1000ULL;
      if (seds_router_announce_discovery(g_router.r) == SEDS_OK) {
        g_discovery_schema_announced = 1U;
      }
    }
  }
  board_commissioning_service();
  telemetry_retry_services_locked();
  (void)flight_state_cache_poll(g_router.r);
  const SedsResult result = seds_router_poll_discovery(g_router.r, &did_queue);
  if (result == SEDS_OK) {
    sim_probe_emit_heartbeat(g_router.r, telemetry_now_ms());
    (void)av_bay_underglow_poll(g_router.r);
  }
  telemetry_update_network_health(g_router.r);
  return result;
#endif
}

SedsResult telemetry_poll_discovery(void) {
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return SEDS_BAD_ARG;
#endif
  SedsResult result = telemetry_poll_discovery_locked();
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_leave() && result == SEDS_OK) return SEDS_ERR;
#endif
  return result;
}


#ifdef TELEMETRY_USE_TLSF
static __attribute__((unused)) bool telemetry_memory_admission_probe(
    size_t additional, size_t largest, uint32_t work)
{
  if (work == SEDS_MEMORY_WORK_SMALL_PROTECTED)
    return telemetry_tlsf_admit_small_protected(additional, largest);
  return telemetry_tlsf_admit(additional, largest);
}
#endif

static SedsResult Board_Recovery_handler(const SedsPacketView *packet, void *user)
{
  (void)user;
  return board_recovery_handle(g_router.r, packet, SEDS_EP_BOARD_RECOVERY,
      (uint32_t)(((uint64_t)tx_time_get() * 1000U) / TX_TIMER_TICKS_PER_SECOND));
}

#ifdef TELEMETRY_ENABLED
/* Start the optional services named in `wanted`; returns the still-failed
 * subset. Each initializer tolerates being repeated after a failure. */
static uint32_t telemetry_start_services_locked(SedsRouter *r, uint32_t wanted) {
  /* Bit i of TELEMETRY_SERVICE_* is entry i. */
  static SedsResult (*const start[4])(SedsRouter *) = {
      telemetry_configure_timesync_locked, ota_stream_init,
      av_bay_underglow_init, flight_state_cache_init};
  uint32_t failed = 0U;
  for (uint32_t i = 0U; i < 4U; i++) {
    if ((wanted & (1UL << i)) == 0U) continue;
    const SedsResult result = start[i](r);
    if (result != SEDS_OK) {
      failed |= 1UL << i;
      g_telemetry_service_last_error = (int32_t)result;
    }
  }
  g_telemetry_service_faults = (g_telemetry_service_faults & ~wanted) | failed;
  if (failed != 0U) {
    /* 1 timesync, 2 OTA stream/storage, 4 underglow, 8 flight-state cache. */
    printf("Error: failed services 0x%lx (%d)\r\n", (unsigned long)failed,
           (int)g_telemetry_service_last_error);
    g_telemetry_service_retry_ms =
        (uint32_t)tx_raw_now_ms_locked() + TELEMETRY_SERVICE_RETRY_MS;
  }
  return failed;
}

/* Bounded background retry of failed optional services (router lock held). */
static void telemetry_retry_services_locked(void) {
  if (!g_router.r || g_telemetry_service_faults == 0U) return;
  const uint32_t now = (uint32_t)tx_raw_now_ms_locked();
  if ((int32_t)(now - g_telemetry_service_retry_ms) < 0) return;
  (void)telemetry_start_services_locked(g_router.r, g_telemetry_service_faults);
}
#endif

static SedsResult init_telemetry_router_locked(void) {
#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
  SedsRouter *r = NULL;
  SedsResult result = SEDS_OK;
  const SedsLocalEndpointDesc locals[] = {
      {
          .endpoint = SEDS_EP_BOARD_RECOVERY,
          .packet_handler = Board_Recovery_handler,
          .packed_handler = NULL,
          .user = NULL,
      },
#if SEDS_FIRMWARE_SIM_TEST
      {.endpoint = SEDS_EP_HEART_BEAT,
       .packet_handler = sim_probe_heartbeat_handler,
       .packed_handler = NULL,
       .user = NULL},
#endif
      // {.endpoint = SEDS_EP_SD_CARD,
      //  .packet_handler = telemetry_sd_card_packet_handler,
      //  .packed_handler = NULL,
      //  .user = NULL},
  };

  if (g_router.created && g_router.r) {
    return SEDS_OK;
  }

  const uint64_t init_now_ms = tx_raw_now_ms_locked();
  /* Discovery and time-sync deadlines are created during router/side setup.
   * Establish the epoch first so node_now_since_ms never moves backwards once
   * those deadlines exist, including after a simulated or hardware reboot. */
  g_router.start_time = init_now_ms;

  if (!g_can_rx_subscribed) {
    if (can_bus_subscribe_rx(telemetry_can_rx, NULL) == HAL_OK) {
      g_can_rx_subscribed = 1U;
    } else {
      printf("Error: can_bus_subscribe_rx failed\r\n");
    }
  }

#ifdef TELEMETRY_USE_TLSF
  seds_set_memory_admission_probe_v2(telemetry_memory_admission_probe);
#endif
  result = board_packet_store_init();
  if (result != SEDS_OK) return result;

  r = seds_router_new(node_now_since_ms, NULL, locals,
                      sizeof(locals) / sizeof(locals[0]));
  if (!r) {
    printf("Error: failed to create router\r\n");
    g_router.r = NULL;
    g_router.created = 0U;
    g_can_side_id = -1;
    return SEDS_ERR;
  }

  if (seds_router_set_preferred_discovery_master(r, "GS", 2U) != SEDS_OK) {
    printf("Error: failed to prefer GroundStation discovery master\r\n");
    seds_router_free(r);
    return SEDS_ERR;
  }

  g_can_side_id = seds_router_add_side_packed_profile_with_priority(
      r, "can", 3U, tx_send_with_priority, NULL, false,
      SEDS_SIDE_TRANSPORT_PROFILE_IPV6_LIKE, BOARD_CAN_MAX_FRAME_BYTES, 0U,
      BOARD_SIDE_TRANSPORT_TEMPLATES);
  if (g_can_side_id < 0) {
    printf("Error: failed to add CAN side: %ld\r\n", (long)g_can_side_id);
    g_can_side_id = -1;
  }

  /* The router and its CAN side exist. Everything below is an optional
   * service: a failure is recorded and retried in the background, but it
   * must never free the router. Freeing it announced DISCOVERY_LEAVE and hid
   * this board from the ground on every 500 ms retry while the failure lasted. */
  g_discovery_schema_announced = 0U;
  g_discovery_schema_retry_ms = 0ULL;
  g_telemetry_service_faults = 0U;
  (void)telemetry_start_services_locked(r, TELEMETRY_SERVICE_ALL);
  (void)result;
  /* Publish only a complete router. RX and lazy producers share its mutex. */
  board_recovery_transport_set_router(r);
  g_router.r = r;
  g_router.created = 1U;
  return SEDS_OK;
#endif
}
SedsResult init_telemetry_router(void)
{
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return SEDS_BAD_ARG;
#endif
  SedsResult result;
#ifdef TELEMETRY_ENABLED
  const uint32_t now_ms = (uint32_t)tx_raw_now_ms_locked();
  if (g_router.created && g_router.r) {
    result = SEDS_OK;
  } else if (g_router_retry_after_ms != 0U &&
             (int32_t)(now_ms - g_router_retry_after_ms) < 0) {
    result = SEDS_IO;
  } else {
    result = init_telemetry_router_locked();
    if (result == SEDS_OK) {
      g_router_retry_after_ms = 0U;
    } else {
      g_router_retry_after_ms = now_ms + TELEMETRY_ROUTER_RETRY_MS;
      if (g_router_retry_after_ms == 0U) g_router_retry_after_ms = 1U;
    }
  }
  if (!telemetry_router_leave() && result == SEDS_OK) return SEDS_ERR;
#else
  result = init_telemetry_router_locked();
#endif
  return result;
}


static inline SedsElemKind guess_kind_from_elem_size(size_t elem_size) {
  if (elem_size == 4U || elem_size == 8U) {
    return SEDS_EK_FLOAT;
  }
  return SEDS_EK_UNSIGNED;
}

static SedsResult log_telemetry_synchronous_locked(SedsDataType data_type, const void *data,
                                     size_t element_count, size_t element_size) {
#ifdef TELEMETRY_ENABLED
  if (!data || element_count == 0U || element_size == 0U) {
    return SEDS_BAD_ARG;
  }

  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_log_typed(g_router.r, data_type, data, element_count, element_size,
                               guess_kind_from_elem_size(element_size));
#else
  (void)data_type;
  print_data_no_telem((void *)data, element_count * element_size);
  return SEDS_OK;
#endif
}

SedsResult log_telemetry_synchronous(SedsDataType data_type, const void *data,
                                     size_t element_count, size_t element_size) {
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return SEDS_BAD_ARG;
#endif
  SedsResult result = log_telemetry_synchronous_locked(data_type, data, element_count, element_size);
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_leave() && result == SEDS_OK) return SEDS_ERR;
#endif
  return result;
}


static SedsResult log_telemetry_asynchronous_locked(SedsDataType data_type, const void *data,
                                      size_t element_count, size_t element_size) {
#ifdef TELEMETRY_ENABLED
  if (!data || element_count == 0U || element_size == 0U) {
    return SEDS_BAD_ARG;
  }

  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_log_typed(g_router.r, data_type, data, element_count, element_size,
                               guess_kind_from_elem_size(element_size));
#else
  (void)data_type;
  print_data_no_telem((void *)data, element_count * element_size);
  return SEDS_OK;
#endif
}

SedsResult log_telemetry_asynchronous(SedsDataType data_type, const void *data,
                                      size_t element_count, size_t element_size) {
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return SEDS_BAD_ARG;
#endif
  SedsResult result = log_telemetry_asynchronous_locked(data_type, data, element_count, element_size);
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_leave() && result == SEDS_OK) return SEDS_ERR;
#endif
  return result;
}


static SedsResult log_telemetry_asynchronous_at_locked(SedsDataType data_type, const void *data,
                                       size_t element_count, size_t element_size,
                                       uint64_t sampled_at_ms) {
#ifdef TELEMETRY_ENABLED
  if (!data || element_count == 0U || element_size == 0U ||
      element_count > SIZE_MAX / element_size) {
    return SEDS_BAD_ARG;
  }
  if (!g_router.r && init_telemetry_router() != SEDS_OK) return SEDS_ERR;
  const uint64_t now_ms = telemetry_now_ms();
  if (sampled_at_ms > now_ms) return SEDS_BAD_ARG;
  const uint64_t age_ms = now_ms - sampled_at_ms;
  const uint64_t network_ms = telemetry_unix_ms();
  uint64_t timestamp_ms;
  if (network_ms > age_ms) {
    timestamp_ms = network_ms - age_ms;
  } else {
    if (sampled_at_ms < g_router.start_time) return SEDS_BAD_ARG;
    timestamp_ms = sampled_at_ms - g_router.start_time;
  }
  return seds_router_log_typed_ex(g_router.r, data_type, data, element_count,
                                element_size, guess_kind_from_elem_size(element_size),
                                &timestamp_ms, 0);
#else
  (void)data_type;
  (void)sampled_at_ms;
  print_data_no_telem((void *)data, element_count * element_size);
  return SEDS_OK;
#endif
}

SedsResult log_telemetry_asynchronous_at(SedsDataType data_type, const void *data,
                                       size_t element_count, size_t element_size,
                                       uint64_t sampled_at_ms) {
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return SEDS_BAD_ARG;
#endif
  SedsResult result = log_telemetry_asynchronous_at_locked(data_type, data, element_count, element_size, sampled_at_ms);
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_leave() && result == SEDS_OK) return SEDS_ERR;
#endif
  return result;
}


static SedsResult log_telemetry_string_asynchronous_locked(SedsDataType data_type, const char *str) {
#ifdef TELEMETRY_ENABLED
  if (!str) {
    return SEDS_BAD_ARG;
  }

  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_log_string_ex(g_router.r, data_type, str, strlen(str), NULL, 0);
#else
  (void)data_type;
  (void)str;
  return SEDS_OK;
#endif
}

SedsResult log_telemetry_string_asynchronous(SedsDataType data_type, const char *str) {
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return SEDS_BAD_ARG;
#endif
  SedsResult result = log_telemetry_string_asynchronous_locked(data_type, str);
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_leave() && result == SEDS_OK) return SEDS_ERR;
#endif
  return result;
}


static SedsResult dispatch_tx_queue_locked(void) {
#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_process_tx_queue(g_router.r);
#endif
}

SedsResult dispatch_tx_queue(void) {
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return SEDS_BAD_ARG;
#endif
  SedsResult result = dispatch_tx_queue_locked();
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_leave() && result == SEDS_OK) return SEDS_ERR;
#endif
  return result;
}


static SedsResult process_rx_queue_locked(void) {
#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_process_rx_queue(g_router.r);
#endif
}

SedsResult process_rx_queue(void) {
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return SEDS_BAD_ARG;
#endif
  SedsResult result = process_rx_queue_locked();
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_leave() && result == SEDS_OK) return SEDS_ERR;
#endif
  return result;
}


static SedsResult dispatch_tx_queue_timeout_locked(uint32_t timeout_ms) {
#ifndef TELEMETRY_ENABLED
  (void)timeout_ms;
  return SEDS_OK;
#else
  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_process_tx_queue_with_timeout(g_router.r, timeout_ms);
#endif
}

SedsResult dispatch_tx_queue_timeout(uint32_t timeout_ms) {
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return SEDS_BAD_ARG;
#endif
  SedsResult result = dispatch_tx_queue_timeout_locked(timeout_ms);
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_leave() && result == SEDS_OK) return SEDS_ERR;
#endif
  return result;
}


static SedsResult process_rx_queue_timeout_locked(uint32_t timeout_ms) {
#ifndef TELEMETRY_ENABLED
  (void)timeout_ms;
  return SEDS_OK;
#else
  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_process_rx_queue_with_timeout(g_router.r, timeout_ms);
#endif
}

SedsResult process_rx_queue_timeout(uint32_t timeout_ms) {
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return SEDS_BAD_ARG;
#endif
  SedsResult result = process_rx_queue_timeout_locked(timeout_ms);
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_leave() && result == SEDS_OK) return SEDS_ERR;
#endif
  return result;
}


static SedsResult process_all_queues_timeout_locked(uint32_t timeout_ms) {
#ifndef TELEMETRY_ENABLED
  (void)timeout_ms;
  return SEDS_OK;
#else
  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_process_all_queues_with_timeout(g_router.r, timeout_ms);
#endif
}

SedsResult process_all_queues_timeout(uint32_t timeout_ms) {
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return SEDS_BAD_ARG;
#endif
  SedsResult result = process_all_queues_timeout_locked(timeout_ms);
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_leave() && result == SEDS_OK) return SEDS_ERR;
#endif
  return result;
}


static SedsResult log_error_impl_locked(uint8_t queue, const char *fmt, va_list args) {
  va_list args_copy;
  int len = 0;
  int written = 0;

  if (!fmt) {
    return SEDS_BAD_ARG;
  }

  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  va_copy(args_copy, args);
  len = vsnprintf(NULL, 0U, fmt, args_copy);
  va_end(args_copy);

  if (len < 0) {
    const char *empty = "";
    return seds_router_log_string_ex(g_router.r, SEDS_DT_TELEMETRY_ERROR, empty, 0U, NULL, queue);
  }

  if (len > 512) {
    len = 512;
  }

  char buf[(size_t)len + 1U];
  written = vsnprintf(buf, (size_t)len + 1U, fmt, args);
  if (written < 0) {
    const char *empty = "";
    return seds_router_log_string_ex(g_router.r, SEDS_DT_TELEMETRY_ERROR, empty, 0U, NULL, queue);
  }

  return seds_router_log_string_ex(g_router.r, SEDS_DT_TELEMETRY_ERROR, buf, (size_t)written,
                                   NULL, queue);
}

static SedsResult log_error_impl(uint8_t queue, const char *fmt, va_list args) {
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_enter()) return SEDS_BAD_ARG;
#endif
  SedsResult result = log_error_impl_locked(queue, fmt, args);
#ifdef TELEMETRY_ENABLED
  if (!telemetry_router_leave() && result == SEDS_OK) return SEDS_ERR;
#endif
  return result;
}


SedsResult log_error_asynchronous(const char *fmt, ...) {
#ifndef TELEMETRY_ENABLED
  (void)fmt;
  return SEDS_OK;
#else
  va_list args;
  SedsResult result;

  va_start(args, fmt);
  result = log_error_impl(1U, fmt, args);
  va_end(args);
  return result;
#endif
}

SedsResult log_error_synchronous(const char *fmt, ...) {
#ifndef TELEMETRY_ENABLED
  (void)fmt;
  return SEDS_OK;
#else
  va_list args;
  SedsResult result;

  va_start(args, fmt);
  result = log_error_impl(0U, fmt, args);
  va_end(args);
  return result;
#endif
}

SedsResult log_error_asyncronous(const char *fmt, ...) {
#ifndef TELEMETRY_ENABLED
  (void)fmt;
  return SEDS_OK;
#else
  va_list args;
  SedsResult result;

  va_start(args, fmt);
  result = log_error_impl(1U, fmt, args);
  va_end(args);
  return result;
#endif
}

SedsResult log_error_syncronous(const char *fmt, ...) {
#ifndef TELEMETRY_ENABLED
  (void)fmt;
  return SEDS_OK;
#else
  va_list args;
  SedsResult result;

  va_start(args, fmt);
  result = log_error_impl(0U, fmt, args);
  va_end(args);
  return result;
#endif
}

SedsResult print_telemetry_error(const int32_t error_code) {
#ifndef TELEMETRY_ENABLED
  (void)error_code;
  return SEDS_OK;
#else
  const int32_t need = seds_error_to_string_len(error_code);
  if (need <= 0) {
    return (SedsResult)need;
  }

  char buf[(size_t)need];
  SedsResult res = seds_error_to_string(error_code, buf, sizeof(buf));
  if (res == SEDS_OK) {
    printf("Error: %s\r\n", buf);
  } else {
    (void)log_error_asynchronous("Error: seds_error_to_string failed: %d\r\n", (int)res);
  }

  return res;
#endif
}

void die(const char *fmt, ...) {
  char buf[128];
  va_list args;

  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  while (1) {
    printf("FATAL: %s\r\n", buf);
    HAL_Delay(1000);
  }
}
