#ifndef INC_SCOPE_DISPLAY_H_
#define INC_SCOPE_DISPLAY_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

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
    uint16_t frame_samples;
    uint16_t info_panel_height;
    uint16_t grid_spacing_px;
    uint16_t waveform_color;
    uint16_t adc_max_counts;
    uint16_t adc_ref_millivolt;
} ScopeDisplayConfig;

void ScopeDisplay_Init(const ScopeDisplayConfig *cfg);
void ScopeDisplay_DrawGrid(void);

typedef struct
{
    uint8_t count;
    uint16_t columns[2];
    uint8_t selected_index;
} ScopeDisplayCursorRenderInfo;

typedef struct
{
    uint32_t sample_rate_hz;
    uint16_t sample_indices[2];
    uint16_t sample_values[2];
    uint8_t count;
} ScopeDisplayCursorMeasurements;

void ScopeDisplay_DrawWaveform(const ScopeDisplaySettings *settings,
                               uint16_t *samples,
                               uint16_t count,
                               uint16_t visible_samples,
                               uint16_t trigger_index,
                               const ScopeDisplayCursorRenderInfo *cursor_info,
                               uint16_t *column_sample_map);
void ScopeDisplay_DrawMeasurements(uint16_t vmin,
                                   uint16_t vmax,
                                   uint32_t freq_hz);
void ScopeDisplay_DrawCursorMeasurements(const ScopeDisplayCursorMeasurements *measurements);

#ifdef __cplusplus
}
#endif

#endif /* INC_SCOPE_DISPLAY_H_ */
