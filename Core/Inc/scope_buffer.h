#ifndef INC_SCOPE_BUFFER_H_
#define INC_SCOPE_BUFFER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void ScopeBuffer_Init(void);
void ScopeBuffer_EnqueueFromISR(uint8_t buffer_index);
uint16_t *ScopeBuffer_Dequeue(uint16_t *frame_samples);
uint32_t ScopeBuffer_GetOverrunCount(void);
uint8_t ScopeBuffer_HasPending(void);
uint16_t *ScopeBuffer_GetDmaBaseAddress(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_SCOPE_BUFFER_H_ */
