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
static TX_BYTE_POOL sedsnet_byte_pool;
volatile uint32_t g_thread_stack_error_count = 0U;

static void thread_stack_error_handler(TX_THREAD *thread_ptr)
{
  (void)thread_ptr;
  g_thread_stack_error_count++;
  Error_Handler();
}
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
  VOID *sedsnet_pool_memory = TX_NULL;
  if (tx_byte_allocate(byte_pool, &sedsnet_pool_memory,
                       POWER_SEDSNET_MEMORY_POOL_SIZE, TX_NO_WAIT) != TX_SUCCESS ||
      tx_byte_pool_create(&sedsnet_byte_pool, "SEDSNet memory pool",
                          sedsnet_pool_memory,
                          POWER_SEDSNET_MEMORY_POOL_SIZE) != TX_SUCCESS)
  {
    Error_Handler();
  }
  telemetry_set_byte_pool(&sedsnet_byte_pool);
  /* Initialize telemetry lock used by Rust (telemetry_lock/telemetry_unlock). */
  telemetry_init_lock();
  (void)tx_thread_stack_error_notify(thread_stack_error_handler);
  
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
