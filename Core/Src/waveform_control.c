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
#include "tim.h"
#include "main.h"

static uint32_t ComputeTim1ClockHz(void);
static uint8_t ApplyWaveformFrequency(uint32_t target_hz);

void WaveformControl_Init(void)
{
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
}

uint8_t WaveformControl_SetFrequency(uint32_t target_hz)
{
    return ApplyWaveformFrequency(target_hz);
}

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
