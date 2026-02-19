// sensor_thread.c (or telemetry_thread.c)
#include <stdint.h>
#include "PB-Threads.h"
#include "tx_api.h"
#include "telemetry.h"
#include "ltc2990.h"
#include "main.h"

TX_THREAD sensor_thread;

#define SENSOR_THREAD_STACK_SIZE 1024u
ULONG sensor_thread_stack[SENSOR_THREAD_STACK_SIZE / sizeof(ULONG)];

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

                                    int boo = 0;
    for (;;) {
        if (!boo){
            //   HAL_GPIO_TogglePin(BLUE_LED_GPIO_Port, BLUE_LED_Pin);
                boo = 1;
        }
        telemetry_ltc2990_update(ltc2990_handle);
        // Consider sleeping/yielding so you don't peg the CPU:
        // tx_thread_sleep(1);  // or a meaningful period
    }
}

void create_sensor_thread(LTC2990_Handle_t *ltc2990_handle_ptr)
{
    HAL_GPIO_WritePin(BLUE_LED_GPIO_Port, BLUE_LED_Pin, GPIO_PIN_SET);
    HAL_Delay(500);
    
    HAL_GPIO_WritePin(BLUE_LED_GPIO_Port, BLUE_LED_Pin, GPIO_PIN_RESET);
    HAL_Delay(500);
    UINT status = tx_thread_create(&sensor_thread,
                                   "Sensor Thread",
                                   sensor_thread_entry,
                                   (ULONG)(uintptr_t)ltc2990_handle_ptr,
                                   sensor_thread_stack,
                                   sizeof(sensor_thread_stack),
                                   5,
                                   5,
                                   TX_NO_TIME_SLICE,
                                   TX_AUTO_START);

    if (status != TX_SUCCESS) {
        die("Failed to create sensor thread: %u", (unsigned)status);
    }
}
