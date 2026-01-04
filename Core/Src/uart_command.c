/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    uart_command.c
  * @brief   UART command handler implementation
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

#include "uart_command.h"
#include "waveform_control.h"
#include "usart.h"
#include <string.h>
#include <stdlib.h>

static uint8_t uart_rx_byte = 0U;
static char uart_rx_buffer[16];
static char uart_cmd_buffer[16];
static volatile uint8_t uart_line_ready = 0U;
static uint8_t uart_rx_len = 0U;

static void SendUartText(const char *text);
static void ProcessUartLine(void);

void UartCommand_Init(void)
{
    uart_rx_len = 0U;
    uart_line_ready = 0U;
    HAL_UART_Receive_IT(&huart3, &uart_rx_byte, 1U);
}

uint8_t UartCommand_HasPending(void)
{
    return uart_line_ready;
}

void UartCommand_Process(void)
{
    if (uart_line_ready != 0U)
    {
        uart_line_ready = 0U;
        ProcessUartLine();
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3)
    {
        uint8_t byte = uart_rx_byte;
        if (byte == '\r' || byte == '\n')
        {
            if (uart_rx_len > 0U && uart_line_ready == 0U)
            {
                uint8_t copy_len = uart_rx_len;
                if (copy_len >= sizeof(uart_cmd_buffer))
                {
                    copy_len = (uint8_t)(sizeof(uart_cmd_buffer) - 1U);
                }
                memcpy(uart_cmd_buffer, uart_rx_buffer, copy_len);
                uart_cmd_buffer[copy_len] = '\0';
                uart_line_ready = 1U;
            }
            uart_rx_len = 0U;
        }
        else if (uart_rx_len < (sizeof(uart_rx_buffer) - 1U))
        {
            uart_rx_buffer[uart_rx_len++] = (char)byte;
        }
        else
        {
            uart_rx_len = 0U;
        }

        HAL_UART_Receive_IT(&huart3, &uart_rx_byte, 1U);
    }
}

static void SendUartText(const char *text)
{
    if (text == NULL)
    {
        return;
    }
    HAL_UART_Transmit(&huart3, (uint8_t *)text, strlen(text), 100U);
}

static void ProcessUartLine(void)
{
    char *line = uart_cmd_buffer;
    if (line[0] == '\0')
    {
        return;
    }

    char *end_ptr;
    while (*line == ' ' || *line == '\t')
    {
        line++;
    }

    uint8_t set_sine = 0U;
    if (*line == 's' || *line == 'S')
    {
        set_sine = 1U;
        line++;
        while (*line == ' ' || *line == '\t')
        {
            line++;
        }
    }

    unsigned long value = strtoul(line, &end_ptr, 10);
    while (*end_ptr == ' ' || *end_ptr == '\t')
    {
        end_ptr++;
    }

    if (end_ptr == line || *end_ptr != '\0')
    {
        SendUartText("ERR\r\n");
        return;
    }

    uint8_t ok = 0U;
    if (set_sine != 0U)
    {
        ok = WaveformControl_SetSineFrequency((uint32_t)value);
    }
    else
    {
        ok = WaveformControl_SetSquareFrequency((uint32_t)value);
    }

    if (ok != 0U)
    {
        SendUartText("OK\r\n");
    }
    else
    {
        SendUartText("ERR\r\n");
    }
}
