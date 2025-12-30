/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    uart_command.h
  * @brief   This file contains all the function prototypes for
  *          the uart_command.c file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef INC_UART_COMMAND_H_
#define INC_UART_COMMAND_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void UartCommand_Init(void);
uint8_t UartCommand_HasPending(void);
void UartCommand_Process(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_UART_COMMAND_H_ */
