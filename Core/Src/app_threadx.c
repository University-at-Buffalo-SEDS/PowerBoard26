/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_threadx.c
  * @author  MCD Application Team
  * @brief   ThreadX applicative file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2020-2021 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "app_threadx.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "main.h"
#include "sedsprintf.h"
#include "telemetry.h"
#include "PB-Threads.h"
#include "tx_api.h"
/* USER CODE END Includes */
// LTC2990 driver header
#include "ltc2990.h"

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

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
  if (init_telemetry_router() != SEDS_OK) {
    Error_Handler();
  }
  /* Log after router is initialized, before threads start */

  char started_txt[] = "Starting Threadx Scheduler";
  log_telemetry_synchronous(SEDS_DT_MESSAGE_DATA, started_txt,
                                  sizeof(started_txt), 1);




  /* USER CODE END App_ThreadX_MEM_POOL */

  /* USER CODE BEGIN App_ThreadX_Init */
  // LTC2990 driver integration
  static LTC2990_Handle_t ltc2990_handle;
  static TX_MUTEX ltc2990_mutex;

  // Create mutex for I2C
  tx_mutex_create(&ltc2990_mutex, "ltc2990_mutex", TX_NO_INHERIT);
  ltc2990_handle.i2c_mutex = &ltc2990_mutex;

  // Initialize LTC2990 for voltage mode
  if (LTC2990_Init(&ltc2990_handle, &hi2c2, LTC2990_I2C_ADDRESS_VOLTAGE, VOLTAGE) != 0) {
    log_error_syncronous("LTC2990 init failed");
    Error_Handler();
  }

  // Telemetry thread will periodically read voltages
  create_telemetry_thread();

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

  /* USER CODE END  Kernel_Start_Error */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
#include "ltc2990.h"
extern LTC2990_Handle_t ltc2990_handle;

void telemetry_ltc2990_update(void) {
  float voltages[4];
  LTC2990_Step(&ltc2990_handle);
  LTC2990_Get_Voltage(&ltc2990_handle, voltages);
  log_telemetry_synchronous(SEDS_DT_MESSAGE_DATA, voltages, 4, sizeof(float));
}
