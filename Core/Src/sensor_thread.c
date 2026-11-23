#include "board_watchdog.h"
// sensor_thread.c (or telemetry_thread.c)
#include <stdint.h>
#include "PB-Threads.h"
#include "tx_api.h"
#include "tx_thread.h"
#include "telemetry.h"
#include "ltc2990.h"
#include "main.h"
#include <stdio.h>
#include "can_bus.h"
#include "telemetry_rate.h"
#include "power_monitor_retry.h"

TX_THREAD sensor_thread;

#define SENSOR_THREAD_STACK_SIZE (3U * 1024U)
/* Voltage and current change slowly. Keeping fresh samples at 0.2 Hz leaves
 * radio/CAN capacity for discovery, commands, managed variables, and ACKs. */
#define SENSOR_LOG_PERIOD_TICKS pb_telemetry_period_ticks(TX_TIMER_TICKS_PER_SECOND)
extern I2C_HandleTypeDef hi2c2;
volatile uint32_t g_sensor_thread_entered __attribute__((used, externally_visible)) = 0U;
volatile uint32_t g_sensor_stack_remaining = SENSOR_THREAD_STACK_SIZE;

static void sensor_update_stack_profile(void)
{
    _tx_thread_stack_analyze(&sensor_thread);
    const uint32_t remaining = (uint32_t)(
        (uintptr_t)sensor_thread.tx_thread_stack_highest_ptr -
        (uintptr_t)sensor_thread.tx_thread_stack_start);
    if (remaining < g_sensor_stack_remaining) {
        g_sensor_stack_remaining = remaining;
    }
}

typedef struct
{
    LTC2990_Handle_t *voltage_handle;
    LTC2990_Handle_t *current_handle;
} ltc_handles_t;

/* Thread entry receives this pointer; storage must outlive create_sensor_thread(). */
static ltc_handles_t sensor_ltc_handles;

/* Power-monitor health for BOARD_HEALTH words 16..19 (power_monitor_retry.h). */
volatile uint32_t g_power_ltc2990_present;
volatile uint32_t g_power_ltc2990_retry_delay_ms;
volatile uint32_t g_power_ltc2990_retries;
static power_monitor_retry_t sensor_voltage_retry, sensor_current_retry;

static void sensor_try_init(power_monitor_retry_t *retry, LTC2990_Handle_t *handle,
                            uint8_t address, LTC2990_ROLE role, uint32_t now_ms)
{
    if (!power_monitor_retry_due(retry, now_ms)) return;
    if (retry->delay_ms != 0U) g_power_ltc2990_retries++;
    power_monitor_retry_result(retry, now_ms,
                               LTC2990_Init(handle, &hi2c2, address, role) == 0);
}

/* One acquisition pass. Each monitor that has not answered yet is retried on
 * its own backoff; each monitor that answers is sampled. A missing voltage
 * monitor never blocks the current monitor, and neither blocks the board. */
static void sensor_service_monitors(LTC2990_Handle_t *voltage, LTC2990_Handle_t *current,
                                    uint32_t now_ms, int sample)
{
    sensor_try_init(&sensor_voltage_retry, voltage, LTC2990_I2C_ADDRESS_VOLTAGE, VOLTAGE, now_ms);
    sensor_try_init(&sensor_current_retry, current, LTC2990_I2C_ADDRESS_CURRENT, CURRENT, now_ms);
    g_power_ltc2990_present = (sensor_voltage_retry.ready ? POWER_MONITOR_VOLTAGE : 0U) |
                              (sensor_current_retry.ready ? POWER_MONITOR_CURRENT : 0U);
    g_power_ltc2990_retry_delay_ms = sensor_voltage_retry.delay_ms > sensor_current_retry.delay_ms
        ? sensor_voltage_retry.delay_ms : sensor_current_retry.delay_ms;
    if (!sample) return;
    if (sensor_voltage_retry.ready) telemetry_ltc2990_update_voltage(voltage);
    if (sensor_current_retry.ready) telemetry_ltc2990_update_current(current);
}

static uint32_t sensor_now_ms(void)
{
    return (uint32_t)(((uint64_t)tx_time_get() * 1000U) / TX_TIMER_TICKS_PER_SECOND);
}

void sensor_thread_entry(ULONG entry_input)
{
    g_sensor_thread_entered++;
    sensor_update_stack_profile();
    ltc_handles_t *ltc_handles = (ltc_handles_t *)(uintptr_t)entry_input;
    LTC2990_Handle_t *ltc2990_voltage_handle = ltc_handles->voltage_handle;
    LTC2990_Handle_t *ltc2990_current_handle = ltc_handles->current_handle;

    /* One attempt each at startup; a monitor that does not answer is marked
     * absent in BOARD_HEALTH and retried in the loop with backoff. */
    sensor_service_monitors(ltc2990_voltage_handle, ltc2990_current_handle, sensor_now_ms(), 0);

    for (;;)
    {
        sensor_update_stack_profile();
        board_watchdog_progress(BOARD_WATCHDOG_ACQUISITION);
        tx_thread_sleep(SENSOR_LOG_PERIOD_TICKS);
        sensor_service_monitors(ltc2990_voltage_handle, ltc2990_current_handle, sensor_now_ms(), 1);
    }
}

UINT create_sensor_thread(TX_BYTE_POOL *byte_pool, LTC2990_Handle_t *ltc2990_voltage_handle_ptr, LTC2990_Handle_t *ltc2990_current_handle_ptr)
{
    CHAR *pointer;

    if (tx_byte_allocate(byte_pool, (VOID **)&pointer,
                         SENSOR_THREAD_STACK_SIZE, TX_NO_WAIT) != TX_SUCCESS)
    {
        return TX_POOL_ERROR;
    }

    // HAL_GPIO_WritePin(BLUE_LED_GPIO_Port, BLUE_LED_Pin, GPIO_PIN_SET);
    /* Keep entry argument data in persistent storage (not stack local). */
    sensor_ltc_handles = (ltc_handles_t){
        .voltage_handle = ltc2990_voltage_handle_ptr,
        .current_handle = ltc2990_current_handle_ptr};
    // HAL_GPIO_WritePin(BLUE_LED_GPIO_Port, BLUE_LED_Pin, GPIO_PIN_RESET);
    // HAL_GPIO_WritePin(GREEN_LED_GPIO_Port, GREEN_LED_Pin, GPIO_PIN_SET);

    UINT status = tx_thread_create(&sensor_thread,
                                   "Sensor Thread",
                                   sensor_thread_entry,
                                   (ULONG)(uintptr_t)&sensor_ltc_handles,
                                   pointer,
                                   SENSOR_THREAD_STACK_SIZE, // must match allocation size
                                   2,                        // priority
                                   2,                        // preemption threshold
                                   TX_NO_TIME_SLICE,
                                   TX_AUTO_START);
    HAL_GPIO_WritePin(GREEN_LED_GPIO_Port, GREEN_LED_Pin, GPIO_PIN_RESET);

    return status;
}
