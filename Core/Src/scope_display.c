#include "scope_display.h"

#include "scope.h"
#include "ili9341.h"
#include "scope_signal.h"

#include <stdio.h>
#include <string.h>

typedef struct
{
    ScopeDisplayConfig cfg;
    uint8_t initialized;
} ScopeDisplayModule;

static ScopeDisplayModule scope_display_module;
static uint16_t last_y_min[SCOPE_FRAME_SAMPLES];
static uint16_t last_y_max[SCOPE_FRAME_SAMPLES];
static uint16_t scope_column_buf[ILI9341_HEIGHT];
static uint8_t first_draw = 1U;

static inline uint16_t ScopeDisplay_InfoPanelHeight(void)
{
    return scope_display_module.cfg.info_panel_height;
}

static inline uint16_t ScopeDisplay_WaveformHeight(void)
{
    return ILI9341_HEIGHT - scope_display_module.cfg.info_panel_height;
}

static uint16_t ScopeDisplay_BackgroundColor(uint16_t x, uint16_t y);
static int32_t ScopeDisplay_SampleToY(const ScopeDisplaySettings *settings, int32_t sample);
static uint8_t ScopeDisplay_ClipWaveformSegment(int32_t *y0, int32_t *y1);
static void ScopeDisplay_EraseColumn(uint16_t x, uint16_t y0, uint16_t y1);
static void ScopeDisplay_DrawColumn(uint16_t x, uint16_t y0, uint16_t y1);
static void ScopeDisplay_UpdateMeasurements(uint16_t vmin, uint16_t vmax, uint32_t freq_hz);
static void ScopeDisplay_UpdateInfoLine(uint16_t x, uint16_t y, const char *text,
                                        uint16_t color, char *last_text, size_t buf_len);

void ScopeDisplay_Init(const ScopeDisplayConfig *cfg)
{
    if (cfg == NULL)
    {
        scope_display_module.initialized = 0U;
        return;
    }

    scope_display_module.cfg = *cfg;
    scope_display_module.initialized = 1U;
    first_draw = 1U;
}

void ScopeDisplay_DrawGrid(void)
{
    if (!scope_display_module.initialized)
    {
        return;
    }

    const uint16_t info_panel = ScopeDisplay_InfoPanelHeight();
    const uint16_t grid_spacing = scope_display_module.cfg.grid_spacing_px;
    const uint16_t waveform_height = ScopeDisplay_WaveformHeight();

    ILI9341_FillScreen(ILI9341_BLACK);
    ILI9341_FillRect(0, 0, ILI9341_WIDTH, info_panel, ILI9341_BLACK);

    for (int x = 0; x < ILI9341_WIDTH; x += grid_spacing)
    {
        for (int y = info_panel; y < ILI9341_HEIGHT; y++)
        {
            ILI9341_DrawPixel((uint16_t)x, (uint16_t)y, ILI9341_BLUE);
        }
    }
    for (int y = info_panel; y < ILI9341_HEIGHT; y += grid_spacing)
    {
        for (int x = 0; x < ILI9341_WIDTH; x++)
        {
            ILI9341_DrawPixel((uint16_t)x, (uint16_t)y, ILI9341_BLUE);
        }
    }

    for (int x = 0; x < ILI9341_WIDTH; x++)
    {
        uint16_t mid = info_panel + (waveform_height / 2U);
        last_y_min[x] = mid;
        last_y_max[x] = mid;
    }

    first_draw = 1U;
}

void ScopeDisplay_DrawWaveform(const ScopeDisplaySettings *settings,
                               uint16_t *samples,
                               uint16_t count,
                               uint16_t visible_samples,
                               uint16_t trigger_min_delta)
{
    if (!scope_display_module.initialized ||
        settings == NULL ||
        samples == NULL ||
        count == 0U ||
        visible_samples == 0U)
    {
        return;
    }

    if (count > scope_display_module.cfg.frame_samples)
    {
        count = scope_display_module.cfg.frame_samples;
    }

    uint16_t frame_min = 0;
    uint16_t frame_max = 0;
    uint16_t trig = ScopeSignal_FindTriggerIndex(samples, count, trigger_min_delta,
                                                 &frame_min, &frame_max);
    uint32_t period_samples = ScopeSignal_EstimatePeriodSamples(samples,
                                                                count,
                                                                trig,
                                                                frame_min,
                                                                frame_max,
                                                                trigger_min_delta);
    uint32_t freq_hz = 0U;
    if (period_samples != 0U)
    {
        uint32_t sample_rate = ScopeSignal_GetSampleRateHz();
        if (sample_rate != 0U)
        {
            freq_hz = sample_rate / period_samples;
        }
    }

    int32_t center_sample = settings->horizontal.center_sample;
    int32_t start_offset = (int32_t)trig - center_sample;
    int32_t samples_in_frame = (int32_t)count;

    uint16_t draw_width = scope_display_module.cfg.frame_samples;
    if (draw_width > ILI9341_WIDTH)
    {
        draw_width = ILI9341_WIDTH;
    }
    if (draw_width == 0U)
    {
        return;
    }

    int32_t new_y[SCOPE_FRAME_SAMPLES];
    for (uint16_t i = 0; i < draw_width; i++)
    {
        uint32_t scaled = ((uint32_t)i * visible_samples);
        int32_t relative = start_offset + (int32_t)(scaled / draw_width);
        int32_t wrapped = relative % samples_in_frame;
        if (wrapped < 0)
        {
            wrapped += samples_in_frame;
        }
        uint16_t idx = (uint16_t)wrapped;

        uint16_t val = samples[idx];
        if (val > scope_display_module.cfg.adc_max_counts)
        {
            val = scope_display_module.cfg.adc_max_counts;
        }
        new_y[i] = ScopeDisplay_SampleToY(settings, (int32_t)val);
    }

    for (uint16_t x = 0; x < draw_width; x++)
    {
        if (!first_draw)
        {
            ScopeDisplay_EraseColumn(x, last_y_min[x], last_y_max[x]);
        }

        int32_t y1 = new_y[x];
        int32_t y0 = (x > 0) ? new_y[x - 1] : new_y[x];

        uint16_t ymin_new = ScopeDisplay_InfoPanelHeight();
        uint16_t ymax_new = ScopeDisplay_InfoPanelHeight();

        if (ScopeDisplay_ClipWaveformSegment(&y0, &y1))
        {
            int32_t ymin_clip = (y0 < y1) ? y0 : y1;
            int32_t ymax_clip = (y0 > y1) ? y0 : y1;
            if (ymax_clip >= (int32_t)ILI9341_HEIGHT)
            {
                ymax_clip = ILI9341_HEIGHT - 1;
            }
            if (ymin_clip < (int32_t)ScopeDisplay_InfoPanelHeight())
            {
                ymin_clip = ScopeDisplay_InfoPanelHeight();
            }
            ScopeDisplay_DrawColumn(x, (uint16_t)ymin_clip, (uint16_t)ymax_clip);
            ymin_new = (uint16_t)ymin_clip;
            ymax_new = (uint16_t)ymax_clip;
        }

        last_y_min[x] = ymin_new;
        last_y_max[x] = ymax_new;
    }

    first_draw = 0U;
    ScopeDisplay_UpdateMeasurements(frame_min, frame_max, freq_hz);
}

static int32_t ScopeDisplay_SampleToY(const ScopeDisplaySettings *settings, int32_t sample)
{
    const int32_t info_panel = (int32_t)ScopeDisplay_InfoPanelHeight();
    const int32_t waveform_height = (int32_t)ScopeDisplay_WaveformHeight();
    int32_t span = (int32_t)settings->vertical.span_counts;
    if (span <= 0)
    {
        span = (int32_t)scope_display_module.cfg.adc_max_counts;
    }
    int32_t half_span = span / 2;
    int32_t lower = settings->vertical.center_counts - half_span;
    int32_t relative = sample - lower;
    int32_t y = info_panel + (waveform_height - 1)
                - (relative * (waveform_height - 1)) / span;
    return y;
}

static uint8_t ScopeDisplay_ClipWaveformSegment(int32_t *y0, int32_t *y1)
{
    if (y0 == NULL || y1 == NULL)
    {
        return 0U;
    }
    int32_t top = (int32_t)ScopeDisplay_InfoPanelHeight();
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

static uint16_t ScopeDisplay_BackgroundColor(uint16_t x, uint16_t y)
{
    const uint16_t info_panel = ScopeDisplay_InfoPanelHeight();
    if (y < info_panel)
    {
        return ILI9341_BLACK;
    }

    uint16_t gy = y - info_panel;
    if ((x % scope_display_module.cfg.grid_spacing_px) == 0U ||
        (gy % scope_display_module.cfg.grid_spacing_px) == 0U)
    {
        return ILI9341_BLUE;
    }
    return ILI9341_BLACK;
}

static void ScopeDisplay_EraseColumn(uint16_t x, uint16_t y0, uint16_t y1)
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
    uint16_t span = y1 - y0 + 1U;
    if (span == 0U)
    {
        return;
    }
    for (uint16_t i = 0; i < span; i++)
    {
        scope_column_buf[i] = ScopeDisplay_BackgroundColor(x, (uint16_t)(y0 + i));
    }
    ILI9341_DrawPixels(x, y0, scope_column_buf, span);
}

static void ScopeDisplay_DrawColumn(uint16_t x, uint16_t y0, uint16_t y1)
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
    uint16_t span = y1 - y0 + 1U;
    if (span == 0U)
    {
        return;
    }

    ILI9341_DrawColorSpan(x, y0, span, scope_display_module.cfg.waveform_color);
}

void ScopeDisplay_DrawMeasurements(uint16_t vmin, uint16_t vmax, uint32_t freq_hz)
{
    if (!scope_display_module.initialized)
    {
        return;
    }

    ScopeDisplay_UpdateMeasurements(vmin, vmax, freq_hz);
}

static void ScopeDisplay_UpdateMeasurements(uint16_t vmin, uint16_t vmax, uint32_t freq_hz)
{
    static char last_line1[32];
    static char last_line2[32];
    static char last_line3[32];
    char line1[32];
    char line2[32];
    char line3[32];
    uint32_t vmin_mv = ScopeSignal_AdcToMillivolt(vmin,
                                                  scope_display_module.cfg.adc_max_counts,
                                                  scope_display_module.cfg.adc_ref_millivolt);
    uint32_t vmax_mv = ScopeSignal_AdcToMillivolt(vmax,
                                                  scope_display_module.cfg.adc_max_counts,
                                                  scope_display_module.cfg.adc_ref_millivolt);
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

    ScopeDisplay_UpdateInfoLine(4U, 4U, line1, ILI9341_YELLOW, last_line1, sizeof(last_line1));
    ScopeDisplay_UpdateInfoLine(4U, 24U, line2, ILI9341_GREEN, last_line2, sizeof(last_line2));
    ScopeDisplay_UpdateInfoLine(4U, 44U, line3, ILI9341_WHITE, last_line3, sizeof(last_line3));
}

static void ScopeDisplay_UpdateInfoLine(uint16_t x, uint16_t y, const char *text,
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
