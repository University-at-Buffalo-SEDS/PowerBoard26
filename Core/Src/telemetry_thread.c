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
#define TELEMETRY_THREAD_STACK_SIZE (11U * 1024U)
#define TELEMETRY_QUEUE_SERVICE_BUDGET_MS 1U
volatile uint32_t g_telemetry_thread_entered __attribute__((used, externally_visible)) = 0U;
volatile uint32_t g_telemetry_stack_used __attribute__((used, externally_visible)) = 0U;
volatile uint32_t g_telemetry_stack_remaining __attribute__((used, externally_visible)) = TELEMETRY_THREAD_STACK_SIZE;
volatile uint32_t g_telemetry_stack_start __attribute__((used, externally_visible)) = 0U;
volatile uint32_t g_telemetry_stack_end __attribute__((used, externally_visible)) = 0U;
volatile uint32_t g_telemetry_init_stage __attribute__((used, externally_visible)) = 0U;
volatile int32_t g_telemetry_init_result __attribute__((used, externally_visible)) = 0;
volatile uint32_t g_telemetry_service_stage __attribute__((used, externally_visible)) = 0U;
extern FDCAN_HandleTypeDef hfdcan2;

void telemetry_thread_entry(ULONG initial_input)
{
    (void)initial_input;
    g_telemetry_thread_entered++;

    /* Router initialization may emit startup traffic, so bring the physical
     * CAN transport up first. */
    g_telemetry_init_stage = 1U;
    can_bus_init(&hfdcan2);
    g_telemetry_init_stage = 2U;
    g_telemetry_init_result = (int32_t)init_telemetry_router();
    g_telemetry_init_stage = 3U;


    for (;;)
    {
        g_telemetry_service_stage = 1U;
        can_bus_process_rx();
        g_telemetry_service_stage = 2U;
        (void)telemetry_poll_discovery();
        g_telemetry_service_stage = 3U;
        /* SEDSNet receive_from_side queues decoded work. Service RX and TX
         * together so network variables and telemetry make bounded progress. */
        (void)process_all_queues_timeout(TELEMETRY_QUEUE_SERVICE_BUDGET_MS);
        g_telemetry_service_stage = 4U;
        (void)telemetry_poll_timesync();
        g_telemetry_service_stage = 5U;
        ota_stream_poll();
        g_telemetry_service_stage = 6U;

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
        g_telemetry_service_stage = 7U;
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
