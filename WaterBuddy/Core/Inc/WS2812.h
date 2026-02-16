/*
 * WS2812.h
 *
 *  Created on: Nov 4, 2025
 *      Author: Admin
 */

#ifndef SRC_WS2812_H_
#define SRC_WS2812_H_

#include "main.h"
#include <stdint.h>

// Broj LEDica
#define NUM_LEDS 8

// Funkcije za kontrolu WS2812 LEDica
void WS2812_Init(void);
void WS2812_SetLED(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
void WS2812_SetAll(uint8_t r, uint8_t g, uint8_t b);
void WS2812_Render(void);
void WS2812_Render_Clean(void);
void HSV_to_RGB(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b);

// DMA callback funkcije (deklaracije)
void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim);

#endif /* SRC_WS2812_H_ */
