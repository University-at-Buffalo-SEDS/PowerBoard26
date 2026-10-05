#ifndef TELEMETRY_RESERVE_H
#define TELEMETRY_RESERVE_H
#include "tx_api.h"
#include <stdint.h>
#include <stddef.h>

/* Separate, fixed-size storage: packet bursts cannot fragment these blocks.
 * Keep small persistent allocations out. The ordinary heap is tried first.
 * Include ThreadX's per-block ownership word in each backing array. */
#define TELEMETRY_RESERVE_MIN 1024U
#define TELEMETRY_RESERVE_MEDIUM 4096U
#define TELEMETRY_RESERVE_LARGE 8192U
#ifdef TELEMETRY_USE_TLSF
#include "telemetry_tlsf.h"
static TX_BYTE_POOL telemetry_reserve_medium_pool;
static TX_BYTE_POOL telemetry_reserve_large_pool;
#else
static TX_BLOCK_POOL telemetry_reserve_medium_pool;
static TX_BLOCK_POOL telemetry_reserve_large_pool;
#endif
static ULONG telemetry_reserve_medium_storage[4U * (TELEMETRY_RESERVE_MEDIUM + sizeof(void *)) / sizeof(ULONG)];
static ULONG telemetry_reserve_large_storage[(TELEMETRY_RESERVE_LARGE + sizeof(void *)) / sizeof(ULONG)];
static UINT telemetry_reserve_ready;
volatile uint32_t g_telemetry_reserve_recoveries;
volatile UINT g_telemetry_reserve_init_status;

static void telemetry_reserve_init(void)
{
    if (telemetry_reserve_ready) return;
#ifdef TELEMETRY_USE_TLSF
    UINT status = tx_byte_pool_create(&telemetry_reserve_medium_pool, "TLSF reserve 4K",
        telemetry_reserve_medium_storage, sizeof(telemetry_reserve_medium_storage));
    if (status == TX_SUCCESS) {
        status = tx_byte_pool_create(&telemetry_reserve_large_pool, "TLSF reserve 8K",
            telemetry_reserve_large_storage, sizeof(telemetry_reserve_large_storage));
    }
    if (status != TX_SUCCESS) { Error_Handler(); return; }
    telemetry_tlsf_register_pool(&telemetry_reserve_medium_pool);
    telemetry_tlsf_register_pool(&telemetry_reserve_large_pool);
#else
    UINT status = tx_block_pool_create(&telemetry_reserve_medium_pool, "packet reserve 4K",
        TELEMETRY_RESERVE_MEDIUM, telemetry_reserve_medium_storage, sizeof(telemetry_reserve_medium_storage));
    if (status == TX_SUCCESS) {
        status = tx_block_pool_create(&telemetry_reserve_large_pool, "packet reserve 8K",
            TELEMETRY_RESERVE_LARGE, telemetry_reserve_large_storage, sizeof(telemetry_reserve_large_storage));
        if (status != TX_SUCCESS) (void)tx_block_pool_delete(&telemetry_reserve_medium_pool);
    }
#endif
    g_telemetry_reserve_init_status = status;
    telemetry_reserve_ready = (status == TX_SUCCESS);
}

#ifndef TELEMETRY_USE_TLSF
static void *telemetry_reserve_allocate(size_t size)
{
    void *ptr = NULL;
    if (!telemetry_reserve_ready || size < TELEMETRY_RESERVE_MIN || size > TELEMETRY_RESERVE_LARGE) return NULL;
    if (size <= TELEMETRY_RESERVE_MEDIUM &&
        tx_block_allocate(&telemetry_reserve_medium_pool, &ptr, TX_NO_WAIT) == TX_SUCCESS) return ptr;
    if (tx_block_allocate(&telemetry_reserve_large_pool, &ptr, TX_NO_WAIT) == TX_SUCCESS) return ptr;
    return NULL;
}

static int telemetry_reserve_owns(void *ptr)
{
    const uintptr_t address = (uintptr_t)ptr;
    const uintptr_t medium = (uintptr_t)telemetry_reserve_medium_storage;
    const uintptr_t large = (uintptr_t)telemetry_reserve_large_storage;
    return (address >= medium && address < medium + sizeof(telemetry_reserve_medium_storage)) ||
           (address >= large && address < large + sizeof(telemetry_reserve_large_storage));
}
#endif /* !TELEMETRY_USE_TLSF */
#endif
