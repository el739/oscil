#include "scope.h"

#include "main.h"
#include "tim.h"

#include <stdio.h>
#include <string.h>

typedef struct
{
    uint16_t samples_per_frame;
    uint16_t info_panel_height;
    uint16_t grid_spacing_px;
    uint16_t trigger_min_delta;
    uint16_t adc_ref_millivolt;
    uint16_t adc_max_counts;
    uint16_t waveform_color;
} ScopeConfig;

static const ScopeConfig scope_cfg = {
    .samples_per_frame = SCOPE_FRAME_SAMPLES,
    .info_panel_height = 64U,
    .grid_spacing_px = 40U,
    .trigger_min_delta = 20U,
    .adc_ref_millivolt = 3300U,
    .adc_max_counts = 4095U,
    .waveform_color = ILI9341_YELLOW
};

typedef struct
{
    uint32_t span_counts;
    int32_t center_counts;
} ScopeVerticalSettings;

typedef struct
{
    uint32_t samples_visible;
    int32_t center_sample;
} ScopeHorizontalSettings;

typedef struct
{
    ScopeVerticalSettings vertical;
    ScopeHorizontalSettings horizontal;
} ScopeDisplaySettings;

typedef struct
{
    volatile uint8_t autoset_request;
} ScopeControlFlags;

static ScopeDisplaySettings scope_display_settings;
static ScopeControlFlags scope_control = {0};
static uint16_t last_y_min[SCOPE_FRAME_SAMPLES];
static uint16_t last_y_max[SCOPE_FRAME_SAMPLES];
static uint16_t scope_column_buf[ILI9341_HEIGHT];
static uint8_t first_draw = 1U;
static uint32_t scope_sample_rate_hz = 0U;

static inline uint16_t Scope_InfoPanelHeight(void)
{
    return scope_cfg.info_panel_height;
}

static inline uint16_t Scope_WaveformHeight(void)
{
    return ILI9341_HEIGHT - scope_cfg.info_panel_height;
}

static void Scope_DrawGrid(void);
static void Scope_DrawWaveform(uint16_t *buf, uint16_t len);
static void Scope_DrawMeasurements(uint16_t vmin, uint16_t vmax, uint32_t freq_hz);
static void Scope_DisplaySettingsInit(void);
static void Scope_ResetVerticalWindow(void);
static void Scope_UpdateVerticalWindow(uint32_t span, int32_t center);
static int32_t Scope_SampleToY(int32_t sample);
static uint8_t Scope_ClipWaveformSegment(int32_t *y0, int32_t *y1);
static void Scope_EraseColumn(uint16_t x, uint16_t y0, uint16_t y1);
static void Scope_DrawColumn(uint16_t x, uint16_t y0, uint16_t y1, uint16_t color);
static uint16_t Scope_FindTriggerIndex(uint16_t *buf, uint16_t len,
                                       uint16_t *out_min, uint16_t *out_max);
static uint32_t Scope_EstimateFrequencyHz(uint16_t *buf, uint16_t len,
                                          uint16_t trig_idx,
                                          uint16_t frame_min,
                                          uint16_t frame_max);
static uint32_t Scope_GetSampleRateHz(void);
static uint32_t Scope_ComputeSampleRateHz(void);
static uint32_t Scope_AdcToMillivolt(uint16_t sample);
static void Scope_UpdateInfoLine(uint16_t x, uint16_t y, const char *text,
                                 uint16_t color, char *last_text, size_t buf_len);
static void Scope_ApplyAutoSet(uint16_t *buf, uint16_t len);
static uint8_t Scope_ConsumeAutoSetRequest(void);
static uint16_t Scope_BackgroundColor(uint16_t x, uint16_t y);

uint16_t Scope_FrameSampleCount(void)
{
    return scope_cfg.samples_per_frame;
}

void Scope_Init(void)
{
    ILI9341_Init();
    Scope_DisplaySettingsInit();
    Scope_DrawGrid();
    Scope_DrawMeasurements(0U, 0U, 0U);
}

void Scope_ProcessFrame(uint16_t *samples, uint16_t count)
{
    if (samples == NULL || count == 0U)
    {
        return;
    }

    if (count > scope_cfg.samples_per_frame)
    {
        count = scope_cfg.samples_per_frame;
    }

    if (Scope_ConsumeAutoSetRequest())
    {
        Scope_ApplyAutoSet(samples, count);
    }

    Scope_DrawWaveform(samples, count);
}

void Scope_RequestAutoSet(void)
{
    scope_control.autoset_request = 1U;
}

