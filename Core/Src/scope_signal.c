#include "scope_signal.h"

#include "main.h"
#include "tim.h"

static uint32_t scope_sample_rate_hz = 0U;

static uint32_t ScopeSignal_ComputeSampleRateHz(void);

uint16_t ScopeSignal_FindTriggerIndex(uint16_t *buf,
                                      uint16_t len,
                                      uint16_t trigger_min_delta,
                                      uint16_t *out_min,
                                      uint16_t *out_max)
{
    if (buf == NULL || len < 2U)
    {
        if (out_min != NULL)
        {
            *out_min = 0U;
        }
        if (out_max != NULL)
        {
            *out_max = 0U;
        }
        return 0U;
    }

    uint16_t vmin = 0xFFFFU;
    uint16_t vmax = 0U;
    for (uint16_t i = 0; i < len; i++)
    {
        uint16_t v = buf[i];
        if (v < vmin)
        {
            vmin = v;
        }
        if (v > vmax)
        {
            vmax = v;
        }
    }

    if (out_min != NULL)
    {
        *out_min = vmin;
    }
    if (out_max != NULL)
    {
        *out_max = vmax;
    }

    if ((uint16_t)(vmax - vmin) < trigger_min_delta)
    {
        return 0U;
    }

    uint16_t thr = (uint16_t)((vmin + vmax) / 2U);

    for (uint16_t i = 1; i < len; i++)
    {
        uint16_t v_prev = buf[i - 1U];
        uint16_t v_now = buf[i];
        if (v_prev < thr && v_now >= thr)
        {
            return i;
        }
    }

    return 0U;
}

uint32_t ScopeSignal_EstimatePeriodSamples(uint16_t *buf,
                                           uint16_t len,
                                           uint16_t trig_idx,
                                           uint16_t frame_min,
                                           uint16_t frame_max,
                                           uint16_t trigger_min_delta)
{
    if (buf == NULL || len < 4U)
    {
        return 0U;
    }

    uint16_t amplitude = frame_max - frame_min;
    if (amplitude < trigger_min_delta)
    {
        return 0U;
    }

    uint16_t threshold = (uint16_t)((frame_min + frame_max) / 2U);
    uint16_t base_idx = trig_idx;
    if (base_idx >= len)
    {
        base_idx %= len;
    }
    uint16_t prev = buf[base_idx];
    uint16_t last_edge_offset = 0;
    uint32_t period_sum = 0;
    uint8_t period_count = 0;

    for (uint16_t step = 1U; step < len; ++step)
    {
        uint16_t idx = trig_idx + step;
        if (idx >= len)
        {
            idx -= len;
        }
        uint16_t curr = buf[idx];
        if (prev < threshold && curr >= threshold)
        {
            uint16_t delta = step - last_edge_offset;
            if (delta > 0U)
            {
                period_sum += delta;
                period_count++;
                last_edge_offset = step;
                if (period_count >= 3U)
                {
                    break;
                }
            }
        }
        prev = curr;
    }

    if (period_count == 0U)
    {
        return 0U;
    }

    uint32_t avg_period = period_sum / period_count;
    if (avg_period == 0U)
    {
        return 0U;
    }

    return avg_period;
}

uint32_t ScopeSignal_GetSampleRateHz(void)
{
    if (scope_sample_rate_hz == 0U)
    {
        scope_sample_rate_hz = ScopeSignal_ComputeSampleRateHz();
    }
    return scope_sample_rate_hz;
}

static uint32_t ScopeSignal_ComputeSampleRateHz(void)
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

    uint32_t psc = (uint32_t)htim3.Init.Prescaler + 1U;
    uint32_t arr = (uint32_t)htim3.Init.Period + 1U;
    if (psc == 0U || arr == 0U)
    {
        return 0U;
    }

    return tim_clk / (psc * arr);
}

uint32_t ScopeSignal_AdcToMillivolt(uint16_t sample,
                                    uint16_t adc_max_counts,
                                    uint16_t adc_ref_millivolt)
{
    if (adc_max_counts == 0U)
    {
        return 0U;
    }

    uint32_t rounding = adc_max_counts / 2U;
    return ((uint32_t)sample * adc_ref_millivolt + rounding) / adc_max_counts;
}
