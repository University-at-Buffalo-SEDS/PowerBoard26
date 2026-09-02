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

TX_THREAD sensor_thread;

#define SENSOR_THREAD_STACK_SIZE (10U * 1024U)
#define SENSOR_LOG_PERIOD_TICKS (5U * TX_TIMER_TICKS_PER_SECOND)
extern I2C_HandleTypeDef hi2c2;
volatile uint32_t g_sensor_stack_remaining = 0U;

typedef struct
{
    LTC2990_Handle_t *voltage_handle;
    LTC2990_Handle_t *current_handle;
} ltc_handles_t;

/* Thread entry receives this pointer; storage must outlive create_sensor_thread(). */
static ltc_handles_t sensor_ltc_handles;

void sensor_thread_entry(ULONG entry_input)
{
    ltc_handles_t *ltc_handles = (ltc_handles_t *)(uintptr_t)entry_input;
    LTC2990_Handle_t *ltc2990_voltage_handle = ltc_handles->voltage_handle;
    LTC2990_Handle_t *ltc2990_current_handle = ltc_handles->current_handle;

    /* A missing or slow sensor must not take down board networking. The
       telemetry task has higher priority and initializes SEDSNet first; retry
       failed devices here without trapping the whole firmware. */
    while (LTC2990_Init(ltc2990_voltage_handle, &hi2c2,
                        LTC2990_I2C_ADDRESS_VOLTAGE, VOLTAGE) != 0)
    {
        (void)log_error_asynchronous("LTC2990 voltage init failed; retrying");
        tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND);
    }
    while (LTC2990_Init(ltc2990_current_handle, &hi2c2,
                        LTC2990_I2C_ADDRESS_CURRENT, CURRENT) != 0)
    {
        (void)log_error_asynchronous("LTC2990 current init failed; retrying");
        tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND);
    }
    // HAL_GPIO_WritePin(BLUE_LED_GPIO_Port, BLUE_LED_Pin, GPIO_PIN_SET);

    for (;;)
    {

        tx_thread_sleep(SENSOR_LOG_PERIOD_TICKS);
        telemetry_ltc2990_update_voltage(ltc2990_voltage_handle);
        telemetry_ltc2990_update_current(ltc2990_current_handle);
        // printf("Sensor thread loop\n");
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
                                   4,                        // priority
                                   4,                        // preemption threshold
                                   TX_NO_TIME_SLICE,
                                   TX_AUTO_START);
    HAL_GPIO_WritePin(GREEN_LED_GPIO_Port, GREEN_LED_Pin, GPIO_PIN_RESET);

    return status;
}