static uint8_t Scope_ConsumeAutoSetRequest(void)
{
    uint8_t pending;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    pending = scope_control.autoset_request;
    scope_control.autoset_request = 0U;
    if (primask == 0U)
    {
        __enable_irq();
    }
    return pending;
}

static void Scope_DisplaySettingsInit(void)
{
    Scope_ResetVerticalWindow();
    scope_display_settings.horizontal.samples_visible = scope_cfg.samples_per_frame;
    scope_display_settings.horizontal.center_sample = scope_cfg.samples_per_frame / 2U;
}

static void Scope_ResetVerticalWindow(void)
{
    Scope_UpdateVerticalWindow(scope_cfg.adc_max_counts,
                               scope_cfg.adc_max_counts / 2U);
}

static void Scope_UpdateVerticalWindow(uint32_t span, int32_t center)
{
    if (span == 0U)
    {
        span = 1U;
    }
    if (span < scope_cfg.trigger_min_delta)
    {
        span = scope_cfg.trigger_min_delta;
    }
    if (center < 0)
    {
        center = 0;
    }
    if (center > (int32_t)scope_cfg.adc_max_counts)
    {
        center = scope_cfg.adc_max_counts;
    }
    scope_display_settings.vertical.span_counts = span;
    scope_display_settings.vertical.center_counts = center;
}

static int32_t Scope_SampleToY(int32_t sample)
{
    const int32_t info_panel = (int32_t)Scope_InfoPanelHeight();
    const int32_t waveform_height = (int32_t)Scope_WaveformHeight();
    int32_t span = (int32_t)scope_display_settings.vertical.span_counts;
    if (span <= 0)
    {
        span = (int32_t)scope_cfg.adc_max_counts;
    }
    int32_t half_span = span / 2;
    int32_t lower = scope_display_settings.vertical.center_counts - half_span;
    int32_t relative = sample - lower;
    int32_t y = info_panel + (waveform_height - 1)
                - (relative * (waveform_height - 1)) / span;
    return y;
}

static uint8_t Scope_ClipWaveformSegment(int32_t *y0, int32_t *y1)
{
    if (y0 == NULL || y1 == NULL)
    {
        return 0U;
    }
    int32_t top = (int32_t)Scope_InfoPanelHeight();
    int32_t bottom = (int32_t)ILI9341_HEIGHT - 1;

    if ((*y0 < top && *y1 < top) || (*y0 > bottom && *y1 > bottom))
    {
        return 0U;
    }

    if (*y0 < top)
    {
        *y0 = top;
    }
    else if (*y0 > bottom)
    {
        *y0 = bottom;
    }

    if (*y1 < top)
    {
        *y1 = top;
    }
    else if (*y1 > bottom)
    {
        *y1 = bottom;
    }

    return 1U;
}

static uint16_t Scope_BackgroundColor(uint16_t x, uint16_t y)
{
    const uint16_t info_panel = Scope_InfoPanelHeight();
    if (y < info_panel)
    {
        return ILI9341_BLACK;
    }

    uint16_t gy = y - info_panel;
    if ((x % scope_cfg.grid_spacing_px) == 0 ||
        (gy % scope_cfg.grid_spacing_px) == 0)
    {
        return ILI9341_BLUE;
    }
    return ILI9341_BLACK;
}

static void Scope_EraseColumn(uint16_t x, uint16_t y0, uint16_t y1)
{
    if (x >= ILI9341_WIDTH)
    {
        return;
    }
    if (y0 > y1)
    {
        uint16_t tmp = y0;
        y0 = y1;
        y1 = tmp;
    }
    if (y0 >= ILI9341_HEIGHT)
    {
        return;
    }
    if (y1 >= ILI9341_HEIGHT)
    {
        y1 = ILI9341_HEIGHT - 1;
    }
    uint16_t span = y1 - y0 + 1;
    if (span == 0)
    {
        return;
    }
    for (uint16_t i = 0; i < span; i++)
    {
        scope_column_buf[i] = Scope_BackgroundColor(x, y0 + i);
    }
    ILI9341_DrawPixels(x, y0, scope_column_buf, span);
}

static void Scope_DrawColumn(uint16_t x, uint16_t y0, uint16_t y1, uint16_t color)
{
    if (x >= ILI9341_WIDTH)
    {
        return;
    }
    if (y0 > y1)
    {
        uint16_t tmp = y0;
        y0 = y1;
        y1 = tmp;
    }
    if (y0 >= ILI9341_HEIGHT)
    {
        return;
    }
    if (y1 >= ILI9341_HEIGHT)
    {
        y1 = ILI9341_HEIGHT - 1;
    }
    uint16_t span = y1 - y0 + 1;
    if (span == 0)
    {
        return;
    }
    ILI9341_DrawColorSpan(x, y0, span, color);
}

