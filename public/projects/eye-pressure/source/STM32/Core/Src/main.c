/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
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
#include "main.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
/* USER CODE END Includes */

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
uint8_t rx_buffer_usart2;  // USART2接收缓冲区
uint8_t rx_buffer_usart1;  // USART1接收缓冲区

// 循环发送数据相关变量
uint8_t tx_data[] = {0x20, 0x00, 0x57, 0x02, 0x00, 0x2C, 0x86, 0x03};  // 要发送的数据
uint8_t tx_index = 0;  // 发送索引
uint32_t last_tx_time = 0;  // 上次发送时间
uint8_t tx_send_count = 0;  // 发送次数计数器

// 缓冲区相关变量（防卡死）
uint8_t usart1_to_ble_buffer[64];  // USART1到BLE的缓冲区
uint8_t usart1_to_ble_index = 0;   // USART1到BLE缓冲区索引
uint8_t ble_to_usart1_buffer[64];  // BLE到USART1的缓冲区
uint8_t ble_to_usart1_index = 0;   // BLE到USART1缓冲区索引
uint8_t usart1_to_ble_flag = 0;    // USART1到BLE发送标志
uint8_t ble_to_usart1_flag = 0;    // BLE到USART1发送标志
uint32_t last_send_time = 0;        // 上次发送时间


/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */ 

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  // 启动USART2接收中断（用于BLE透传）
  HAL_UART_Receive_IT(&huart2, &rx_buffer_usart2, 1);
  HAL_NVIC_SetPriority(USART2_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(USART2_IRQn);
  
  // 启动USART1接收中断（接收数据通过BLE透传到手机）
  HAL_UART_Receive_IT(&huart1, &rx_buffer_usart1, 1);
  HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    uint32_t current_time = HAL_GetTick();
    
    // 上电后发送两次固定数据到USART1
    if (tx_send_count < 2 && current_time - last_tx_time >= 1000) {  // 每1秒发送一次，最多发送2次
      // 发送完整的数据包
      HAL_UART_Transmit(&huart1, tx_data, sizeof(tx_data), 1000);
      tx_send_count++;  // 增加发送计数
      last_tx_time = current_time;
    }
    
    // 处理缓冲数据发送（防卡死机制）
    if ((usart1_to_ble_flag || ble_to_usart1_flag) && (current_time - last_send_time >= 50)) {  // 至少间隔50ms，提高实时性
      // 发送USART1到BLE的数据
      if (usart1_to_ble_flag && usart1_to_ble_index > 0) {
        HAL_UART_Transmit(&huart2, usart1_to_ble_buffer, usart1_to_ble_index, 1000);
        usart1_to_ble_index = 0;  // 重置缓冲区
        usart1_to_ble_flag = 0;   // 清除发送标志
      }
      
      // 发送BLE到USART1的数据（无限制透传）
      if (ble_to_usart1_flag && ble_to_usart1_index > 0) {
        HAL_UART_Transmit(&huart1, ble_to_usart1_buffer, ble_to_usart1_index, 1000);
        ble_to_usart1_index = 0;  // 重置缓冲区
        ble_to_usart1_flag = 0;   // 清除发送标志
      }
      
      last_send_time = current_time;
    }
    
    // 防止缓冲区溢出
    if (usart1_to_ble_index >= 63 || ble_to_usart1_index >= 63) {
      // 发送USART1到BLE的数据
      if (usart1_to_ble_flag && usart1_to_ble_index > 0) {
        HAL_UART_Transmit(&huart2, usart1_to_ble_buffer, usart1_to_ble_index, 1000);
        usart1_to_ble_index = 0;  // 重置缓冲区
        usart1_to_ble_flag = 0;   // 清除发送标志
      }
      
      // 发送BLE到USART1的数据（无限制透传）
      if (ble_to_usart1_flag && ble_to_usart1_index > 0) {
        HAL_UART_Transmit(&huart1, ble_to_usart1_buffer, ble_to_usart1_index, 1000);
        ble_to_usart1_index = 0;  // 重置缓冲区
        ble_to_usart1_flag = 0;   // 清除发送标志
      }
    }
    
    HAL_Delay(100);  // 增加延时，提高稳定性
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
  if (huart->Instance == USART1) {
    // 将数据添加到USART1到BLE的缓冲区
    if (usart1_to_ble_index < 63) {  // 留1字节空间
      usart1_to_ble_buffer[usart1_to_ble_index++] = rx_buffer_usart1;
      usart1_to_ble_flag = 1;  // 设置发送标志
    }
    
    // 重新启动USART1接收
    HAL_UART_Receive_IT(&huart1, &rx_buffer_usart1, 1);
  }
  else if (huart->Instance == USART2) {
    // 透传模式：将数据添加到BLE到USART1的缓冲区
    if (ble_to_usart1_index < 63) {  // 留1字节空间
      ble_to_usart1_buffer[ble_to_usart1_index++] = rx_buffer_usart2;
      ble_to_usart1_flag = 1;  // 设置发送标志
    }
    
    // 重新启动USART2接收
    HAL_UART_Receive_IT(&huart2, &rx_buffer_usart2, 1);
  }
}






/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number 
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */


