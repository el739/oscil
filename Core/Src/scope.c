#include "scope.h"

#include "main.h"
#include "scope_display.h"
#include "scope_signal.h"

enum
{
    VERTICAL_SCALE_MAX_FACTOR = 8U,
    OFFSET_STEP_DIVISOR = 10U,
    AUTOSET_MARGIN_PERCENT_NUMERATOR = 1U,
    AUTOSET_MARGIN_PERCENT_DENOMINATOR = 5U
};

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
    volatile uint8_t zoom_out_requests;
    volatile uint8_t zoom_in_requests;
    volatile uint8_t v_zoom_out_requests;
    volatile uint8_t v_zoom_in_requests;
    volatile int8_t offset_shift_requests;
} ScopeControlFlags;

static ScopeDisplaySettings scope_display_settings;
static ScopeControlFlags scope_control = {0};
static ScopeScaleTarget scope_scale_target = SCOPE_SCALE_TARGET_VOLTAGE;
static void Scope_DisplaySettingsInit(void);
static void Scope_ResetVerticalWindow(void);
static void Scope_UpdateVerticalWindow(uint32_t span, int32_t center);
static void Scope_UpdateHorizontalWindow(uint32_t span_samples);
static uint32_t Scope_MaxVerticalSpan(void);
static uint16_t Scope_GetVisibleSampleCount(uint16_t available_samples);
static void Scope_ApplyAutoSet(uint16_t *buf, uint16_t len);
static uint8_t Scope_ConsumeAutoSetRequest(void);
static void Scope_ApplyHorizontalScaleRequests(void);
static void Scope_ApplyVerticalScaleRequests(void);
static void Scope_ApplyOffsetRequests(void);
static void Scope_ZoomHorizontal(uint8_t zoom_in);
static void Scope_ZoomVertical(uint8_t zoom_in);
static void Scope_QueueZoomRequest(volatile uint8_t *request_counter);
static void Scope_RequestOffsetStep(int8_t direction);
static void Scope_ShiftHorizontalOffset(int8_t steps);
static void Scope_ShiftVerticalOffset(int8_t steps);

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

    Scope_ApplyHorizontalScaleRequests();
    Scope_ApplyVerticalScaleRequests();
    Scope_ApplyOffsetRequests();

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

void Scope_RequestMoreCycles(void)
{
    Scope_QueueZoomRequest(&scope_control.zoom_out_requests);
}

void Scope_RequestFewerCycles(void)
{
    Scope_QueueZoomRequest(&scope_control.zoom_in_requests);
}

void Scope_RequestMoreVoltageScale(void)
{
    Scope_QueueZoomRequest(&scope_control.v_zoom_out_requests);
}

void Scope_RequestLessVoltageScale(void)
{
    Scope_QueueZoomRequest(&scope_control.v_zoom_in_requests);
}

void Scope_RequestOffsetDecrease(void)
{
    Scope_RequestOffsetStep(-1);
}

void Scope_RequestOffsetIncrease(void)
{
    Scope_RequestOffsetStep(1);
}

void Scope_ToggleScaleTarget(void)
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

