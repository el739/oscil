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
#include "adc.h"
#include "dma.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "usb_otg.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdlib.h>
#include "scope.h"
#include "scope_buffer.h"
#include "input_handler.h"
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
static uint16_t scope_frame_copy[SCOPE_FRAME_SAMPLES];
static uint8_t uart_rx_byte = 0U;
static char uart_rx_buffer[16];
static char uart_cmd_buffer[16];
static volatile uint8_t uart_line_ready = 0U;
static uint8_t uart_rx_len = 0U;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static uint32_t ComputeTim1ClockHz(void);
static uint8_t ApplyWaveformFrequency(uint32_t target_hz);
static void ProcessUartLine(void);
static void SendUartText(const char *text);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
    {
        ScopeBuffer_EnqueueFromISR(1U);
    }
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
    {
        ScopeBuffer_EnqueueFromISR(0U);
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3)
    {
        uint8_t byte = uart_rx_byte;
        if (byte == '\r' || byte == '\n')
        {
            if (uart_rx_len > 0U && uart_line_ready == 0U)
            {
                uint8_t copy_len = uart_rx_len;
                if (copy_len >= sizeof(uart_cmd_buffer))
                {
                    copy_len = (uint8_t)(sizeof(uart_cmd_buffer) - 1U);
                }
                memcpy(uart_cmd_buffer, uart_rx_buffer, copy_len);
                uart_cmd_buffer[copy_len] = '\0';
                uart_line_ready = 1U;
            }
            uart_rx_len = 0U;
        }
        else if (uart_rx_len < (sizeof(uart_rx_buffer) - 1U))
        {
            uart_rx_buffer[uart_rx_len++] = (char)byte;
        }
        else
        {
            uart_rx_len = 0U;
        }

        HAL_UART_Receive_IT(&huart3, &uart_rx_byte, 1U);
    }
}
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
  MX_DMA_Init();
  MX_USART3_UART_Init();
  MX_USB_OTG_FS_PCD_Init();
  MX_ADC1_Init();
  MX_TIM3_Init();
  MX_SPI1_Init();
  MX_TIM1_Init();
  /* USER CODE BEGIN 2 */
  ScopeBuffer_Init();
  InputHandler_Init();
  Scope_Init();
  const uint16_t frame_samples = Scope_FrameSampleCount();
  const uint32_t dma_samples = (uint32_t)frame_samples * 2U;
  HAL_ADC_Start_DMA(&hadc1, (uint32_t *)ScopeBuffer_GetDmaBaseAddress(), dma_samples);
  HAL_TIM_Base_Start(&htim3);
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
  HAL_UART_Receive_IT(&huart3, &uart_rx_byte, 1U);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
      if (ScopeBuffer_HasPending())
      {
          uint16_t samples_count = 0U;
          uint16_t *ready_buf = ScopeBuffer_Dequeue(&samples_count);
          if (ready_buf != NULL)
          {
              uint16_t samples_to_process = samples_count;
              if (samples_to_process > SCOPE_FRAME_SAMPLES)
              {
                  samples_to_process = SCOPE_FRAME_SAMPLES;
              }
              memcpy(scope_frame_copy,
                     ready_buf,
                     (size_t)samples_to_process * sizeof(uint16_t));
              Scope_ProcessFrame(scope_frame_copy, samples_to_process);
          }
      }
      if (uart_line_ready != 0U)
      {
          uart_line_ready = 0U;
          ProcessUartLine();
      }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

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

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 7;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 8;
  RCC_OscInitStruct.PLL.PLLR = 2;
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

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    InputHandler_ProcessGpioInterrupt(GPIO_Pin);
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

#ifdef  USE_FULL_ASSERT
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

/* USER CODE BEGIN 7 */
static uint32_t ComputeTim1ClockHz(void)
{
    uint32_t tim_clk = HAL_RCC_GetPCLK2Freq();
    if (tim_clk == 0U)
    {
        return 0U;
    }

    RCC_ClkInitTypeDef clk_config;
    uint32_t flash_latency;
    HAL_RCC_GetClockConfig(&clk_config, &flash_latency);
    if (clk_config.APB2CLKDivider != RCC_HCLK_DIV1)
    {
        tim_clk *= 2U;
    }

    return tim_clk;
}

static uint8_t ApplyWaveformFrequency(uint32_t target_hz)
{
    if (target_hz == 0U)
    {
        return 0U;
    }

    uint32_t tim_clk = ComputeTim1ClockHz();
    if (tim_clk == 0U)
    {
        return 0U;
    }

    uint32_t psc = 0U;
    uint32_t arr_plus_one = tim_clk / target_hz;

    while (arr_plus_one > 65536U)
    {
        psc++;
        if (psc > 0xFFFFU)
        {
            return 0U;
        }
        arr_plus_one = tim_clk / (target_hz * (psc + 1U));
    }

    if (arr_plus_one == 0U)
    {
        return 0U;
    }

    uint32_t arr = arr_plus_one - 1U;
    uint32_t pulse = (arr + 1U) / 2U;

    HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_1);
    __HAL_TIM_SET_PRESCALER(&htim1, psc);
    __HAL_TIM_SET_AUTORELOAD(&htim1, arr);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, pulse);
    HAL_TIM_GenerateEvent(&htim1, TIM_EVENTSOURCE_UPDATE);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);

    return 1U;
}

static void SendUartText(const char *text)
{
    if (text == NULL)
    {
        return;
    }
    HAL_UART_Transmit(&huart3, (uint8_t *)text, strlen(text), 100U);
}

static void ProcessUartLine(void)
{
    char *line = uart_cmd_buffer;
    if (line[0] == '\0')
    {
        return;
    }

    char *end_ptr;
    while (*line == ' ' || *line == '\t')
    {
        line++;
    }

    unsigned long value = strtoul(line, &end_ptr, 10);
    while (*end_ptr == ' ' || *end_ptr == '\t')
    {
        end_ptr++;
    }

    if (end_ptr == line || *end_ptr != '\0')
    {
        SendUartText("ERR\r\n");
        return;
    }

    if (value == 0UL || value >= 50000UL)
    {
        SendUartText("ERR\r\n");
        return;
    }

    if (ApplyWaveformFrequency((uint32_t)value) != 0U)
    {
        SendUartText("OK\r\n");
    }
    else
    {
        SendUartText("ERR\r\n");
    }
}
/* USER CODE END 7 */
