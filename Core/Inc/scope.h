#ifndef INC_SCOPE_H_
#define INC_SCOPE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "ili9341.h"
#include <stdint.h>

enum { SCOPE_FRAME_SAMPLES = ILI9341_WIDTH };

typedef enum
{
    SCOPE_SCALE_TARGET_VOLTAGE = 0,
    SCOPE_SCALE_TARGET_TIME
} ScopeScaleTarget;

void Scope_Init(void);
void Scope_ProcessFrame(uint16_t *samples, uint16_t count);
void Scope_RequestAutoSet(void);
void Scope_RequestMoreCycles(void);
void Scope_RequestFewerCycles(void);
void Scope_RequestMoreVoltageScale(void);
void Scope_RequestLessVoltageScale(void);
void Scope_RequestOffsetDecrease(void);
void Scope_RequestOffsetIncrease(void);
void Scope_ToggleScaleTarget(void);
ScopeScaleTarget Scope_GetScaleTarget(void);
uint16_t Scope_FrameSampleCount(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_SCOPE_H_ */
