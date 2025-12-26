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

typedef enum
{
    SCOPE_DISPLAY_INFO_MODE_NONE = 0,
    SCOPE_DISPLAY_INFO_MODE_MEASUREMENTS,
    SCOPE_DISPLAY_INFO_MODE_CURSOR
} ScopeDisplayInfoMode;

static ScopeDisplayInfoMode scope_display_info_mode = SCOPE_DISPLAY_INFO_MODE_NONE;
static char measurement_last_line1[32];
static char measurement_last_line2[32];
static char measurement_last_line3[32];
static char cursor_last_line1[32];
static char cursor_last_line2[32];
static char cursor_last_line3[32];

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
static void ScopeDisplay_DrawCursorLine(uint16_t x, uint16_t color);
static void ScopeDisplay_UpdateMeasurements(uint16_t vmin, uint16_t vmax, uint32_t freq_hz);
static void ScopeDisplay_UpdateInfoLine(uint16_t x, uint16_t y, const char *text,
                                        uint16_t color, char *last_text, size_t buf_len);
static int64_t ScopeDisplay_SamplesToTimeNs(int32_t sample_index, uint32_t sample_rate_hz);
static void ScopeDisplay_FormatTimeValue(char *buf, size_t len, int64_t time_ns);
static void ScopeDisplay_FormatVoltageString(char *buf, size_t len, int32_t millivolt, uint8_t force_sign);
static void ScopeDisplay_ClearInfoPanel(void);
static void ScopeDisplay_ClearMeasurementInfoCache(void);
static void ScopeDisplay_ClearCursorInfoCache(void);

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
    scope_display_info_mode = SCOPE_DISPLAY_INFO_MODE_NONE;
    ScopeDisplay_ClearMeasurementInfoCache();
    ScopeDisplay_ClearCursorInfoCache();
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
                               uint16_t trigger_index,
                               const ScopeDisplayCursorRenderInfo *cursor_info,
                               uint16_t *column_sample_map)
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
    if (trigger_index >= count)
    {
        trigger_index = 0U;
    }

    int32_t center_sample = settings->horizontal.center_sample;
    int32_t start_offset = (int32_t)trigger_index - center_sample;
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
        if (column_sample_map != NULL)
        {
            column_sample_map[i] = idx;
        }
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

    if (cursor_info != NULL && cursor_info->count > 0U)
    {
        for (uint8_t cursor_idx = 0U; cursor_idx < cursor_info->count; cursor_idx++)
        {
            uint16_t column = cursor_info->columns[cursor_idx];
            if (column >= draw_width)
            {
                continue;
            }
            uint16_t color = (cursor_idx == cursor_info->selected_index) ? ILI9341_CYAN
                                                                        : ILI9341_MAGENTA;
            ScopeDisplay_DrawCursorLine(column, color);
            uint16_t top = ScopeDisplay_InfoPanelHeight();
            uint16_t bottom = ILI9341_HEIGHT - 1U;
            if (column < ILI9341_WIDTH)
            {
                last_y_min[column] = top;
                last_y_max[column] = bottom;
            }
        }
    }

    first_draw = 0U;
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

static void ScopeDisplay_DrawCursorLine(uint16_t x, uint16_t color)
{
    if (x >= ILI9341_WIDTH)
    {
        return;
    }
    uint16_t y0 = ScopeDisplay_InfoPanelHeight();
    if (y0 >= ILI9341_HEIGHT)
    {
        return;
    }
    uint16_t span = ILI9341_HEIGHT - y0;
    if (span == 0U)
    {
        return;
    }
    ILI9341_DrawColorSpan(x, y0, span, color);
}

void ScopeDisplay_DrawMeasurements(uint16_t vmin, uint16_t vmax, uint32_t freq_hz)
{
    if (!scope_display_module.initialized)
    {
        return;
    }

    if (scope_display_info_mode != SCOPE_DISPLAY_INFO_MODE_MEASUREMENTS)
    {
        ScopeDisplay_ClearInfoPanel();
        ScopeDisplay_ClearMeasurementInfoCache();
        scope_display_info_mode = SCOPE_DISPLAY_INFO_MODE_MEASUREMENTS;
    }

    ScopeDisplay_UpdateMeasurements(vmin, vmax, freq_hz);
}

