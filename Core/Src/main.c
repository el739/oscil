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
#include "ili9341.h"
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define SCOPE_SAMPLES       320   // 每帧采样 320 个点，对应屏幕宽度
#define INFO_PANEL_HEIGHT    64   // 信息面板高度，方便展示多个数值
#define WAVEFORM_HEIGHT     (ILI9341_HEIGHT - INFO_PANEL_HEIGHT)
#define ADC_REF_VOLTAGE    3.3f
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
uint16_t adc_buf[SCOPE_SAMPLES];
volatile uint8_t scope_frame_ready = 0;

// 记录上一帧每个 x 的竖线范围 [y_min, y_max]
uint16_t last_y_min[ILI9341_WIDTH];
uint16_t last_y_max[ILI9341_WIDTH];
uint8_t first_draw = 1;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void Scope_DrawGrid(void);
void Scope_DrawWaveform(uint16_t *buf, uint16_t len);
void Scope_DrawMeasurements(uint16_t vmin, uint16_t vmax);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static uint16_t Scope_FindTriggerIndex(uint16_t *buf, uint16_t len,
                                       uint16_t *out_min, uint16_t *out_max)
{
    if (len < 2) return 0;

    // 1. 先找出本帧大致的最小值 / 最大值
    uint16_t vmin = 0xFFFF;
    uint16_t vmax = 0;
    for (uint16_t i = 0; i < len; i++) {
        uint16_t v = buf[i];
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }

    if (out_min) *out_min = vmin;
    if (out_max) *out_max = vmax;

    // 如果波形几乎是平的（比如没信号），就不要触发，直接从 0 开始
    if (vmax - vmin < 20) {   // 20 可以自己调
        return 0;
    }

    // 2. 设一个触发门限 = (min + max) / 2，类似示波器的“中间电平”
    uint16_t thr = (uint16_t)((vmin + vmax) / 2);

    // 3. 找“上升沿”：前一个点在门限下，后一个点在门限上
    for (uint16_t i = 1; i < len; i++) {
        uint16_t v_prev = buf[i - 1];
        uint16_t v_now  = buf[i];
        if (v_prev < thr && v_now >= thr) {
            return i;   // 第一个满足条件的点作为触发点
        }
    }

    // 找不到就从 0 画（会像现在一样自由漂动）
    return 0;
}
// 返回 (x, y) 处的背景颜色：在格线上是蓝色，其他地方是黑色
static uint16_t Scope_BackgroundColor(uint16_t x, uint16_t y)
{
    if (y < INFO_PANEL_HEIGHT) {
        return ILI9341_BLACK;
    }
    // 和你画网格时保持一致：每 40 像素一条线（网格只覆盖波形区域）
    uint16_t gy = y - INFO_PANEL_HEIGHT;
    if ((x % 40) == 0 || (gy % 40) == 0) {
        return ILI9341_BLUE;   // 网格线
    } else {
        return ILI9341_BLACK;  // 背景
    }
}
// 画背景网格
void Scope_DrawGrid(void)
{
    ILI9341_FillScreen(ILI9341_BLACK);
    ILI9341_FillRect(0, 0, ILI9341_WIDTH, INFO_PANEL_HEIGHT, ILI9341_BLACK);

    for (int x = 0; x < ILI9341_WIDTH; x += 40) {
        for (int y = INFO_PANEL_HEIGHT; y < ILI9341_HEIGHT; y++) {
            ILI9341_DrawPixel(x, y, ILI9341_BLUE);
        }
    }
    for (int y = INFO_PANEL_HEIGHT; y < ILI9341_HEIGHT; y += 40) {
        for (int x = 0; x < ILI9341_WIDTH; x++) {
            ILI9341_DrawPixel(x, y, ILI9341_BLUE);
        }
    }

    // 初始化上一帧竖线范围：先都设成屏幕中间一个点
    for (int x = 0; x < ILI9341_WIDTH; x++) {
        uint16_t mid = INFO_PANEL_HEIGHT + (WAVEFORM_HEIGHT / 2);
        last_y_min[x] = mid;
        last_y_max[x] = mid;
    }
    first_draw = 1;
}



