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
typedef struct {
    uint16_t samples_per_frame;
    uint16_t info_panel_height;
    uint16_t grid_spacing_px;
    uint16_t trigger_min_delta;
    uint16_t adc_ref_millivolt;
    uint16_t adc_max_counts;
    uint16_t waveform_color;
} ScopeConfig;

static const ScopeConfig scope_cfg = {
    .samples_per_frame = ILI9341_WIDTH,  // 采样点数与屏幕宽度一致，便于逐像素绘制
    .info_panel_height = 64U,
    .grid_spacing_px = 40U,
    .trigger_min_delta = 20U,
    .adc_ref_millivolt = 3300U,
    .adc_max_counts = 4095U,
    .waveform_color = ILI9341_YELLOW
};

static inline uint16_t Scope_InfoPanelHeight(void)
{
    return scope_cfg.info_panel_height;
}

static inline uint16_t Scope_WaveformHeight(void)
{
    return ILI9341_HEIGHT - scope_cfg.info_panel_height;
}
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
uint16_t adc_buf[ILI9341_WIDTH];
volatile uint8_t scope_frame_ready = 0;

// 记录上一帧每个 x 的竖线范围 [y_min, y_max]
uint16_t last_y_min[ILI9341_WIDTH];
uint16_t last_y_max[ILI9341_WIDTH];
static uint16_t scope_column_buf[ILI9341_HEIGHT];
uint8_t first_draw = 1;
static uint32_t scope_sample_rate_hz = 0U;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void Scope_DrawGrid(void);
void Scope_DrawWaveform(uint16_t *buf, uint16_t len);
void Scope_DrawMeasurements(uint16_t vmin, uint16_t vmax, uint32_t freq_hz);
static void Scope_EraseColumn(uint16_t x, uint16_t y0, uint16_t y1);
static void Scope_DrawColumn(uint16_t x, uint16_t y0, uint16_t y1, uint16_t color);
static uint32_t Scope_AdcToMillivolt(uint16_t sample);
static uint32_t Scope_EstimateFrequencyHz(uint16_t *buf, uint16_t len,
                                          uint16_t trig_idx,
                                          uint16_t frame_min,
                                          uint16_t frame_max);
static uint32_t Scope_GetSampleRateHz(void);
static uint32_t Scope_ComputeSampleRateHz(void);
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
    if ((uint16_t)(vmax - vmin) < scope_cfg.trigger_min_delta) {   // 由 scope_cfg.trigger_min_delta 控制
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
    const uint16_t info_panel = Scope_InfoPanelHeight();
    if (y < info_panel) {
        return ILI9341_BLACK;
    }
    // 和你画网格时保持一致：每 40 像素一条线（网格只覆盖波形区域）
    uint16_t gy = y - info_panel;
    if ((x % scope_cfg.grid_spacing_px) == 0 ||
        (gy % scope_cfg.grid_spacing_px) == 0) {
        return ILI9341_BLUE;   // 网格线
    } else {
        return ILI9341_BLACK;  // 背景
    }
}

static void Scope_EraseColumn(uint16_t x, uint16_t y0, uint16_t y1)
{
    if (x >= ILI9341_WIDTH) return;
    if (y0 > y1) {
        uint16_t tmp = y0;
        y0 = y1;
        y1 = tmp;
    }
    if (y0 >= ILI9341_HEIGHT) return;
    if (y1 >= ILI9341_HEIGHT) {
        y1 = ILI9341_HEIGHT - 1;
    }
    uint16_t span = y1 - y0 + 1;
    if (span == 0) return;
    for (uint16_t i = 0; i < span; i++) {
        scope_column_buf[i] = Scope_BackgroundColor(x, y0 + i);
    }
    ILI9341_DrawPixels(x, y0, scope_column_buf, span);
}

