#include "scope.h"

#include "main.h"
#include "scope_display.h"
#include "scope_signal.h"

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
    volatile uint8_t autoset_request;
} ScopeControlFlags;

static ScopeDisplaySettings scope_display_settings;
static ScopeControlFlags scope_control = {0};
static void Scope_DisplaySettingsInit(void);
static void Scope_ResetVerticalWindow(void);
static void Scope_UpdateVerticalWindow(uint32_t span, int32_t center);
static void Scope_UpdateHorizontalWindow(uint32_t span_samples);
static uint16_t Scope_GetVisibleSampleCount(uint16_t available_samples);
static void Scope_ApplyAutoSet(uint16_t *buf, uint16_t len);
static uint8_t Scope_ConsumeAutoSetRequest(void);

uint16_t Scope_FrameSampleCount(void)
{
    return scope_cfg.samples_per_frame;
}

void Scope_Init(void)
{
    ILI9341_Init();
    ScopeDisplayConfig display_cfg = {
        .frame_samples = scope_cfg.samples_per_frame,
        .info_panel_height = scope_cfg.info_panel_height,
        .grid_spacing_px = scope_cfg.grid_spacing_px,
        .waveform_color = scope_cfg.waveform_color,
        .adc_max_counts = scope_cfg.adc_max_counts,
        .adc_ref_millivolt = scope_cfg.adc_ref_millivolt
    };
    ScopeDisplay_Init(&display_cfg);
    Scope_DisplaySettingsInit();
    ScopeDisplay_DrawGrid();
    ScopeDisplay_DrawMeasurements(0U, 0U, 0U);
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

    uint16_t visible_samples = Scope_GetVisibleSampleCount(count);
    ScopeDisplay_DrawWaveform(&scope_display_settings,
                              samples,
                              count,
                              visible_samples,
                              scope_cfg.trigger_min_delta);
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
    Scope_UpdateHorizontalWindow(scope_cfg.samples_per_frame);
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

static void Scope_UpdateHorizontalWindow(uint32_t span_samples)
{
    uint32_t max_samples = scope_cfg.samples_per_frame;
    if (max_samples == 0U)
    {
        scope_display_settings.horizontal.samples_visible = 0U;
        scope_display_settings.horizontal.center_sample = 0;
        return;
    }

    if (span_samples == 0U)
    {
        span_samples = max_samples;
    }

    if (span_samples < 2U)
    {
        span_samples = 2U;
    }

    if (span_samples > max_samples)
    {
        span_samples = max_samples;
    }

    scope_display_settings.horizontal.samples_visible = (uint16_t)span_samples;
    scope_display_settings.horizontal.center_sample =
        (int32_t)(scope_display_settings.horizontal.samples_visible / 2U);
}

static uint16_t Scope_GetVisibleSampleCount(uint16_t available_samples)
{
    if (available_samples == 0U)
    {
        return 0U;
    }

    uint16_t visible = scope_display_settings.horizontal.samples_visible;
    if (visible == 0U || visible > available_samples)
    {
        visible = available_samples;
    }

    if (visible == 0U)
    {
        visible = 1U;
    }

    return visible;
}

static void Scope_ApplyAutoSet(uint16_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0U)
    {
        return;
    }

    uint16_t frame_min = 0xFFFFU;
    uint16_t frame_max = 0U;
    uint16_t trig = ScopeSignal_FindTriggerIndex(buf,
                                                 len,
                                                 scope_cfg.trigger_min_delta,
                                                 &frame_min,
                                                 &frame_max);

    if (frame_max < frame_min)
    {
        Scope_ResetVerticalWindow();
        Scope_UpdateHorizontalWindow(scope_cfg.samples_per_frame);
        return;
    }

    uint32_t span = (uint32_t)frame_max - (uint32_t)frame_min;
    if (span < scope_cfg.trigger_min_delta)
    {
        Scope_ResetVerticalWindow();
        Scope_UpdateHorizontalWindow(scope_cfg.samples_per_frame);
        return;
    }

    uint32_t margin_span = span + span / 5U;
    if (margin_span == 0U)
    {
        margin_span = scope_cfg.trigger_min_delta;
    }

    uint32_t center = (uint32_t)frame_min + span / 2U;
    Scope_UpdateVerticalWindow(margin_span, (int32_t)center);

    uint32_t period_samples = ScopeSignal_EstimatePeriodSamples(buf,
                                                                len,
                                                                trig,
                                                                frame_min,
                                                                frame_max,
                                                                scope_cfg.trigger_min_delta);
    if (period_samples == 0U)
    {
        Scope_UpdateHorizontalWindow(scope_cfg.samples_per_frame);
    }
    else
    {
        uint32_t span_samples = period_samples * 2U;
        if (span_samples == 0U)
        {
            span_samples = scope_cfg.samples_per_frame;
        }
        Scope_UpdateHorizontalWindow(span_samples);
    }
}