static void ScopeDisplay_UpdateMeasurements(uint16_t vmin, uint16_t vmax, uint32_t freq_hz)
{
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

    ScopeScaleTarget target = Scope_GetScaleTarget();
    const char *target_str = (target == SCOPE_SCALE_TARGET_VOLTAGE) ? "V" : "T";
    size_t len = strlen(line3);
    if (len < sizeof(line3))
    {
        snprintf(&line3[len], sizeof(line3) - len, " | Adj: %s", target_str);
    }

    ScopeDisplay_UpdateInfoLine(4U,
                                4U,
                                line1,
                                ILI9341_YELLOW,
                                measurement_last_line1,
                                sizeof(measurement_last_line1));
    ScopeDisplay_UpdateInfoLine(4U,
                                24U,
                                line2,
                                ILI9341_GREEN,
                                measurement_last_line2,
                                sizeof(measurement_last_line2));
    ScopeDisplay_UpdateInfoLine(4U,
                                44U,
                                line3,
                                ILI9341_WHITE,
                                measurement_last_line3,
                                sizeof(measurement_last_line3));
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

static int64_t ScopeDisplay_SamplesToTimeNs(int32_t sample_index, uint32_t sample_rate_hz)
{
    if (sample_rate_hz == 0U)
    {
        return 0;
    }
    return ((int64_t)sample_index * 1000000000LL) / (int32_t)sample_rate_hz;
}

static void ScopeDisplay_FormatTimeValue(char *buf, size_t len, int64_t time_ns)
{
    if (buf == NULL || len == 0U)
    {
        return;
    }
    uint8_t negative = 0U;
    if (time_ns < 0)
    {
        negative = 1U;
        time_ns = -time_ns;
    }

    const uint64_t ONE_SECOND = 1000000000ULL;
    const uint64_t ONE_MILLISECOND = 1000000ULL;
    const uint64_t ONE_MICROSECOND = 1000ULL;

    if ((uint64_t)time_ns >= ONE_SECOND)
    {
        uint64_t whole = (uint64_t)time_ns / ONE_SECOND;
        uint64_t frac = ((uint64_t)time_ns % ONE_SECOND) / 1000000ULL;
        snprintf(buf,
                 len,
                 "%s%lu.%03lu s",
                 negative ? "-" : "",
                 (unsigned long)whole,
                 (unsigned long)frac);
    }
    else if ((uint64_t)time_ns >= ONE_MILLISECOND)
    {
        uint64_t whole = (uint64_t)time_ns / ONE_MILLISECOND;
        uint64_t frac = ((uint64_t)time_ns % ONE_MILLISECOND) / 1000ULL;
        snprintf(buf,
                 len,
                 "%s%lu.%03lu ms",
                 negative ? "-" : "",
                 (unsigned long)whole,
                 (unsigned long)frac);
    }
    else if ((uint64_t)time_ns >= ONE_MICROSECOND)
    {
        uint64_t whole = (uint64_t)time_ns / ONE_MICROSECOND;
        uint64_t frac = ((uint64_t)time_ns % ONE_MICROSECOND);
        snprintf(buf,
                 len,
                 "%s%lu.%03lu us",
                 negative ? "-" : "",
                 (unsigned long)whole,
                 (unsigned long)frac);
    }
    else
    {
        snprintf(buf,
                 len,
                 "%s%llu ns",
                 negative ? "-" : "",
                 (unsigned long long)time_ns);
    }
}

static void ScopeDisplay_FormatVoltageString(char *buf, size_t len, int32_t millivolt, uint8_t force_sign)
{
    if (buf == NULL || len == 0U)
    {
        return;
    }

    uint8_t negative = 0U;
    if (millivolt < 0)
    {
        negative = 1U;
        millivolt = -millivolt;
    }

    uint32_t whole = (uint32_t)millivolt / 1000U;
    uint32_t frac = ((uint32_t)millivolt % 1000U) / 10U;

    if (negative)
    {
        snprintf(buf,
                 len,
                 "-%lu.%02lu V",
                 (unsigned long)whole,
                 (unsigned long)frac);
    }
    else if (force_sign)
    {
        snprintf(buf,
                 len,
                 "+%lu.%02lu V",
                 (unsigned long)whole,
                 (unsigned long)frac);
    }
    else
    {
        snprintf(buf,
                 len,
                 "%lu.%02lu V",
                 (unsigned long)whole,
                 (unsigned long)frac);
    }
}

void ScopeDisplay_DrawCursorMeasurements(const ScopeDisplayCursorMeasurements *measurements)
{
    if (!scope_display_module.initialized || measurements == NULL || measurements->count == 0U)
    {
        return;
    }

    if (scope_display_info_mode != SCOPE_DISPLAY_INFO_MODE_CURSOR)
    {
        ScopeDisplay_ClearInfoPanel();
        ScopeDisplay_ClearCursorInfoCache();
        scope_display_info_mode = SCOPE_DISPLAY_INFO_MODE_CURSOR;
    }

    char line1[32];
    char line2[32];
    char line3[32];
    char t_buf[2][16];
    char v_buf[2][16];
    char dt_buf[16];
    char dv_buf[16];

    for (uint8_t idx = 0U; idx < 2U; idx++)
    {
        snprintf(t_buf[idx], sizeof(t_buf[idx]), "---");
        snprintf(v_buf[idx], sizeof(v_buf[idx]), "---");
    }
    snprintf(dt_buf, sizeof(dt_buf), "---");
    snprintf(dv_buf, sizeof(dv_buf), "---");

    for (uint8_t idx = 0U; idx < measurements->count && idx < 2U; idx++)
    {
        uint32_t mv = ScopeSignal_AdcToMillivolt(measurements->sample_values[idx],
                                                 scope_display_module.cfg.adc_max_counts,
                                                 scope_display_module.cfg.adc_ref_millivolt);
        ScopeDisplay_FormatVoltageString(v_buf[idx], sizeof(v_buf[idx]), (int32_t)mv, 0U);

        int32_t sample_idx = (int32_t)measurements->sample_indices[idx];
        int64_t t_ns = ScopeDisplay_SamplesToTimeNs(sample_idx,
                                                    measurements->sample_rate_hz);
        ScopeDisplay_FormatTimeValue(t_buf[idx], sizeof(t_buf[idx]), t_ns);
    }

    if (measurements->count >= 2U)
    {
        int32_t sample_delta = (int32_t)measurements->sample_indices[0] -
                               (int32_t)measurements->sample_indices[1];
        int64_t delta_ns = ScopeDisplay_SamplesToTimeNs(sample_delta,
                                                        measurements->sample_rate_hz);
        ScopeDisplay_FormatTimeValue(dt_buf, sizeof(dt_buf), delta_ns);

        uint32_t mv1 = ScopeSignal_AdcToMillivolt(measurements->sample_values[0],
                                                  scope_display_module.cfg.adc_max_counts,
                                                  scope_display_module.cfg.adc_ref_millivolt);
        uint32_t mv2 = ScopeSignal_AdcToMillivolt(measurements->sample_values[1],
                                                  scope_display_module.cfg.adc_max_counts,
                                                  scope_display_module.cfg.adc_ref_millivolt);
        int32_t delta_mv = (int32_t)mv1 - (int32_t)mv2;
        ScopeDisplay_FormatVoltageString(dv_buf, sizeof(dv_buf), delta_mv, 1U);
    }

    snprintf(line1, sizeof(line1), "T1:%s | V1:%s", t_buf[0], v_buf[0]);
    snprintf(line2, sizeof(line2), "T2:%s | V2:%s", t_buf[1], v_buf[1]);

    if (measurements->count >= 2U)
    {
        snprintf(line3, sizeof(line3), "DT:%s | DV:%s", dt_buf, dv_buf);
    }
    else
    {
        snprintf(line3, sizeof(line3), "DT: --- | DV: ---");
    }

    ScopeDisplay_UpdateInfoLine(4U,
                                4U,
                                line1,
                                ILI9341_WHITE,
                                cursor_last_line1,
                                sizeof(cursor_last_line1));
    ScopeDisplay_UpdateInfoLine(4U,
                                24U,
                                line2,
                                ILI9341_WHITE,
                                cursor_last_line2,
                                sizeof(cursor_last_line2));
    ScopeDisplay_UpdateInfoLine(4U,
                                44U,
                                line3,
                                ILI9341_WHITE,
                                cursor_last_line3,
                                sizeof(cursor_last_line3));
}

static void ScopeDisplay_ClearInfoPanel(void)
{
    uint16_t panel_height = ScopeDisplay_InfoPanelHeight();
    if (panel_height == 0U)
    {
        return;
    }
    ILI9341_FillRect(0U, 0U, ILI9341_WIDTH, panel_height, ILI9341_BLACK);
}

static void ScopeDisplay_ClearMeasurementInfoCache(void)
{
    measurement_last_line1[0] = '\0';
    measurement_last_line2[0] = '\0';
    measurement_last_line3[0] = '\0';
}

static void ScopeDisplay_ClearCursorInfoCache(void)
{
    cursor_last_line1[0] = '\0';
    cursor_last_line2[0] = '\0';
    cursor_last_line3[0] = '\0';
}
