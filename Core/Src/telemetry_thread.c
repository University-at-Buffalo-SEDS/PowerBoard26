// telemetry_thread.c
#include "PB-Threads.h"
#include "tx_api.h"
#include "tx_thread.h"
#include "telemetry.h"
#include "ota_stream.h"
#include "can_bus.h"
#include "main.h"

TX_THREAD telemetry_thread;
extern TX_THREAD sensor_thread;
extern volatile uint32_t g_sensor_stack_remaining;
volatile uint32_t g_telemetry_thread_entered __attribute__((used, externally_visible)) = 0U;
volatile uint32_t g_telemetry_stack_used __attribute__((used, externally_visible)) = 0U;
volatile uint32_t g_telemetry_stack_remaining __attribute__((used, externally_visible)) = 0U;
volatile uint32_t g_telemetry_stack_start __attribute__((used, externally_visible)) = 0U;
volatile uint32_t g_telemetry_stack_end __attribute__((used, externally_visible)) = 0U;
#define TELEMETRY_THREAD_STACK_SIZE (12U * 1024U)
extern FDCAN_HandleTypeDef hfdcan2;

void telemetry_thread_entry(ULONG initial_input)
{
    (void)initial_input;
    g_telemetry_thread_entered++;

    // Ensure router exists early (so we can send requests immediately)
    (void)init_telemetry_router();
    can_bus_init(&hfdcan2);


    for (;;)
    {
        can_bus_process_rx();
        (void)telemetry_poll_discovery();
        (void)dispatch_tx_queue_timeout(50);
        (void)telemetry_poll_timesync();
        ota_stream_poll();

        _tx_thread_stack_analyze(&telemetry_thread);
        g_telemetry_stack_used = (uint32_t)(
            (uintptr_t)telemetry_thread.tx_thread_stack_end -
            (uintptr_t)telemetry_thread.tx_thread_stack_highest_ptr + sizeof(ULONG));
        g_telemetry_stack_remaining = (uint32_t)(
            (uintptr_t)telemetry_thread.tx_thread_stack_highest_ptr -
            (uintptr_t)telemetry_thread.tx_thread_stack_start);
        _tx_thread_stack_analyze(&sensor_thread);
        g_sensor_stack_remaining = (uint32_t)(
            (uintptr_t)sensor_thread.tx_thread_stack_highest_ptr -
            (uintptr_t)sensor_thread.tx_thread_stack_start);

        tx_thread_sleep(1);
    }
}

UINT create_telemetry_thread(TX_BYTE_POOL *byte_pool)
{
    CHAR *pointer;

    if (tx_byte_allocate(byte_pool, (VOID **)&pointer,
                         TELEMETRY_THREAD_STACK_SIZE, TX_NO_WAIT) != TX_SUCCESS)
    {
        return TX_POOL_ERROR;
    }

    UINT status = tx_thread_create(&telemetry_thread,
                                   "Telemetry Thread",
                                   telemetry_thread_entry,
                                   0,
                                   pointer,
                                   TELEMETRY_THREAD_STACK_SIZE,
                                   3,
                                   3,
                                   TX_NO_TIME_SLICE,
                                   TX_AUTO_START);

    g_telemetry_stack_start = (uint32_t)(uintptr_t)pointer;
    g_telemetry_stack_end = (uint32_t)(uintptr_t)pointer + TELEMETRY_THREAD_STACK_SIZE;

    return status;
}