static void Scope_ApplyAutoSet(uint16_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0U)
    {
        return;
    }

    uint16_t vmin = 0xFFFF;
    uint16_t vmax = 0;
    for (uint16_t i = 0; i < len; ++i)
    {
        uint16_t sample = buf[i];
        if (sample < vmin)
        {
            vmin = sample;
        }
        if (sample > vmax)
        {
            vmax = sample;
        }
    }

    if (vmax < vmin)
    {
        Scope_ResetVerticalWindow();
        return;
    }

    uint32_t span = (uint32_t)vmax - (uint32_t)vmin;
    if (span < scope_cfg.trigger_min_delta)
    {
        Scope_ResetVerticalWindow();
        return;
    }

    uint32_t margin_span = span + span / 5U;
    if (margin_span == 0U)
    {
        margin_span = scope_cfg.trigger_min_delta;
    }

    uint32_t center = (uint32_t)vmin + span / 2U;
    Scope_UpdateVerticalWindow(margin_span, (int32_t)center);
}

static uint16_t Scope_FindTriggerIndex(uint16_t *buf, uint16_t len,
                                       uint16_t *out_min, uint16_t *out_max)
{
    if (len < 2)
    {
        return 0;
    }

    uint16_t vmin = 0xFFFF;
    uint16_t vmax = 0;
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

    if (out_min)
    {
        *out_min = vmin;
    }
    if (out_max)
    {
        *out_max = vmax;
    }

    if ((uint16_t)(vmax - vmin) < scope_cfg.trigger_min_delta)
    {
        return 0;
    }

    uint16_t thr = (uint16_t)((vmin + vmax) / 2);

    for (uint16_t i = 1; i < len; i++)
    {
        uint16_t v_prev = buf[i - 1];
        uint16_t v_now = buf[i];
        if (v_prev < thr && v_now >= thr)
        {
            return i;
        }
    }

    return 0;
}

static void Scope_DrawGrid(void)
{
    const uint16_t info_panel = Scope_InfoPanelHeight();
    const uint16_t grid_spacing = scope_cfg.grid_spacing_px;
    const uint16_t waveform_height = Scope_WaveformHeight();

    ILI9341_FillScreen(ILI9341_BLACK);
    ILI9341_FillRect(0, 0, ILI9341_WIDTH, info_panel, ILI9341_BLACK);

    for (int x = 0; x < ILI9341_WIDTH; x += grid_spacing)
    {
        for (int y = info_panel; y < ILI9341_HEIGHT; y++)
        {
            ILI9341_DrawPixel(x, y, ILI9341_BLUE);
        }
    }
    for (int y = info_panel; y < ILI9341_HEIGHT; y += grid_spacing)
    {
        for (int x = 0; x < ILI9341_WIDTH; x++)
        {
            ILI9341_DrawPixel(x, y, ILI9341_BLUE);
        }
    }

    for (int x = 0; x < ILI9341_WIDTH; x++)
    {
        uint16_t mid = info_panel + (waveform_height / 2);
        last_y_min[x] = mid;
        last_y_max[x] = mid;
    }
    first_draw = 1;
}

