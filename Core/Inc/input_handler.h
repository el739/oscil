#ifndef INC_INPUT_HANDLER_H_
#define INC_INPUT_HANDLER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void InputHandler_Init(void);
void InputHandler_ProcessGpioInterrupt(uint16_t gpio_pin);

#ifdef __cplusplus
}
#endif

#endif /* INC_INPUT_HANDLER_H_ */