// 根据 ADC 数据画波形
// 按触发对齐后画波形，使用“擦旧点+画新点”的方式，避免网格闪烁
void Scope_DrawWaveform(uint16_t *buf, uint16_t len)
{
    if (len > ILI9341_WIDTH) len = ILI9341_WIDTH;
    uint16_t frame_min = 0;
    uint16_t frame_max = 0;

    // 1. 触发对齐
    uint16_t trig = Scope_FindTriggerIndex(buf, len, &frame_min, &frame_max);

    // 2. 先算出这一帧的 y 数组（触发对齐 + 简单平滑）
    uint16_t new_y[ILI9341_WIDTH];

    for (uint16_t i = 0; i < len; i++) {
        uint16_t idx = trig + i;
        if (idx >= len) idx -= len;

        // 代替那段 sum/cnt 的代码
        uint16_t val = buf[idx];   // 不做平滑，直接用原始采样值


        if (val > 4095) val = 4095;
        uint16_t y = INFO_PANEL_HEIGHT + (WAVEFORM_HEIGHT - 1)
                   - (val * (WAVEFORM_HEIGHT - 1) / 4095);

        new_y[i] = y;
    }

    // 3. 对每一个 x：先擦旧竖线，再画新竖线，再更新范围
    for (uint16_t x = 0; x < len; x++) {

        // 3.1 擦掉上一帧在这个 x 上画过的竖线
        if (!first_draw) {
            uint16_t ymin_old = last_y_min[x];
            uint16_t ymax_old = last_y_max[x];
            if (ymin_old > ymax_old) {
                uint16_t tmp = ymin_old;
                ymin_old = ymax_old;
                ymax_old = tmp;
            }
            if (ymax_old >= ILI9341_HEIGHT) ymax_old = ILI9341_HEIGHT - 1;

            for (uint16_t yy = ymin_old; yy <= ymax_old; yy++) {
                uint16_t bg = Scope_BackgroundColor(x, yy);
                ILI9341_DrawPixel(x, yy, bg);
            }
        }

        // 3.2 计算这一帧在这个 x 上的新竖线范围
        uint16_t y1 = new_y[x];
        uint16_t y0 = (x > 0) ? new_y[x - 1] : new_y[x];

        uint16_t ymin_new = (y0 < y1) ? y0 : y1;
        uint16_t ymax_new = (y0 > y1) ? y0 : y1;
        if (ymax_new >= ILI9341_HEIGHT) ymax_new = ILI9341_HEIGHT - 1;

        // 3.3 画新竖线
        for (uint16_t yy = ymin_new; yy <= ymax_new; yy++) {
            ILI9341_DrawPixel(x, yy, ILI9341_YELLOW);
        }

        // 3.4 更新记录
        last_y_min[x] = ymin_new;
        last_y_max[x] = ymax_new;
    }

    first_draw = 0;
    Scope_DrawMeasurements(frame_min, frame_max);
}

// ADC + DMA 采满一帧后的回调函数
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
    {
        scope_frame_ready = 1;
    }
}
// 在一帧 ADC 数据中寻找一个“上升沿过中值”的触发点
// 返回触发起始的索引（0..len-1），找不到就返回 0

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
  // 初始化 LCD
    ILI9341_Init();
    Scope_DrawGrid();
    Scope_DrawMeasurements(0, 0);

    // 启动 ADC + DMA
    HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_buf, SCOPE_SAMPLES);

    // 启动 TIM3，产生 TRGO 触发 ADC
    HAL_TIM_Base_Start(&htim3);
    HAL_TIM_PWM_Start(&htim1,TIM_CHANNEL_1);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
	  if (scope_frame_ready)
	  {
	      scope_frame_ready = 0;
	      Scope_DrawWaveform(adc_buf, SCOPE_SAMPLES);
	      // 这里现在已经是有触发对齐和擦除逻辑的“简易示波器”了
	  }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

  }
  /* USER CODE END 3 */
}

void Scope_DrawMeasurements(uint16_t vmin, uint16_t vmax)
{
    char line1[32];
    char line2[32];
    float vmin_volt = (vmin * ADC_REF_VOLTAGE) / 4095.0f;
    float vmax_volt = (vmax * ADC_REF_VOLTAGE) / 4095.0f;

    snprintf(line1, sizeof(line1), "Vmax: %.2f V", vmax_volt);
    snprintf(line2, sizeof(line2), "Vmin: %.2f V", vmin_volt);

    ILI9341_FillRect(0, 0, ILI9341_WIDTH, INFO_PANEL_HEIGHT, ILI9341_BLACK);
    ILI9341_DrawString(4, 4, line1, ILI9341_YELLOW, ILI9341_BLACK, 2);
    ILI9341_DrawString(4, 24, line2, ILI9341_GREEN, ILI9341_BLACK, 2);
    // 信息面板剩余区域可继续打印频率、RMS 等更多指标
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