static void Scope_DrawWaveform(uint16_t *buf, uint16_t len)
{
    if (len > scope_cfg.samples_per_frame)
    {
        len = scope_cfg.samples_per_frame;
    }
    if (len > ILI9341_WIDTH)
    {
        len = ILI9341_WIDTH;
    }
    uint16_t frame_min = 0;
    uint16_t frame_max = 0;

    uint16_t trig = Scope_FindTriggerIndex(buf, len, &frame_min, &frame_max);
    uint32_t freq_hz = Scope_EstimateFrequencyHz(buf, len, trig, frame_min, frame_max);

    int32_t new_y[SCOPE_FRAME_SAMPLES];
    for (uint16_t i = 0; i < len; i++)
    {
        uint16_t idx = trig + i;
        if (idx >= len)
        {
            idx -= len;
        }

        uint16_t val = buf[idx];
        if (val > scope_cfg.adc_max_counts)
        {
            val = scope_cfg.adc_max_counts;
        }
        new_y[i] = Scope_SampleToY((int32_t)val);
    }

    for (uint16_t x = 0; x < len; x++)
    {
        if (!first_draw)
        {
            Scope_EraseColumn(x, last_y_min[x], last_y_max[x]);
        }

        int32_t y1 = new_y[x];
        int32_t y0 = (x > 0) ? new_y[x - 1] : new_y[x];

        uint16_t ymin_new = Scope_InfoPanelHeight();
        uint16_t ymax_new = Scope_InfoPanelHeight();

        if (Scope_ClipWaveformSegment(&y0, &y1))
        {
            int32_t ymin_clip = (y0 < y1) ? y0 : y1;
            int32_t ymax_clip = (y0 > y1) ? y0 : y1;
            if (ymax_clip >= (int32_t)ILI9341_HEIGHT)
            {
                ymax_clip = ILI9341_HEIGHT - 1;
            }
            if (ymin_clip < (int32_t)Scope_InfoPanelHeight())
            {
                ymin_clip = Scope_InfoPanelHeight();
            }
            Scope_DrawColumn(x, (uint16_t)ymin_clip, (uint16_t)ymax_clip,
                             scope_cfg.waveform_color);
            ymin_new = (uint16_t)ymin_clip;
            ymax_new = (uint16_t)ymax_clip;
        }

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
    if (buf == NULL || len < 4U)
    {
        return 0U;
    }

    uint16_t amplitude = frame_max - frame_min;
    if (amplitude < scope_cfg.trigger_min_delta)
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

    for (uint16_t step = 1; step < len; ++step)
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

    uint32_t sample_rate = Scope_GetSampleRateHz();
    if (sample_rate == 0U)
    {
        return 0U;
    }

    return sample_rate / avg_period;
}

static uint32_t Scope_GetSampleRateHz(void)
{
    if (scope_sample_rate_hz == 0U)
    {
        scope_sample_rate_hz = Scope_ComputeSampleRateHz();
    }
    return scope_sample_rate_hz;
}

static uint32_t Scope_ComputeSampleRateHz(void)
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

static uint32_t Scope_AdcToMillivolt(uint16_t sample)
{
    uint32_t rounding = scope_cfg.adc_max_counts / 2U;
    return ((uint32_t)sample * scope_cfg.adc_ref_millivolt + rounding)
           / scope_cfg.adc_max_counts;
}

static void Scope_UpdateInfoLine(uint16_t x, uint16_t y, const char *text,
                                 uint16_t color, char *last_text, size_t buf_len)
{
    const uint8_t font_size = 2U;
    const uint16_t char_width = 6U * font_size;
    const uint16_t char_height = 8U * font_size;

    if (text == NULL || last_text == NULL || buf_len == 0U)
    {
        return;
    }

    if (strncmp(text, last_text, buf_len) == 0)
    {
        return;
    }

    ILI9341_DrawString(x, y, text, color, ILI9341_BLACK, font_size);

    size_t new_len = strlen(text);
    size_t prev_len = strlen(last_text);
    if (new_len < prev_len)
    {
        uint16_t clear_x = x + (uint16_t)(new_len * char_width);
        uint16_t clear_width = (uint16_t)((prev_len - new_len) * char_width);
        ILI9341_FillRect(clear_x, y, clear_width, char_height, ILI9341_BLACK);
    }

    strncpy(last_text, text, buf_len);
    last_text[buf_len - 1U] = '\0';
}

static void Scope_DrawMeasurements(uint16_t vmin, uint16_t vmax, uint32_t freq_hz)
{
    static char last_line1[32];
    static char last_line2[32];
    static char last_line3[32];
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

    if (freq_hz == 0U)
    {
        snprintf(line3, sizeof(line3), "Freq: ---");
    }
    else if (freq_hz >= 1000000U)
    {
        uint32_t whole = freq_hz / 1000000U;
        uint32_t frac = (freq_hz % 1000000U) / 1000U;
        snprintf(line3, sizeof(line3), "Freq: %lu.%03lu MHz",
                 (unsigned long)whole,
                 (unsigned long)frac);
    }
    else if (freq_hz >= 1000U)
    {
        uint32_t whole = freq_hz / 1000U;
        uint32_t frac = (freq_hz % 1000U) / 10U;
        snprintf(line3, sizeof(line3), "Freq: %lu.%02lu kHz",
                 (unsigned long)whole,
                 (unsigned long)frac);
    }
    else
    {
        snprintf(line3, sizeof(line3), "Freq: %lu Hz",
                 (unsigned long)freq_hz);
    }

    Scope_UpdateInfoLine(4U, 4U, line1, ILI9341_YELLOW, last_line1, sizeof(last_line1));
    Scope_UpdateInfoLine(4U, 24U, line2, ILI9341_GREEN, last_line2, sizeof(last_line2));
    Scope_UpdateInfoLine(4U, 44U, line3, ILI9341_WHITE, last_line3, sizeof(last_line3));
}
