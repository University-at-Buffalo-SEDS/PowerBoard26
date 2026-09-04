/* USER CODE BEGIN Header */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "app_threadx.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "main.h"
#include "PB-Threads.h"
/* Provide telemetry_set_byte_pool so Rust uses an isolated allocator pool. */
extern void telemetry_set_byte_pool(TX_BYTE_POOL *pool);
extern void telemetry_init_lock(void);
#include "telemetry.h"
#include "ltc2990.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
static void busy_delay(volatile uint32_t n)
{
  while (n--) { __NOP(); }
}
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
volatile uint32_t g_thread_stack_error_count = 0U;
volatile uint32_t g_telemetry_thread_stack_error_count = 0U;
volatile uint32_t g_sensor_thread_stack_error_count = 0U;
volatile uint32_t g_stack_error_thread_ptr = 0U;
volatile uint32_t g_stack_error_stack_ptr = 0U;
volatile uint32_t g_stack_error_stack_start = 0U;
volatile uint32_t g_stack_error_stack_end = 0U;
volatile uint32_t g_stack_error_stack_highest = 0U;
volatile uint32_t g_stack_error_start_guard = 0U;
volatile uint32_t g_stack_error_end_guard = 0U;

#ifndef SEDS_FIRMWARE_SIM_TEST
static void thread_stack_error_handler(TX_THREAD *thread_ptr)
{
  g_thread_stack_error_count++;
  if (thread_ptr != TX_NULL) {
    g_stack_error_thread_ptr = (uint32_t)(uintptr_t)thread_ptr;
    g_stack_error_stack_ptr = (uint32_t)(uintptr_t)thread_ptr->tx_thread_stack_ptr;
    g_stack_error_stack_start = (uint32_t)(uintptr_t)thread_ptr->tx_thread_stack_start;
    g_stack_error_stack_end = (uint32_t)(uintptr_t)thread_ptr->tx_thread_stack_end;
    g_stack_error_stack_highest = (uint32_t)(uintptr_t)thread_ptr->tx_thread_stack_highest_ptr;
    g_stack_error_start_guard = *((volatile uint32_t *)thread_ptr->tx_thread_stack_start);
    g_stack_error_end_guard = *((volatile uint32_t *)
        ((uint8_t *)thread_ptr->tx_thread_stack_end + 1U));
  }
  if (thread_ptr == &telemetry_thread) {
    g_telemetry_thread_stack_error_count++;
  } else if (thread_ptr == &sensor_thread) {
    g_sensor_thread_stack_error_count++;
  }
  Error_Handler();
}
#endif
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/**
  * @brief  Application ThreadX Initialization.
  * @param memory_ptr: memory pointer
  * @retval int
  */
UINT App_ThreadX_Init(VOID *memory_ptr)
{
  UINT ret = TX_SUCCESS;
  /* USER CODE BEGIN App_ThreadX_MEM_POOL */
  TX_BYTE_POOL *byte_pool = (TX_BYTE_POOL*)memory_ptr;
  /* USER CODE END App_ThreadX_MEM_POOL */

  /* USER CODE BEGIN App_ThreadX_Init */
  /*
   * Keep one ThreadX ownership/free list for stacks and SEDSNet allocations.
   * A byte pool nested inside an allocation from this pool corrupted its
   * search pointer under the first SEDSNet schema packing burst.
   */
  telemetry_set_byte_pool(byte_pool);
  /* Initialize telemetry lock used by Rust (telemetry_lock/telemetry_unlock). */
  telemetry_init_lock();
#ifndef SEDS_FIRMWARE_SIM_TEST
  (void)tx_thread_stack_error_notify(thread_stack_error_handler);
#else
  /* Keep the diagnostic counters in the simulated ELF even though Renode's
   * ThreadX model cannot safely invoke the runtime stack-error callback. */
  g_thread_stack_error_count = 0U;
  g_telemetry_thread_stack_error_count = 0U;
  g_sensor_thread_stack_error_count = 0U;
  g_stack_error_thread_ptr = 0U;
  g_stack_error_stack_ptr = 0U;
  g_stack_error_stack_start = 0U;
  g_stack_error_stack_end = 0U;
  g_stack_error_stack_highest = 0U;
  g_stack_error_start_guard = 0U;
  g_stack_error_end_guard = 0U;
#endif
  
  static LTC2990_Handle_t ltc2990_voltage_handle;
  static LTC2990_Handle_t ltc2990_current_handle;

  ret = create_sensor_thread(byte_pool, &ltc2990_voltage_handle, &ltc2990_current_handle);
  if (ret != TX_SUCCESS) {
    Error_Handler();
  }

  ret = create_telemetry_thread(byte_pool);
  if (ret != TX_SUCCESS)
  {
    Error_Handler();
  }
  


  /* USER CODE END App_ThreadX_Init */

  return ret;
}

  /**
  * @brief  Function that implements the kernel's initialization.
  * @param  None
  * @retval None
  */
void MX_ThreadX_Init(void)
{
  /* USER CODE BEGIN  Before_Kernel_Start */
  /* USER CODE END  Before_Kernel_Start */

  tx_kernel_enter();

  /* USER CODE BEGIN  Kernel_Start_Error */
  while (1)
  {
    HAL_GPIO_TogglePin(GREEN_LED_GPIO_Port, GREEN_LED_Pin);
    busy_delay(40000000); // adjust until visible
  }
  /* USER CODE END  Kernel_Start_Error */
}

/* USER CODE BEGIN 1 */
/* USER CODE END 1 */
