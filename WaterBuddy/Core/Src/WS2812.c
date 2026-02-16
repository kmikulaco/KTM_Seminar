#include "WS2812.h"
#include "tim.h"
#include "dma.h"

// Constants za 48MHz i ARR=59
#define PWM_HI (38)  //"1" bit: 0.8µs HIGH = 0.8µs / 1.25µs = 64% → ARR × 0.64 = 38
#define PWM_LO (13)  //"0" bit: 0.4µs HIGH = 0.4µs / 1.25µs = 32% → ARR × 0.32 = 19

// LED buffer (GRB format)
uint8_t led_buffer[3 * NUM_LEDS];

// **ISPRAVKA: Povećaj DMA buffer za SVIH 8 LEDica**
#define BITS_PER_LED 24
#define RESET_BITS 200  // Povećaj reset na 100 bitova (125µs)
#define DMA_BUFFER_SIZE (BITS_PER_LED * NUM_LEDS + RESET_BITS)

uint16_t dma_buffer[DMA_BUFFER_SIZE];
volatile uint8_t transfer_in_progress = 0;

void WS2812_Init(void) {
    // Inicijalizacija - eksplicitno postavi sve na 0
    for(int i = 0; i < 3 * NUM_LEDS; i++) {
        led_buffer[i] = 0;
    }
    for(int i = 0; i < DMA_BUFFER_SIZE; i++) {
        dma_buffer[i] = 0;
    }
    transfer_in_progress = 0;

    // Inicijalno renderovanje
    WS2812_Render();
}

void WS2812_SetLED(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    if(index >= NUM_LEDS) return;

    // WS2812 koristi GRB format!
    led_buffer[3 * index] = g;     // Green
    led_buffer[3 * index + 1] = r; // Red
    led_buffer[3 * index + 2] = b; // Blue
}

void WS2812_SetAll(uint8_t r, uint8_t g, uint8_t b) {
    for(uint8_t i = 0; i < NUM_LEDS; i++) {
        WS2812_SetLED(i, r, g, b);
    }
}

void WS2812_Render(void) {
    // **ISPRAVKA: Čekaj da prethodni transfer završi**
    while(transfer_in_progress) {
        HAL_Delay(1);
    }

    // Encode LED data u DMA buffer
    uint32_t buffer_index = 0;

    // Encode sve LEDice
    for(uint8_t led = 0; led < NUM_LEDS; led++) {
        uint32_t color = (led_buffer[3*led] << 16) |    // Green
                        (led_buffer[3*led + 1] << 8) |  // Red
                        led_buffer[3*led + 2];          // Blue

        // Encode svih 24 bitova
        for(int8_t bit = 23; bit >= 0; bit--) {
            dma_buffer[buffer_index++] = (color & (1 << bit)) ? PWM_HI : PWM_LO;
        }
    }

    // Reset signal (>50µs LOW) - povećano na 100
    for(uint8_t i = 0; i < RESET_BITS; i++) {
        dma_buffer[buffer_index++] = 0;
    }

    transfer_in_progress = 1;
    HAL_TIM_PWM_Start_DMA(&htim3, TIM_CHANNEL_1, (uint32_t*)dma_buffer, buffer_index);
}

// DMA Complete Callback
void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim) {
    if(htim->Instance == TIM3) {
        HAL_TIM_PWM_Stop_DMA(&htim3, TIM_CHANNEL_1);
        transfer_in_progress = 0;
    }
}

void WS2812_Render_Clean(void) {
    // Čekaj da prethodni transfer potpuno završi
    while(transfer_in_progress) {
        HAL_Delay(2);
    }

    // Dodatni delay za reset
    HAL_Delay(1);

    // Onda renderaj
    WS2812_Render();
}
void HSV_to_RGB(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint8_t region, remainder, p, q, t;

    if (s == 0) {
        *r = v;
        *g = v;
        *b = v;
        return;
    }

    region = h / 43;
    remainder = (h - (region * 43)) * 6;

    p = (v * (255 - s)) >> 8;
    q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

    switch (region) {
        case 0:  *r = v; *g = t; *b = p; break;
        case 1:  *r = q; *g = v; *b = p; break;
        case 2:  *r = p; *g = v; *b = t; break;
        case 3:  *r = p; *g = q; *b = v; break;
        case 4:  *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}
