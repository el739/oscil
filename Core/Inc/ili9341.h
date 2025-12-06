/*
 * ili9341.h
 *
 *  Created on: Dec 4, 2025
 *      Author: 20817
 */

#ifndef INC_ILI9341_H_
#define INC_ILI9341_H_

#include "main.h"
#include "spi.h"
#include "gpio.h"

// 分辨率
#define ILI9341_WIDTH   320
#define ILI9341_HEIGHT  240

// 一些 16bit 颜色 (RGB565)
#define ILI9341_BLACK   0x0000
#define ILI9341_BLUE    0x001F
#define ILI9341_RED     0xF800
#define ILI9341_GREEN   0x07E0
#define ILI9341_WHITE   0xFFFF
#define ILI9341_YELLOW  0xFFE0

// 根据你接线修改这些宏
#define LCD_CS_LOW()    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14, GPIO_PIN_RESET)
#define LCD_CS_HIGH()   HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14, GPIO_PIN_SET)

#define LCD_DC_LOW()    HAL_GPIO_WritePin(GPIOF, GPIO_PIN_14, GPIO_PIN_RESET)
#define LCD_DC_HIGH()   HAL_GPIO_WritePin(GPIOF, GPIO_PIN_14, GPIO_PIN_SET)

#define LCD_RST_LOW()   HAL_GPIO_WritePin(GPIOE, GPIO_PIN_13, GPIO_PIN_RESET)
#define LCD_RST_HIGH()  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_13, GPIO_PIN_SET)

// 外部 SPI 句柄（Cube 生成）
extern SPI_HandleTypeDef hspi1;

void ILI9341_Init(void);
void ILI9341_SetRotation(uint8_t m);
void ILI9341_FillScreen(uint16_t color);
void ILI9341_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void ILI9341_DrawPixel(uint16_t x, uint16_t y, uint16_t color);
void ILI9341_DrawHLine(uint16_t x, uint16_t y, uint16_t w, uint16_t color);
void ILI9341_DrawChar(uint16_t x, uint16_t y, char c,
                      uint16_t color, uint16_t bg, uint8_t size);
void ILI9341_DrawString(uint16_t x, uint16_t y, const char *str,
                        uint16_t color, uint16_t bg, uint8_t size);




#endif /* INC_ILI9341_H_ */
