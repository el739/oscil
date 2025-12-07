#ifndef INC_SCOPE_H_
#define INC_SCOPE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "ili9341.h"
#include <stdint.h>

enum { SCOPE_FRAME_SAMPLES = ILI9341_WIDTH };

void Scope_Init(void);
void Scope_ProcessFrame(uint16_t *samples, uint16_t count);
void Scope_RequestAutoSet(void);
uint16_t Scope_FrameSampleCount(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_SCOPE_H_ */
