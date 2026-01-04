/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    waveform_control.h
  * @brief   This file contains all the function prototypes for
  *          the waveform_control.c file
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

#ifndef INC_WAVEFORM_CONTROL_H_
#define INC_WAVEFORM_CONTROL_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void WaveformControl_Init(void);
uint8_t WaveformControl_SetSquareFrequency(uint32_t target_hz);
uint8_t WaveformControl_SetSineFrequency(uint32_t target_hz);
uint8_t WaveformControl_SetFrequency(uint32_t target_hz);

#ifdef __cplusplus
}
#endif

#endif /* INC_WAVEFORM_CONTROL_H_ */
