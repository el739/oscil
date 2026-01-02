/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    waveform_control.c
  * @brief   Waveform frequency control implementation
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

#include "waveform_control.h"
#include "dac.h"
#include "tim.h"
#include "main.h"

#define SINE_SAMPLE_COUNT   (64U)
#define SINE_MIN_FREQ_HZ    (1U)
#define SINE_MAX_FREQ_HZ    (5000U)
#define SINE_DEFAULT_FREQ_HZ (1000U)

static const uint16_t sine_lut[SINE_SAMPLE_COUNT] = {
    2048, 2249, 2447, 2642, 2831, 3013, 3185, 3347,
    3495, 3630, 3750, 3853, 3939, 4007, 4056, 4085,
    4095, 4085, 4056, 4007, 3939, 3853, 3750, 3630,
    3495, 3347, 3185, 3013, 2831, 2642, 2447, 2249,
    2048, 1847, 1649, 1454, 1265, 1083, 911, 749,
    601, 466, 346, 243, 157, 89, 40, 11,
    1, 11, 40, 89, 157, 243, 346, 466,
    601, 749, 911, 1083, 1265, 1454, 1649, 1847
};

static uint8_t dac_started = 0U;

static uint32_t ComputeTim4ClockHz(void);
static uint8_t ApplyWaveformFrequency(uint32_t target_hz);

void WaveformControl_Init(void)
{
    if (dac_started == 0U)
    {
        /* Start DAC with DMA in circular mode; data transfers on TIM4 TRGO. */
        if (HAL_DAC_Start_DMA(&hdac, DAC_CHANNEL_1, (uint32_t *)sine_lut, SINE_SAMPLE_COUNT, DAC_ALIGN_12B_R) != HAL_OK)
        {
            Error_Handler();
        }
        __HAL_TIM_SET_COUNTER(&htim4, 0U);
        if (HAL_TIM_Base_Start(&htim4) != HAL_OK)
        {
            Error_Handler();
        }
        dac_started = 1U;
    }

    (void)WaveformControl_SetFrequency(SINE_DEFAULT_FREQ_HZ);
}

uint8_t WaveformControl_SetFrequency(uint32_t target_hz)
{
    if (target_hz < SINE_MIN_FREQ_HZ || target_hz > SINE_MAX_FREQ_HZ)
    {
        return 0U;
    }

    return ApplyWaveformFrequency(target_hz);
}

static uint32_t ComputeTim4ClockHz(void)
{
    uint32_t tim_clk = HAL_RCC_GetPCLK1Freq();
    if (tim_clk == 0U)
    {
        return 0U;
    }

    RCC_ClkInitTypeDef clk_config;
    uint32_t flash_latency;
    HAL_RCC_GetClockConfig(&clk_config, &flash_latency);
    if (clk_config.APB1CLKDivider != RCC_HCLK_DIV1)
    {
        tim_clk *= 2U;
    }

    return tim_clk;
}

static uint8_t ApplyWaveformFrequency(uint32_t target_hz)
{
    uint32_t tim_clk = ComputeTim4ClockHz();
    if (tim_clk == 0U)
    {
        return 0U;
    }

    uint32_t sample_rate = target_hz * SINE_SAMPLE_COUNT;
    if (sample_rate == 0U)
    {
        return 0U;
    }

    uint32_t psc = 0U;
    uint32_t arr_plus_one = tim_clk / sample_rate;
    while (arr_plus_one == 0U || arr_plus_one > 0x10000U)
    {
        psc++;
        if (psc > 0xFFFFU)
        {
            return 0U;
        }
        arr_plus_one = tim_clk / ((psc + 1U) * sample_rate);
    }

    uint32_t arr = arr_plus_one - 1U;

    __HAL_TIM_DISABLE(&htim4);
    __HAL_TIM_SET_PRESCALER(&htim4, psc);
    __HAL_TIM_SET_AUTORELOAD(&htim4, arr);
    __HAL_TIM_SET_COUNTER(&htim4, 0U);
    HAL_TIM_GenerateEvent(&htim4, TIM_EVENTSOURCE_UPDATE);
    __HAL_TIM_ENABLE(&htim4);

    return 1U;
}