static void Scope_DrawColumn(uint16_t x, uint16_t y0, uint16_t y1, uint16_t color)
{
    if (x >= ILI9341_WIDTH) return;
    if (y0 > y1) {
        uint16_t tmp = y0;
        y0 = y1;
        y1 = tmp;
    }
    if (y0 >= ILI9341_HEIGHT) return;
    if (y1 >= ILI9341_HEIGHT) {
        y1 = ILI9341_HEIGHT - 1;
    }
    uint16_t span = y1 - y0 + 1;
    if (span == 0) return;
    ILI9341_DrawColorSpan(x, y0, span, color);
}
// 画背景网格
void Scope_DrawGrid(void)
{
    const uint16_t info_panel = Scope_InfoPanelHeight();
    const uint16_t grid_spacing = scope_cfg.grid_spacing_px;
    const uint16_t waveform_height = Scope_WaveformHeight();

    ILI9341_FillScreen(ILI9341_BLACK);
    ILI9341_FillRect(0, 0, ILI9341_WIDTH, info_panel, ILI9341_BLACK);

    for (int x = 0; x < ILI9341_WIDTH; x += grid_spacing) {
        for (int y = info_panel; y < ILI9341_HEIGHT; y++) {
            ILI9341_DrawPixel(x, y, ILI9341_BLUE);
        }
    }
    for (int y = info_panel; y < ILI9341_HEIGHT; y += grid_spacing) {
        for (int x = 0; x < ILI9341_WIDTH; x++) {
            ILI9341_DrawPixel(x, y, ILI9341_BLUE);
        }
    }

    // 初始化上一帧竖线范围：先都设成屏幕中间一个点
    for (int x = 0; x < ILI9341_WIDTH; x++) {
        uint16_t mid = info_panel + (waveform_height / 2);
        last_y_min[x] = mid;
        last_y_max[x] = mid;
    }
    first_draw = 1;
}



// 根据 ADC 数据画波形
// 按触发对齐后画波形，使用“擦旧点+画新点”的方式，避免网格闪烁
void Scope_DrawWaveform(uint16_t *buf, uint16_t len)
{
    if (len > scope_cfg.samples_per_frame) len = scope_cfg.samples_per_frame;
    if (len > ILI9341_WIDTH) len = ILI9341_WIDTH;
    const uint16_t waveform_height = Scope_WaveformHeight();
    const uint16_t info_panel = Scope_InfoPanelHeight();
    uint16_t frame_min = 0;
    uint16_t frame_max = 0;

    // 1. 触发对齐
    uint16_t trig = Scope_FindTriggerIndex(buf, len, &frame_min, &frame_max);
    uint32_t freq_hz = Scope_EstimateFrequencyHz(buf, len, trig, frame_min, frame_max);

    // 2. 先算出这一帧的 y 数组（触发对齐 + 简单平滑）
    uint16_t new_y[ILI9341_WIDTH];

    for (uint16_t i = 0; i < len; i++) {
        uint16_t idx = trig + i;
        if (idx >= len) idx -= len;

        // 代替那段 sum/cnt 的代码
        uint16_t val = buf[idx];   // 不做平滑，直接用原始采样值


        if (val > scope_cfg.adc_max_counts) val = scope_cfg.adc_max_counts;
        uint16_t y = info_panel + (waveform_height - 1)
                   - (val * (waveform_height - 1) / scope_cfg.adc_max_counts);

        new_y[i] = y;
    }

    // 3. 对每一个 x：先擦旧竖线，再画新竖线，再更新范围
    for (uint16_t x = 0; x < len; x++) {

        // 3.1 擦掉上一帧在这个 x 上画过的竖线
        if (!first_draw) {
            Scope_EraseColumn(x, last_y_min[x], last_y_max[x]);
        }

        // 3.2 计算这一帧在这个 x 上的新竖线范围
        uint16_t y1 = new_y[x];
        uint16_t y0 = (x > 0) ? new_y[x - 1] : new_y[x];

        uint16_t ymin_new = (y0 < y1) ? y0 : y1;
        uint16_t ymax_new = (y0 > y1) ? y0 : y1;
        if (ymax_new >= ILI9341_HEIGHT) ymax_new = ILI9341_HEIGHT - 1;

        // 3.3 画新竖线
        Scope_DrawColumn(x, ymin_new, ymax_new, scope_cfg.waveform_color);

        // 3.4 更新记录
        last_y_min[x] = ymin_new;
        last_y_max[x] = ymax_new;
    }

    first_draw = 0;
    Scope_DrawMeasurements(frame_min, frame_max, freq_hz);
}

static uint32_t Scope_EstimateFrequencyHz(uint16_t *buf, uint16_t len,
                                          uint16_t trig_idx,
                                          uint16_t frame_min,
                                          uint16_t frame_max)
{
    if (buf == NULL || len < 4U) {
        return 0U;
    }

    uint16_t amplitude = frame_max - frame_min;
    if (amplitude < scope_cfg.trigger_min_delta) {
        return 0U;
    }

    uint16_t threshold = (uint16_t)((frame_min + frame_max) / 2U);
    uint16_t base_idx = trig_idx;
    if (base_idx >= len) {
        base_idx %= len;
    }
    uint16_t prev = buf[base_idx];
    uint16_t last_edge_offset = 0;
    uint32_t period_sum = 0;
    uint8_t period_count = 0;

    for (uint16_t step = 1; step < len; ++step) {
        uint16_t idx = trig_idx + step;
        if (idx >= len) {
            idx -= len;
        }
        uint16_t curr = buf[idx];
        if (prev < threshold && curr >= threshold) {
            uint16_t delta = step - last_edge_offset;
            if (delta > 0U) {
                period_sum += delta;
                period_count++;
                last_edge_offset = step;
                if (period_count >= 3U) {
                    break;
                }
            }
        }
        prev = curr;
    }

    if (period_count == 0U) {
        return 0U;
    }

    uint32_t avg_period = period_sum / period_count;
    if (avg_period == 0U) {
        return 0U;
    }

    uint32_t sample_rate = Scope_GetSampleRateHz();
    if (sample_rate == 0U) {
        return 0U;
    }

    return sample_rate / avg_period;
}

