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
#include "scope.h"
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
enum
{
    SCOPE_DMA_BUFFER_COUNT = 2U,
    BUTTON_DEBOUNCE_MS = 80U
};

static uint16_t adc_dma_buf[SCOPE_DMA_BUFFER_COUNT][SCOPE_FRAME_SAMPLES];

typedef enum
{
    SCOPE_SCALE_TARGET_VOLTAGE = 0,
    SCOPE_SCALE_TARGET_TIME
} ScopeScaleTarget;

typedef struct
{
    uint8_t order[SCOPE_DMA_BUFFER_COUNT];
    uint8_t head;
    uint8_t tail;
    uint8_t pending;
    uint32_t overruns;
} ScopeDmaFrameQueue;

typedef struct
{
    uint16_t gpio_pin;
    uint32_t last_tick;
} ButtonDebounceState;

static ScopeDmaFrameQueue scope_dma_queue = {0};
static volatile uint8_t scope_frame_ready = 0U;
static void Scope_DmaEnqueueBuffer(uint8_t buffer_index);
static uint16_t *Scope_DmaDequeueBuffer(void);
static ScopeScaleTarget scope_scale_target = SCOPE_SCALE_TARGET_VOLTAGE;
static uint8_t Scope_ButtonDebounced(uint16_t gpio_pin);
static ButtonDebounceState button_debounce_states[] = {
    {USER_Btn_Pin, 0U},
    {K1_Pin, 0U},
    {K2_Pin, 0U},
    {K3_Pin, 0U},
    {K4_Pin, 0U},
    {K5_Pin, 0U},
    {K6_Pin, 0U},
    {K7_Pin, 0U},
    {K8_Pin, 0U}};
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
    {
        Scope_DmaEnqueueBuffer(1U);
    }
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
    {
        Scope_DmaEnqueueBuffer(0U);
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
  Scope_Init();
  const uint16_t frame_samples = Scope_FrameSampleCount();
  const uint32_t dma_samples = (uint32_t)frame_samples * SCOPE_DMA_BUFFER_COUNT;
  HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_dma_buf[0], dma_samples);
  HAL_TIM_Base_Start(&htim3);
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
      if (scope_frame_ready)
      {
          uint16_t *ready_buf = Scope_DmaDequeueBuffer();
          if (ready_buf != NULL)
          {
              Scope_ProcessFrame(ready_buf, frame_samples);
          }
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
    if (!Scope_ButtonDebounced(GPIO_Pin))
    {
        return;
    }

    if (GPIO_Pin == USER_Btn_Pin)
    {
        HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
        Scope_RequestAutoSet();
    }
    else if (GPIO_Pin == K1_Pin)
    {
        if (scope_scale_target == SCOPE_SCALE_TARGET_VOLTAGE)
        {
            Scope_RequestMoreVoltageScale();
        }
        else
        {
            Scope_RequestMoreCycles();
        }
    }
    else if (GPIO_Pin == K2_Pin)
    {
        if (scope_scale_target == SCOPE_SCALE_TARGET_VOLTAGE)
        {
            Scope_RequestLessVoltageScale();
        }
        else
        {
            Scope_RequestFewerCycles();
        }
    }
    else if (GPIO_Pin == K8_Pin)
    {
        if (scope_scale_target == SCOPE_SCALE_TARGET_VOLTAGE)
        {
            scope_scale_target = SCOPE_SCALE_TARGET_TIME;
        }
        else
        {
            scope_scale_target = SCOPE_SCALE_TARGET_VOLTAGE;
        }
    }
}

static uint8_t Scope_ButtonDebounced(uint16_t gpio_pin)
{
    const uint32_t now = HAL_GetTick();
    ButtonDebounceState *state = NULL;
    const uint32_t state_count = (uint32_t)(sizeof(button_debounce_states) / sizeof(button_debounce_states[0]));

    for (uint32_t idx = 0U; idx < state_count; idx++)
    {
        if (button_debounce_states[idx].gpio_pin == gpio_pin)
        {
            state = &button_debounce_states[idx];
            break;
        }
    }

    if (state == NULL)
    {
        return 1U;
    }

    if (state->last_tick != 0U)
    {
        if ((now - state->last_tick) < BUTTON_DEBOUNCE_MS)
        {
            return 0U;
        }
    }

    state->last_tick = now;
    return 1U;
}

/* USER CODE BEGIN Header_Scope_DmaEnqueueBuffer */
/**
  * @brief Queue ADC buffer index for processing, dropping frames on overflow.
  */
/* USER CODE END Header_Scope_DmaEnqueueBuffer */
static void Scope_DmaEnqueueBuffer(uint8_t buffer_index)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if (scope_dma_queue.pending == SCOPE_DMA_BUFFER_COUNT)
    {
        scope_dma_queue.tail = (uint8_t)((scope_dma_queue.tail + 1U) % SCOPE_DMA_BUFFER_COUNT);
        scope_dma_queue.pending--;
        scope_dma_queue.overruns++;
    }

    scope_dma_queue.order[scope_dma_queue.head] = buffer_index;
    scope_dma_queue.head = (uint8_t)((scope_dma_queue.head + 1U) % SCOPE_DMA_BUFFER_COUNT);
    scope_dma_queue.pending++;
    scope_frame_ready = 1U;

    if (primask == 0U)
    {
        __enable_irq();
    }
}

static uint16_t *Scope_DmaDequeueBuffer(void)
{
    uint16_t *buffer = NULL;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if (scope_dma_queue.pending > 0U)
    {
        uint8_t buffer_index = scope_dma_queue.order[scope_dma_queue.tail];
        scope_dma_queue.tail = (uint8_t)((scope_dma_queue.tail + 1U) % SCOPE_DMA_BUFFER_COUNT);
        scope_dma_queue.pending--;
        buffer = adc_dma_buf[buffer_index];
    }

    if (scope_dma_queue.pending == 0U)
    {
        scope_frame_ready = 0U;
    }

    if (primask == 0U)
    {
        __enable_irq();
    }

    return buffer;
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
