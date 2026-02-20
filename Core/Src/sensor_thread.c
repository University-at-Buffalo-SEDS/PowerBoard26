// sensor_thread.c (or telemetry_thread.c)
#include <stdint.h>
#include "PB-Threads.h"
#include "tx_api.h"
#include "telemetry.h"
#include "ltc2990.h"
#include "main.h"
#include <stdio.h>

TX_THREAD sensor_thread;

#define SENSOR_THREAD_STACK_SIZE (3U *1024U)
extern I2C_HandleTypeDef hi2c2;

void sensor_thread_entry(ULONG entry_input)
{
    LTC2990_Handle_t *ltc2990_handle =
        (LTC2990_Handle_t *)(uintptr_t)entry_input;

    // Initialize LTC2990 for voltage mode
    if (LTC2990_Init(ltc2990_handle, &hi2c2, LTC2990_I2C_ADDRESS_VOLTAGE, VOLTAGE) != 0) {
        log_error_syncronous("LTC2990 init failed");
        Error_Handler();
    }

    const char started_txt[] = "Sensor thread starting";
    (void)log_telemetry_synchronous(SEDS_DT_MESSAGE_DATA,
                                    started_txt,
                                    sizeof(started_txt),
                                    1);

    for (;;) {
        // HAL_GPIO_TogglePin(BLUE_LED_GPIO_Port, BLUE_LED_Pin);
        tx_thread_sleep(50); // 50 ticks = 0.5s if your tick is 100Hz (it is)
        telemetry_ltc2990_update(ltc2990_handle);
        // Consider sleeping/yielding so you don't peg the CPU:
        // tx_thread_sleep(1);  // or a meaningful period
    }
}

UINT create_sensor_thread(TX_BYTE_POOL *byte_pool, LTC2990_Handle_t *ltc2990_handle_ptr)
{
     CHAR *pointer;

  /* Allocate the stack for test  */
  if (tx_byte_allocate(byte_pool, (VOID**) &pointer,
                       SENSOR_THREAD_STACK_SIZE, TX_NO_WAIT) != TX_SUCCESS)
  {
    return TX_POOL_ERROR;
  }
UINT status = tx_thread_create(&sensor_thread,
                               "Sensor Thread",
                               sensor_thread_entry,
                               (ULONG)(uintptr_t)ltc2990_handle_ptr,
                               pointer,                 // stack pointer from tx_byte_allocate
                               SENSOR_THREAD_STACK_SIZE,       // must match allocation size
                               5,                       // priority
                               5,                       // preemption threshold
                               TX_NO_TIME_SLICE,
                               TX_AUTO_START);

    return status;
}