ScopeScaleTarget Scope_GetScaleTarget(void)
{
    return scope_scale_target;
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
    if (center < 0)
    {
        center = 0;
    }
    uint32_t max_span = Scope_MaxVerticalSpan();
    if (span > max_span)
    {
        span = max_span;
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

static uint32_t Scope_MaxVerticalSpan(void)
{
    uint32_t base_span = scope_cfg.adc_max_counts;
    if (base_span == 0U)
    {
        base_span = 1U;
    }
    uint64_t scaled = (uint64_t)base_span * VERTICAL_SCALE_MAX_FACTOR;
    if (scaled == 0U)
    {
        scaled = 1U;
    }
    if (scaled > 0xFFFFFFFFULL)
    {
        scaled = 0xFFFFFFFFULL;
    }
    return (uint32_t)scaled;
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

static void Scope_ConsumeAndApplyZoomRequests(volatile uint8_t *zoom_out_requests,
                                               volatile uint8_t *zoom_in_requests,
                                               void (*zoom_func)(uint8_t zoom_in))
{
    uint8_t zoom_out = 0U;
    uint8_t zoom_in = 0U;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    zoom_out = *zoom_out_requests;
    zoom_in = *zoom_in_requests;
    *zoom_out_requests = 0U;
    *zoom_in_requests = 0U;
    if (primask == 0U)
    {
        __enable_irq();
    }

    while (zoom_out != 0U)
    {
        zoom_func(0U);
        zoom_out--;
    }

    while (zoom_in != 0U)
    {
        zoom_func(1U);
        zoom_in--;
    }
}

static void Scope_ApplyHorizontalScaleRequests(void)
{
    Scope_ConsumeAndApplyZoomRequests(&scope_control.zoom_out_requests,
                                      &scope_control.zoom_in_requests,
                                      Scope_ZoomHorizontal);
}

static void Scope_ApplyVerticalScaleRequests(void)
{
    Scope_ConsumeAndApplyZoomRequests(&scope_control.v_zoom_out_requests,
                                      &scope_control.v_zoom_in_requests,
                                      Scope_ZoomVertical);
}

static void Scope_ApplyOffsetRequests(void)
{
    int8_t pending = 0;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    pending = scope_control.offset_shift_requests;
    scope_control.offset_shift_requests = 0;
    if (primask == 0U)
    {
        __enable_irq();
    }

    if (pending == 0)
    {
        return;
    }

    Scope_ShiftHorizontalOffset(pending);
    Scope_ShiftVerticalOffset(pending);
}

static void Scope_ZoomHorizontal(uint8_t zoom_in)
{
    uint32_t span = scope_display_settings.horizontal.samples_visible;
    if (span == 0U)
    {
        span = scope_cfg.samples_per_frame;
    }

    if (zoom_in)
    {
        if (span > 2U)
        {
            span /= 2U;
        }
        else
        {
            span = 2U;
        }
    }
    else
    {
        if (span < scope_cfg.samples_per_frame)
        {
            span *= 2U;
            if (span > scope_cfg.samples_per_frame)
            {
                span = scope_cfg.samples_per_frame;
            }
        }
        else
        {
            span = scope_cfg.samples_per_frame;
        }
    }

    Scope_UpdateHorizontalWindow(span);
}

static void Scope_ZoomVertical(uint8_t zoom_in)
{
    uint32_t span = scope_display_settings.vertical.span_counts;
    uint32_t max_span = Scope_MaxVerticalSpan();
    if (span == 0U)
    {
        span = scope_cfg.adc_max_counts;
    }

    if (zoom_in)
    {
        if (span > scope_cfg.trigger_min_delta)
        {
            span /= 2U;
            if (span < scope_cfg.trigger_min_delta)
            {
                span = scope_cfg.trigger_min_delta;
            }
        }
        else
        {
            span = scope_cfg.trigger_min_delta;
        }
    }
    else
    {
        if (span < max_span)
        {
            span *= 2U;
            if (span > max_span)
            {
                span = max_span;
            }
        }
        else
        {
            span = max_span;
        }
    }

    Scope_UpdateVerticalWindow(span, scope_display_settings.vertical.center_counts);
}

static void Scope_QueueZoomRequest(volatile uint8_t *request_counter)
{
    if (request_counter == NULL)
    {
        return;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    if (*request_counter < 0xFFU)
    {
        (*request_counter)++;
    }
    if (primask == 0U)
    {
        __enable_irq();
    }
}

static void Scope_RequestOffsetStep(int8_t direction)
{
    if (direction == 0)
    {
        return;
    }

    const int8_t max_pending = 64;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    int16_t pending = (int16_t)scope_control.offset_shift_requests + (int16_t)direction;
    if (pending > max_pending)
    {
        pending = max_pending;
    }
    else if (pending < -max_pending)
    {
        pending = -max_pending;
    }
    scope_control.offset_shift_requests = (int8_t)pending;
    if (primask == 0U)
    {
        __enable_irq();
    }
}

static void Scope_ShiftHorizontalOffset(int8_t steps)
{
    if (steps == 0)
    {
        return;
    }

    uint32_t visible = scope_display_settings.horizontal.samples_visible;
    uint32_t frame_samples = scope_cfg.samples_per_frame;
    if (visible == 0U || frame_samples == 0U)
    {
        return;
    }

    int32_t delta = (int32_t)visible / (int32_t)OFFSET_STEP_DIVISOR;
    if (delta == 0)
    {
        delta = 1;
    }

    int32_t center = scope_display_settings.horizontal.center_sample;
    center += (int32_t)steps * delta;

    int32_t limit = (int32_t)frame_samples * 2;
    if (limit <= 0)
    {
        limit = (int32_t)frame_samples;
    }
    if (center > limit)
    {
        center = limit;
    }
    else if (center < -limit)
    {
        center = -limit;
    }

    scope_display_settings.horizontal.center_sample = center;
}

static void Scope_ShiftVerticalOffset(int8_t steps)
{
    if (steps == 0)
    {
        return;
    }

    uint32_t span = scope_display_settings.vertical.span_counts;
    if (span == 0U)
    {
        span = scope_cfg.adc_max_counts;
    }

    int32_t delta = (int32_t)span / (int32_t)OFFSET_STEP_DIVISOR;
    if (delta == 0)
    {
        delta = 1;
    }

    int32_t center = scope_display_settings.vertical.center_counts;
    center -= (int32_t)steps * delta;

    if (center < 0)
    {
        center = 0;
    }
    else if (center > (int32_t)scope_cfg.adc_max_counts)
    {
        center = (int32_t)scope_cfg.adc_max_counts;
    }

    scope_display_settings.vertical.center_counts = center;
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

    uint32_t margin_span = span + (span * AUTOSET_MARGIN_PERCENT_NUMERATOR / AUTOSET_MARGIN_PERCENT_DENOMINATOR);
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
