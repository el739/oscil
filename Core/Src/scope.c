#include "scope.h"

#include "main.h"
#include "scope_display.h"
#include "scope_signal.h"

#include <string.h>

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
    volatile int8_t horizontal_offset_shift_requests;
    volatile int8_t vertical_offset_shift_requests;
    volatile uint8_t hold_toggle_request;
} ScopeControlFlags;

enum { SCOPE_CURSOR_COUNT = 2U };

typedef struct
{
    uint16_t samples[SCOPE_FRAME_SAMPLES];
    uint16_t column_map[SCOPE_FRAME_SAMPLES];
    uint16_t sample_count;
    uint16_t trigger_index;
    uint16_t frame_min;
    uint16_t frame_max;
    uint32_t freq_hz;
    uint8_t valid;
} ScopeFrameSnapshot;

typedef struct
{
    uint16_t columns[SCOPE_CURSOR_COUNT];
    uint8_t selected;
    uint8_t active;
    volatile int8_t shift_requests;
    volatile uint8_t select_toggle_request;
} ScopeCursorState;

static ScopeDisplaySettings scope_display_settings;
static ScopeControlFlags scope_control = {0};
static ScopeScaleTarget scope_scale_target = SCOPE_SCALE_TARGET_VOLTAGE;
static volatile uint8_t scope_waveform_hold = 0U;
static ScopeFrameSnapshot scope_live_frame = {0};
static ScopeFrameSnapshot scope_hold_frame = {0};
static ScopeCursorState scope_cursor_state = {0};
static uint8_t scope_hold_render_pending = 0U;
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
static void Scope_QueueOffsetRequest(volatile int8_t *request_counter, int8_t direction);
static void Scope_ShiftHorizontalOffset(int8_t steps);
static void Scope_ShiftVerticalOffset(int8_t steps);
static void Scope_HandleHoldToggleRequest(void);
static void Scope_SetHoldState(uint8_t enable);
static uint8_t Scope_CopyLiveFrameToHold(void);
static void Scope_DisableHoldState(void);
static void Scope_InitCursorPositions(void);
static void Scope_ApplyCursorRequests(void);
static void Scope_MoveCursor(uint8_t cursor_index, int8_t steps);
static void Scope_RenderHoldFrame(void);
static void Scope_DrawCursorMeasurements(void);
static uint16_t Scope_GetCursorColumnLimit(void);

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
    Scope_HandleHoldToggleRequest();

    if (samples == NULL || count == 0U)
    {
        if (scope_waveform_hold)
        {
            Scope_ApplyCursorRequests();
            if (scope_hold_render_pending)
            {
                Scope_RenderHoldFrame();
                scope_hold_render_pending = 0U;
            }
        }
        return;
    }

    if (count > scope_cfg.samples_per_frame)
    {
        count = scope_cfg.samples_per_frame;
    }

    if (scope_waveform_hold)
    {
        Scope_ApplyCursorRequests();
        if (scope_hold_render_pending)
        {
            Scope_RenderHoldFrame();
            scope_hold_render_pending = 0U;
        }
        return;
    }

    if (Scope_ConsumeAutoSetRequest())
    {
        Scope_ApplyAutoSet(samples, count);
    }

    Scope_ApplyHorizontalScaleRequests();
    Scope_ApplyVerticalScaleRequests();
    Scope_ApplyOffsetRequests();

    uint16_t frame_min = 0U;
    uint16_t frame_max = 0U;
    uint16_t trig = ScopeSignal_FindTriggerIndex(samples,
                                                 count,
                                                 scope_cfg.trigger_min_delta,
                                                 &frame_min,
                                                 &frame_max);
    uint32_t period_samples = ScopeSignal_EstimatePeriodSamples(samples,
                                                                count,
                                                                trig,
                                                                frame_min,
                                                                frame_max,
                                                                scope_cfg.trigger_min_delta);
    uint32_t freq_hz = 0U;
    if (period_samples != 0U)
    {
        uint32_t sample_rate = ScopeSignal_GetSampleRateHz();
        if (sample_rate != 0U)
        {
            freq_hz = sample_rate / period_samples;
        }
    }

    uint16_t visible_samples = Scope_GetVisibleSampleCount(count);
    ScopeDisplay_DrawWaveform(&scope_display_settings,
                              samples,
                              count,
                              visible_samples,
                              trig,
                              NULL,
                              scope_live_frame.column_map);
    ScopeDisplay_DrawMeasurements(frame_min, frame_max, freq_hz);

    if (count != 0U)
    {
        memcpy(scope_live_frame.samples, samples, (size_t)count * sizeof(uint16_t));
    }
    scope_live_frame.sample_count = count;
    scope_live_frame.trigger_index = trig;
    scope_live_frame.frame_min = frame_min;
    scope_live_frame.frame_max = frame_max;
    scope_live_frame.freq_hz = freq_hz;
    scope_live_frame.valid = 1U;
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

