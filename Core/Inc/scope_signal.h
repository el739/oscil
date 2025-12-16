#ifndef INC_SCOPE_SIGNAL_H_
#define INC_SCOPE_SIGNAL_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

uint16_t ScopeSignal_FindTriggerIndex(uint16_t *buf,
                                      uint16_t len,
                                      uint16_t trigger_min_delta,
                                      uint16_t *out_min,
                                      uint16_t *out_max);

uint32_t ScopeSignal_EstimatePeriodSamples(uint16_t *buf,
                                           uint16_t len,
                                           uint16_t trig_idx,
                                           uint16_t frame_min,
                                           uint16_t frame_max,
                                           uint16_t trigger_min_delta);

uint32_t ScopeSignal_GetSampleRateHz(void);
uint32_t ScopeSignal_AdcToMillivolt(uint16_t sample,
                                    uint16_t adc_max_counts,
                                    uint16_t adc_ref_millivolt);

#ifdef __cplusplus
}
#endif

#endif /* INC_SCOPE_SIGNAL_H_ */