static uint32_t Scope_GetSampleRateHz(void)
{
    if (scope_sample_rate_hz == 0U) {
        scope_sample_rate_hz = Scope_ComputeSampleRateHz();
    }
    return scope_sample_rate_hz;
}

static uint32_t Scope_ComputeSampleRateHz(void)
{
    uint32_t tim_clk = HAL_RCC_GetPCLK1Freq();
    if (tim_clk == 0U) {
        return 0U;
    }

    RCC_ClkInitTypeDef clk_config;
    uint32_t flash_latency;
    HAL_RCC_GetClockConfig(&clk_config, &flash_latency);
    if (clk_config.APB1CLKDivider != RCC_HCLK_DIV1) {
        tim_clk *= 2U;
    }

    uint32_t psc = (uint32_t)htim3.Init.Prescaler + 1U;
    uint32_t arr = (uint32_t)htim3.Init.Period + 1U;
    if (psc == 0U || arr == 0U) {
        return 0U;
    }

    return tim_clk / (psc * arr);
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
    Scope_DrawMeasurements(0, 0, 0U);

    // 启动 ADC + DMA
    HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_buf, scope_cfg.samples_per_frame);

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
	      Scope_DrawWaveform(adc_buf, scope_cfg.samples_per_frame);
	      // 这里现在已经是有触发对齐和擦除逻辑的“简易示波器”了
	  }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

  }
  /* USER CODE END 3 */
}

static uint32_t Scope_AdcToMillivolt(uint16_t sample)
{
    // +2047 实现四舍五入，避免在转换成毫伏时总是向下取整
    uint32_t rounding = scope_cfg.adc_max_counts / 2U;
    return ((uint32_t)sample * scope_cfg.adc_ref_millivolt + rounding)
           / scope_cfg.adc_max_counts;
}

void Scope_DrawMeasurements(uint16_t vmin, uint16_t vmax, uint32_t freq_hz)
{
    char line1[32];
    char line2[32];
    char line3[32];
    uint32_t vmin_mv = Scope_AdcToMillivolt(vmin);
    uint32_t vmax_mv = Scope_AdcToMillivolt(vmax);
    uint32_t vmax_whole = vmax_mv / 1000U;
    uint32_t vmax_frac = (vmax_mv % 1000U) / 10U;
    uint32_t vmin_whole = vmin_mv / 1000U;
    uint32_t vmin_frac = (vmin_mv % 1000U) / 10U;

    snprintf(line1, sizeof(line1), "Vmax: %lu.%02lu V",
             (unsigned long)vmax_whole,
             (unsigned long)vmax_frac);
    snprintf(line2, sizeof(line2), "Vmin: %lu.%02lu V",
             (unsigned long)vmin_whole,
             (unsigned long)vmin_frac);

    if (freq_hz == 0U) {
        snprintf(line3, sizeof(line3), "Freq: ---");
    } else if (freq_hz >= 1000000U) {
        uint32_t whole = freq_hz / 1000000U;
        uint32_t frac = (freq_hz % 1000000U) / 1000U;
        snprintf(line3, sizeof(line3), "Freq: %lu.%03lu MHz",
                 (unsigned long)whole,
                 (unsigned long)frac);
    } else if (freq_hz >= 1000U) {
        uint32_t whole = freq_hz / 1000U;
        uint32_t frac = (freq_hz % 1000U) / 10U;
        snprintf(line3, sizeof(line3), "Freq: %lu.%02lu kHz",
                 (unsigned long)whole,
                 (unsigned long)frac);
    } else {
        snprintf(line3, sizeof(line3), "Freq: %lu Hz",
                 (unsigned long)freq_hz);
    }

    ILI9341_FillRect(0, 0, ILI9341_WIDTH, Scope_InfoPanelHeight(), ILI9341_BLACK);
    ILI9341_DrawString(4, 4, line1, ILI9341_YELLOW, ILI9341_BLACK, 2);
    ILI9341_DrawString(4, 24, line2, ILI9341_GREEN, ILI9341_BLACK, 2);
    ILI9341_DrawString(4, 44, line3, ILI9341_WHITE, ILI9341_BLACK, 2);
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