void Scope_ToggleWaveformHold(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    scope_control.hold_toggle_request = 1U;
    if (primask == 0U)
    {
        __enable_irq();
    }
}

uint8_t Scope_IsWaveformHoldEnabled(void)
{
    return scope_waveform_hold;
}

void Scope_RequestCursorShift(int8_t direction)
{
    if (direction == 0 || !Scope_IsWaveformHoldEnabled())
    {
        return;
    }

    const int8_t max_pending = 64;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    int16_t pending = (int16_t)scope_cursor_state.shift_requests + (int16_t)direction;
    if (pending > max_pending)
    {
        pending = max_pending;
    }
    else if (pending < -max_pending)
    {
        pending = -max_pending;
    }
    scope_cursor_state.shift_requests = (int8_t)pending;
    if (primask == 0U)
    {
        __enable_irq();
    }
}

void Scope_RequestCursorSelectNext(void)
{
    if (!Scope_IsWaveformHoldEnabled())
    {
        return;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    scope_cursor_state.select_toggle_request = 1U;
    if (primask == 0U)
    {
        __enable_irq();
    }
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
    int8_t horizontal_pending = 0;
    int8_t vertical_pending = 0;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    horizontal_pending = scope_control.horizontal_offset_shift_requests;
    vertical_pending = scope_control.vertical_offset_shift_requests;
    scope_control.horizontal_offset_shift_requests = 0;
    scope_control.vertical_offset_shift_requests = 0;
    if (primask == 0U)
    {
        __enable_irq();
    }

    if (horizontal_pending != 0)
    {
        Scope_ShiftHorizontalOffset(horizontal_pending);
    }

    if (vertical_pending != 0)
    {
        Scope_ShiftVerticalOffset(vertical_pending);
    }
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

    volatile int8_t *queue = &scope_control.vertical_offset_shift_requests;
    if (scope_scale_target == SCOPE_SCALE_TARGET_TIME)
    {
        queue = &scope_control.horizontal_offset_shift_requests;
    }

    Scope_QueueOffsetRequest(queue, direction);
}

static void Scope_QueueOffsetRequest(volatile int8_t *request_counter, int8_t direction)
{
    if (request_counter == NULL || direction == 0)
    {
        return;
    }

    const int8_t max_pending = 64;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    int16_t pending = (int16_t)(*request_counter) + (int16_t)direction;
    if (pending > max_pending)
    {
        pending = max_pending;
    }
    else if (pending < -max_pending)
    {
        pending = -max_pending;
    }
    *request_counter = (int8_t)pending;
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

static void Scope_HandleHoldToggleRequest(void)
{
    uint8_t pending = 0U;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    pending = scope_control.hold_toggle_request;
    scope_control.hold_toggle_request = 0U;
    if (primask == 0U)
    {
        __enable_irq();
    }

    if (pending != 0U)
    {
        Scope_SetHoldState((uint8_t)(!scope_waveform_hold));
    }
}

static void Scope_SetHoldState(uint8_t enable)
{
    if (enable)
    {
        if (!Scope_CopyLiveFrameToHold())
        {
            scope_waveform_hold = 0U;
            return;
        }
        scope_waveform_hold = 1U;
        Scope_InitCursorPositions();
        scope_hold_render_pending = 1U;
    }
    else
    {
        Scope_DisableHoldState();
        scope_waveform_hold = 0U;
    }
}

static uint8_t Scope_CopyLiveFrameToHold(void)
{
    if (!scope_live_frame.valid || scope_live_frame.sample_count == 0U)
    {
        return 0U;
    }

    size_t sample_bytes = (size_t)scope_live_frame.sample_count * sizeof(uint16_t);
    memcpy(scope_hold_frame.samples, scope_live_frame.samples, sample_bytes);
    memcpy(scope_hold_frame.column_map,
           scope_live_frame.column_map,
           sizeof(scope_live_frame.column_map));
    scope_hold_frame.sample_count = scope_live_frame.sample_count;
    scope_hold_frame.trigger_index = scope_live_frame.trigger_index;
    scope_hold_frame.frame_min = scope_live_frame.frame_min;
    scope_hold_frame.frame_max = scope_live_frame.frame_max;
    scope_hold_frame.freq_hz = scope_live_frame.freq_hz;
    scope_hold_frame.valid = 1U;
    return 1U;
}

static void Scope_DisableHoldState(void)
{
    scope_hold_frame.valid = 0U;
    scope_cursor_state.active = 0U;
    scope_cursor_state.selected = 0U;
    scope_cursor_state.shift_requests = 0;
    scope_cursor_state.select_toggle_request = 0U;
    scope_hold_render_pending = 0U;
}

static void Scope_InitCursorPositions(void)
{
    uint16_t limit = Scope_GetCursorColumnLimit();
    if (limit == 0U)
    {
        limit = 1U;
    }

    scope_cursor_state.columns[0] = limit / 3U;
    scope_cursor_state.columns[1] = (uint16_t)((2U * limit) / 3U);

    if (scope_cursor_state.columns[0] >= limit)
    {
        scope_cursor_state.columns[0] = limit - 1U;
    }
    if (scope_cursor_state.columns[1] >= limit)
    {
        scope_cursor_state.columns[1] = limit - 1U;
    }

    scope_cursor_state.selected = 0U;
    scope_cursor_state.shift_requests = 0;
    scope_cursor_state.select_toggle_request = 0U;
    scope_cursor_state.active = 1U;
}

static void Scope_ApplyCursorRequests(void)
{
    int8_t shift = 0;
    uint8_t toggle = 0U;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    shift = scope_cursor_state.shift_requests;
    scope_cursor_state.shift_requests = 0;
    toggle = scope_cursor_state.select_toggle_request;
    scope_cursor_state.select_toggle_request = 0U;
    if (primask == 0U)
    {
        __enable_irq();
    }

    if (!scope_waveform_hold || !scope_hold_frame.valid || !scope_cursor_state.active)
    {
        return;
    }

    uint8_t updated = 0U;
    if (toggle != 0U)
    {
        scope_cursor_state.selected++;
        if (scope_cursor_state.selected >= SCOPE_CURSOR_COUNT)
        {
            scope_cursor_state.selected = 0U;
        }
        updated = 1U;
    }

    if (shift != 0)
    {
        Scope_MoveCursor(scope_cursor_state.selected, shift);
        updated = 1U;
    }

    if (updated)
    {
        scope_hold_render_pending = 1U;
    }
}

static void Scope_MoveCursor(uint8_t cursor_index, int8_t steps)
{
    if (cursor_index >= SCOPE_CURSOR_COUNT || steps == 0)
    {
        return;
    }

    int32_t column = scope_cursor_state.columns[cursor_index];
    int32_t limit = (int32_t)Scope_GetCursorColumnLimit();
    if (limit <= 0)
    {
        return;
    }

    column += (int32_t)steps;
    if (column < 0)
    {
        column = 0;
    }
    else if (column >= limit)
    {
        column = limit - 1;
    }

    scope_cursor_state.columns[cursor_index] = (uint16_t)column;
}

static void Scope_RenderHoldFrame(void)
{
    if (!scope_waveform_hold || !scope_hold_frame.valid)
    {
        return;
    }

    ScopeDisplayCursorRenderInfo cursor_info = {0};
    if (scope_cursor_state.active)
    {
        cursor_info.count = SCOPE_CURSOR_COUNT;
        cursor_info.selected_index = scope_cursor_state.selected;
        for (uint8_t idx = 0U; idx < SCOPE_CURSOR_COUNT; idx++)
        {
            uint16_t limit = Scope_GetCursorColumnLimit();
            uint16_t column = scope_cursor_state.columns[idx];
            if (limit != 0U && column >= limit)
            {
                column = limit - 1U;
            }
            cursor_info.columns[idx] = column;
        }
    }

    uint16_t visible_samples = Scope_GetVisibleSampleCount(scope_hold_frame.sample_count);
    ScopeDisplay_DrawWaveform(&scope_display_settings,
                              scope_hold_frame.samples,
                              scope_hold_frame.sample_count,
                              visible_samples,
                              scope_hold_frame.trigger_index,
                              (cursor_info.count > 0U) ? &cursor_info : NULL,
                              scope_hold_frame.column_map);

    if (scope_cursor_state.active)
    {
        Scope_DrawCursorMeasurements();
    }
    else
    {
        ScopeDisplay_DrawMeasurements(scope_hold_frame.frame_min,
                                      scope_hold_frame.frame_max,
                                      scope_hold_frame.freq_hz);
    }
}

static void Scope_DrawCursorMeasurements(void)
{
    if (!scope_hold_frame.valid || scope_hold_frame.sample_count == 0U)
    {
        return;
    }

    ScopeDisplayCursorMeasurements measurements = {0};
    measurements.count = SCOPE_CURSOR_COUNT;
    measurements.sample_rate_hz = ScopeSignal_GetSampleRateHz();
    uint16_t limit = Scope_GetCursorColumnLimit();
    if (limit == 0U)
    {
        return;
    }

    for (uint8_t idx = 0U; idx < SCOPE_CURSOR_COUNT; idx++)
    {
        uint16_t column = scope_cursor_state.columns[idx];
        if (column >= limit)
        {
            column = limit - 1U;
        }
        uint16_t sample_idx = scope_hold_frame.column_map[column];
        if (sample_idx >= scope_hold_frame.sample_count)
        {
            sample_idx = scope_hold_frame.sample_count - 1U;
        }
        measurements.sample_indices[idx] = sample_idx;
        measurements.sample_values[idx] = scope_hold_frame.samples[sample_idx];
    }

    ScopeDisplay_DrawCursorMeasurements(&measurements);
}

static uint16_t Scope_GetCursorColumnLimit(void)
{
    if (scope_cfg.samples_per_frame == 0U)
    {
        return 0U;
    }
    return scope_cfg.samples_per_frame;
}
