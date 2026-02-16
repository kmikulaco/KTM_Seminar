/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "i2c.h"
#include "rtc.h"
#include "tim.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>
#include "WS2812.h"
#include "st25dv.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
extern I2C_HandleTypeDef hi2c1;
#define FLASH_STORAGE_BASE_ADDR 0x0800FF00  // Zadnja stranica
#define FLASH_STORAGE_MAGIC 0xCA5A0001      // Magic broj za provjeru valjanosti

typedef struct {
	uint32_t magic;
	uint16_t casa_vode;
	uint16_t crc;
} flash_storage_t;

volatile uint16_t casa_vode = 0;  // brojač čaša
uint8_t gpo_prev = 1;             // prethodno stanje GPO pina
uint8_t btn_prev = 1;             // prethodno stanje gumba
uint8_t gpo_state_immediate = 1;

uint8_t target_leds = 4;       // Default: 4 LED = 1L (1-8 = 0.25-2L)
uint8_t interval_leds = 4;     // Default: 4 LED = 1h (1-8 = 15min-2h)
uint8_t daily_alarm_count = 0; // Broj alarma koji su se oglasili danas
uint8_t max_daily_alarms = 0; // Maksimalni broj alarma (izračun iz target_leds)
uint8_t morning_reset_hour = 10; // U koliko sati se resetira daily count (default 10h)
volatile uint8_t rtc_wakeup_flag = 0;  // Flag za RTC wake-up

uint8_t current_hour = 12;    // Default: 12 sati (podne)
uint8_t sleep_start_hour = 22; // Default: 22h - 6h = noćni period (8h spavanja)
uint8_t sleep_end_hour = 6;    // Default: 6h ujutro

// ⭐⭐ EXTERN FUNKCIJE IZ rtc.c ⭐⭐
extern void set_wakeup_timer_minutes(uint32_t minutes);
extern void set_wakeup_timer_seconds(uint32_t seconds);
extern void stop_wakeup_timer(void);
extern uint8_t get_current_hour(void);
extern uint8_t is_day_time(void);
extern uint8_t get_current_minute(void);
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_NVIC_Init(void);
/* USER CODE BEGIN PFP */

HAL_StatusTypeDef flash_read_storage(uint16_t *casa_value);
HAL_StatusTypeDef flash_erase_page(void);
HAL_StatusTypeDef flash_write_storage(uint16_t casa_value);
void save_casa_to_flash(void);
void send_casa_to_nfc(void);
void reset_casa_counter(void);

void setup_litara_mode(void);
void setup_interval_mode(void);
void show_setup_confirmation(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

uint16_t calculate_crc(uint16_t data) {
	return ~data;
}

// Čita podatke iz Flash memorije
HAL_StatusTypeDef flash_read_storage(uint16_t *casa_value) {
	flash_storage_t *storage = (flash_storage_t*) FLASH_STORAGE_BASE_ADDR;

	// Provjeri magic broj
	if (storage->magic != FLASH_STORAGE_MAGIC) {
		return HAL_ERROR;
	}

	// Provjeri CRC
	if (storage->crc != calculate_crc(storage->casa_vode)) {
		return HAL_ERROR;
	}

	*casa_value = storage->casa_vode;
	return HAL_OK;
}

// Briše Flash stranicu prije pisanja
HAL_StatusTypeDef flash_erase_page(void) {
	HAL_StatusTypeDef status;

	HAL_FLASH_Unlock();

	FLASH_EraseInitTypeDef erase;
	erase.TypeErase = FLASH_TYPEERASE_PAGES;
	erase.Page = (FLASH_STORAGE_BASE_ADDR - FLASH_BASE) / FLASH_PAGE_SIZE;
	erase.NbPages = 1;

	uint32_t page_error;
	status = HAL_FLASHEx_Erase(&erase, &page_error);

	HAL_FLASH_Lock();

	return status;
}

// Piše podatke u Flash memoriju
HAL_StatusTypeDef flash_write_storage(uint16_t casa_value) {
	HAL_StatusTypeDef status;
	flash_storage_t storage;

	// Pripremi podatke
	storage.magic = FLASH_STORAGE_MAGIC;
	storage.casa_vode = casa_value;
	storage.crc = calculate_crc(casa_value);

	// Prvo obriši stranicu
	status = flash_erase_page();
	if (status != HAL_OK) {
		return status;
	}

	HAL_FLASH_Unlock();

	// Zapiši podatke - STM32G0 koristi double-word programming
	uint64_t *src = (uint64_t*) &storage;
	uint64_t *dst = (uint64_t*) FLASH_STORAGE_BASE_ADDR;

	for (int i = 0; i < sizeof(flash_storage_t) / 8; i++) {
		status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, (uint32_t) dst,
				*src);
		if (status != HAL_OK) {
			break;
		}
		dst++;
		src++;
	}

	HAL_FLASH_Lock();
	return status;
}

// Funkcija za spremanje kada se broj čaša promijeni
void save_casa_to_flash(void) {
	if (flash_write_storage(casa_vode) == HAL_OK) {
		// Uspješno spremljeno
	}
}

// Funkcija za slanje broja čaša na NFC tag (koristi biblioteku)
void send_casa_to_nfc(void) {
	ST25DV_SendCasaCount(casa_vode);  // ← BIBLIOTEKA
}

// Funkcija za reset brojača na 0
void reset_casa_counter(void) {
	casa_vode = 0;

	// Resetiraj Flash memoriju
	if (flash_write_storage(0) == HAL_OK) {
		// Flash uspješno resetiran
	}

	// Resetiraj NFC tag koristeći biblioteku
	char reset_buffer[32];
	snprintf(reset_buffer, sizeof(reset_buffer), "0 čaša vode");
	ST25DV_WriteNDEFText(reset_buffer);
}

void setup_litara_mode(void) {
	uint8_t confirmed = 0;
	uint32_t last_action_time = HAL_GetTick();

	for (int i = 0; i < NUM_LEDS; i++) {
		WS2812_SetLED(i, 0, 0, 0);
	}
	WS2812_Render();

	// Osvijetli prvih target_leds LEDica počevši od LED 2 (kružno)
	for (int i = 0; i < target_leds; i++) {
		int led = (i + 2) % NUM_LEDS;
		WS2812_SetLED(led, 0, 30, 50);
	}
	WS2812_Render();

	__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 18382 / 128);
	HAL_Delay(100);
	__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);

	while (!confirmed) {
		// Timeout nakon 30 sekundi - automatski potvrdi
		if ((HAL_GetTick() - last_action_time) > 30000) {
			confirmed = 1;
			break;
		}

		// Provjeri L gumb (povećaj)
		if (HAL_GPIO_ReadPin(BTN_L_GPIO_Port, BTN_L_Pin) == GPIO_PIN_SET) {
			HAL_Delay(50); // Debounce
			if (HAL_GPIO_ReadPin(BTN_L_GPIO_Port, BTN_L_Pin) == GPIO_PIN_SET) {
				last_action_time = HAL_GetTick();

				// Povećaj broj LEDica (ciklički 1-8)
				target_leds++;
				if (target_leds > 8) {
					target_leds = 1;
				}

				for (int i = 0; i < NUM_LEDS; i++) {
					WS2812_SetLED(i, 0, 0, 0);
				}

				for (int i = 0; i < target_leds; i++) {
					int led = (i + 2) % NUM_LEDS;  //  POČINJE OD LED 2
					WS2812_SetLED(led, 0, 30, 50);
				}
				WS2812_Render();

				__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 21818 / 128);
				HAL_Delay(50);
				__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);

				while (HAL_GPIO_ReadPin(BTN_L_GPIO_Port, BTN_L_Pin)
						== GPIO_PIN_SET) {
					HAL_Delay(10);
				}
			}
		}

		// Provjeri R gumb za potvrdu
		if (HAL_GPIO_ReadPin(BTN_R_GPIO_Port, BTN_R_Pin) == GPIO_PIN_SET) {
			HAL_Delay(50);
			if (HAL_GPIO_ReadPin(BTN_R_GPIO_Port, BTN_R_Pin) == GPIO_PIN_SET) {
				confirmed = 1;

				// Potvrdni zvuk
				__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 18382 / 128);
				HAL_Delay(150);
				__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);

				// Čekaj otpuštanje
				while (HAL_GPIO_ReadPin(BTN_R_GPIO_Port, BTN_R_Pin)
						== GPIO_PIN_SET) {
					HAL_Delay(10);
				}
			}
		}

		HAL_Delay(10);
	}
	max_daily_alarms = target_leds;
	daily_alarm_count = 0;
}

void setup_interval_mode(void) {
	uint8_t confirmed = 0;
	uint32_t last_action_time = HAL_GetTick();

	// PRVO POKAŽI DEFAULT (4 LED = 1h) - POČINJE OD LED 2
	for (int i = 0; i < NUM_LEDS; i++) {
		WS2812_SetLED(i, 0, 0, 0);
	}
	WS2812_Render();

	for (int i = 0; i < interval_leds; i++) {
		int led = (i + 2) % NUM_LEDS;
		WS2812_SetLED(led, 50, 0, 0);
	}
	WS2812_Render();

	__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 18382 / 128);
	HAL_Delay(100);
	__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);

	while (!confirmed) {
		// Timeout nakon 30 sekundi
		if ((HAL_GetTick() - last_action_time) > 30000) {
			confirmed = 1;
			break;
		}

		// Provjeri L gumb (povećaj)
		if (HAL_GPIO_ReadPin(BTN_L_GPIO_Port, BTN_L_Pin) == GPIO_PIN_SET) {
			HAL_Delay(50);
			if (HAL_GPIO_ReadPin(BTN_L_GPIO_Port, BTN_L_Pin) == GPIO_PIN_SET) {
				last_action_time = HAL_GetTick();

				interval_leds++;
				if (interval_leds > 8) {
					interval_leds = 1;
				}

				for (int i = 0; i < NUM_LEDS; i++) {
					WS2812_SetLED(i, 0, 0, 0);
				}

				for (int i = 0; i < interval_leds; i++) {
					int led = (i + 2) % NUM_LEDS;
					WS2812_SetLED(led, 50, 0, 0);
				}
				WS2812_Render();

				__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 21818 / 128);
				HAL_Delay(50);
				__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);

				while (HAL_GPIO_ReadPin(BTN_L_GPIO_Port, BTN_L_Pin)
						== GPIO_PIN_SET) {
					HAL_Delay(10);
				}
			}
		}

		// Provjeri R gumb za potvrdu
		if (HAL_GPIO_ReadPin(BTN_R_GPIO_Port, BTN_R_Pin) == GPIO_PIN_SET) {
			HAL_Delay(50);
			if (HAL_GPIO_ReadPin(BTN_R_GPIO_Port, BTN_R_Pin) == GPIO_PIN_SET) {
				confirmed = 1;

				// Potvrdni zvuk
				__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 18382 / 128);
				HAL_Delay(150);
				__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);

				while (HAL_GPIO_ReadPin(BTN_R_GPIO_Port, BTN_R_Pin)
						== GPIO_PIN_SET) {
					HAL_Delay(10);
				}
			}
		}

		HAL_Delay(10);
	}
}

// Postavi trenutno vrijeme u RTC - BINARNO
void setup_current_time_mode(void) {
	uint8_t confirmed = 0;
	uint32_t last_action_time = HAL_GetTick();

	// Prikaži trenutni sat (default 12h)
	for (int i = 0; i < NUM_LEDS; i++) {
		WS2812_SetLED(i, 0, 0, 0);
	}
	WS2812_Render();

	for (int hour = 0; hour <= current_hour; hour++) {
		int led = (hour + 2) % NUM_LEDS;
		if (hour <= 7) {
			WS2812_SetLED(led, 50, 0, 0);
		} else if (hour <= 15) {
			WS2812_SetLED(led, 0, 30, 0);
		} else {
			WS2812_SetLED(led, 0, 0, 30);
		}
	}
	WS2812_Render();

	__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 18382 / 128);
	HAL_Delay(100);
	__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);

	while (!confirmed) {
		if ((HAL_GetTick() - last_action_time) > 30000) {
			confirmed = 1;
			break;
		}

		if (HAL_GPIO_ReadPin(BTN_L_GPIO_Port, BTN_L_Pin) == GPIO_PIN_SET) {
			HAL_Delay(50);
			if (HAL_GPIO_ReadPin(BTN_L_GPIO_Port, BTN_L_Pin) == GPIO_PIN_SET) {
				last_action_time = HAL_GetTick();

				current_hour++;
				if (current_hour > 23)
					current_hour = 0;

				// OSVJEŽI PRIKAZ
				for (int i = 0; i < NUM_LEDS; i++) {
					WS2812_SetLED(i, 0, 0, 0);
				}

				for (int hour = 0; hour <= current_hour; hour++) {
					int led = (hour + 2) % NUM_LEDS;
					if (hour <= 7) {
						WS2812_SetLED(led, 50, 0, 0);
					} else if (hour <= 15) {
						WS2812_SetLED(led, 0, 30, 0);
					} else {
						WS2812_SetLED(led, 0, 0, 30);
					}
				}
				WS2812_Render();

				uint32_t note = 18382 + (current_hour * 100);
				if (note > 32876)
					note = 32876;
				__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, note / 128);
				HAL_Delay(50);
				__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);

				while (HAL_GPIO_ReadPin(BTN_L_GPIO_Port, BTN_L_Pin)
						== GPIO_PIN_SET) {
					HAL_Delay(10);
				}
			}
		}

		if (HAL_GPIO_ReadPin(BTN_R_GPIO_Port, BTN_R_Pin) == GPIO_PIN_SET) {
			HAL_Delay(50);
			if (HAL_GPIO_ReadPin(BTN_R_GPIO_Port, BTN_R_Pin) == GPIO_PIN_SET) {
				confirmed = 1;

				// POSTAVLJANJE RTC VREMENA (SATI, 00 MINUTA, 00 SEKUNDI)
				RTC_TimeTypeDef sTime = { 0 };
				RTC_DateTypeDef sDate = { 0 };

				sTime.Hours = current_hour;  // Binarno 0-23
				sTime.Minutes = 0;           // 00 minuta
				sTime.Seconds = 0;           // 00 sekundi
				sTime.SubSeconds = 0;
				sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
				sTime.StoreOperation = RTC_STOREOPERATION_RESET;

				sDate.WeekDay = RTC_WEEKDAY_MONDAY;
				sDate.Month = RTC_MONTH_JANUARY;
				sDate.Date = 1;
				sDate.Year = 0;

				if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK) {
					Error_Handler();
				}
				if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK) {
					Error_Handler();
				}

				// Potvrdni zvuk
				for (int i = 0; i < 3; i++) {
					__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 18382 / 128);
					HAL_Delay(50);
					__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
					if (i < 2)
						HAL_Delay(30);
				}

				// Kratko blinkanje
				if (current_hour <= 7) {
					WS2812_SetAll(50, 0, 0);
				} else if (current_hour <= 15) {
					WS2812_SetAll(0, 30, 0);
				} else {
					WS2812_SetAll(0, 0, 30);
				}
				WS2812_Render();
				HAL_Delay(300);
				WS2812_SetAll(0, 0, 0);
				WS2812_Render();

				while (HAL_GPIO_ReadPin(BTN_R_GPIO_Port, BTN_R_Pin)
						== GPIO_PIN_SET) {
					HAL_Delay(10);
				}
			}
		}

		HAL_Delay(10);
	}
}

void show_setup_confirmation(void) {
	// Brzo pali i gasi sve LEDice 2 puta
	for (int blink = 0; blink < 2; blink++) {
		WS2812_SetAll(20, 20, 20);  // BIJELA
		WS2812_Render();
		__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 18382 / 128);
		HAL_Delay(150);

		WS2812_SetAll(0, 0, 0);
		WS2812_Render();
		__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
		HAL_Delay(150);
	}
}

/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void) {

	/* USER CODE BEGIN 1 */

	/* USER CODE END 1 */

	/* MCU Configuration--------------------------------------------------------*/

	/* Reset of all peripherals, Initializes the Flash interface and the Systick. */
	HAL_Init();

	/* USER CODE BEGIN Init */

	/* USER CODE END Init */

	/* Configure the system clock */
	SystemClock_Config();

	/* USER CODE BEGIN SysInit */

	/* USER CODE END SysInit */

	/* Initialize all configured peripherals */
	MX_GPIO_Init();
	MX_DMA_Init();
	MX_TIM1_Init();
	MX_I2C1_Init();
	MX_TIM3_Init();
	MX_RTC_Init();

	/* Initialize interrupts */
	MX_NVIC_Init();
	/* USER CODE BEGIN 2 */
	HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
	HAL_GPIO_WritePin(LED_SW_GPIO_Port, LED_SW_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(LPD_GPIO_Port, LPD_Pin, GPIO_PIN_RESET);
	HAL_Delay(250);
	WS2812_Init();

	if (ST25DV_Init(&hi2c1) != HAL_OK) {
		WS2812_SetAll(50, 0, 0);
		WS2812_Render();
		__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 18382 / 128);
		HAL_Delay(100);
		WS2812_SetAll(0, 0, 0);
		WS2812_Render();
		__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
		HAL_Delay(1000);
	}

	HAL_Delay(10); // Mali delay za stabilnost GPIO

	if (HAL_GPIO_ReadPin(BTN_L_GPIO_Port, BTN_L_Pin) == GPIO_PIN_SET) {
		// Gumb je pritisnut pri pokretanju - čekaj 3 sekunde
		uint32_t reset_start_time = HAL_GetTick();

		while ((HAL_GetTick() - reset_start_time) < 3000) {
			// Ako se gumb otpusti prije 3 sekunde, prekini reset
			if (HAL_GPIO_ReadPin(BTN_L_GPIO_Port, BTN_L_Pin)
					== GPIO_PIN_RESET) {
				break;
			}
			HAL_Delay(100);
		}

		// Provjeri je li gumb i dalje pritisnut nakon 3 sekunde
		if (HAL_GPIO_ReadPin(BTN_L_GPIO_Port, BTN_L_Pin) == GPIO_PIN_SET) {
			// RESETIRAJ NA 0!
			reset_casa_counter();
			while (HAL_GPIO_ReadPin(BTN_L_GPIO_Port, BTN_L_Pin) == GPIO_PIN_SET) {
				HAL_Delay(10); // Čekaj dok se gumb ne otpusti
			}
			WS2812_SetAll(0, 50, 0);
			WS2812_Render();
			__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 32876 / 128);
			HAL_Delay(100);
			WS2812_SetAll(0, 0, 0);
			WS2812_Render();
			__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
			HAL_Delay(1000);
		}
	} else {
		uint16_t saved_casa = 0;
		if (flash_read_storage(&saved_casa) == HAL_OK) {
			casa_vode = saved_casa;
		} else {
			casa_vode = 0;
		}
	}

	// Inicijaliziranje NFCa sa trenutnom vrijednošću KORISTEĆI BIBLIOTEKU
	char init_buffer[32];
	snprintf(init_buffer, sizeof(init_buffer), "%u čaša vode", casa_vode);
	if (ST25DV_WriteNDEFText(init_buffer) != HAL_OK) {
	}

	setup_litara_mode();

	// Faza 2: Odabir intervala
	setup_interval_mode();

	// ⭐ FAZA 3: Postavljanje trenutnog vremena (sati)
	setup_current_time_mode();

	// Potvrda setupa
	show_setup_confirmation();

	uint8_t current_hour_now = get_current_hour();

	// Postavi početno stanje - alarmi su već aktivirani u RTC inicijalizaciji!
	if (current_hour_now >= 10 && current_hour_now <= 22) {
		// DAN
		daily_alarm_count = 0;  // Reset za dan

		// Pokreni intervale za 5 sekundi
		if (daily_alarm_count <= target_leds) {
			set_wakeup_timer_seconds(5);
		}
	} else {
		// NOĆ
		daily_alarm_count = 0;  // Reset za noć
		// alarm B će se automatski aktivirati u 22:00
	}

	// Mali delay prije ulaska u sleep
	HAL_Delay(500);

	/* USER CODE END 2 */

	/* Infinite loop */
	/* USER CODE BEGIN WHILE */
	while (1) {
		// Spavanje
		HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_1);
		HAL_GPIO_WritePin(LED_SW_GPIO_Port, LED_SW_Pin, GPIO_PIN_SET);
		HAL_GPIO_WritePin(LPD_GPIO_Port, LPD_Pin, GPIO_PIN_SET);
		HAL_Delay(100);
		HAL_SuspendTick();
		HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);

		// ⭐⭐ BUĐENJE ⭐⭐

		// Provjera GPIO stanje ODMAH nakon budenja
		gpo_state_immediate = HAL_GPIO_ReadPin(GPO_GPIO_Port, GPO_Pin);
		uint8_t BTNL_state_immediate = HAL_GPIO_ReadPin(BTN_L_GPIO_Port,
		BTN_L_Pin);
		uint8_t BTNR_state_immediate = HAL_GPIO_ReadPin(BTN_R_GPIO_Port,
		BTN_R_Pin);

		SystemClock_Config();
		HAL_ResumeTick();

		if (rtc_wakeup_flag) {
			uint8_t waiting_for_btnR = 0;
			uint8_t btnL_was_pressed = 0;
			uint32_t btnL_press_time = 0;
			uint32_t orange_fade_start = 0;
			uint8_t orange_fade_active = 0;
			uint8_t alarm_paused = 0;
			uint8_t snooze_active = 0;

			// Paljenje LED driver a
			HAL_GPIO_WritePin(LED_SW_GPIO_Port, LED_SW_Pin, GPIO_PIN_RESET);

			// Petlja za alarm
			while (1) {
				uint32_t current_time = HAL_GetTick();

				// ⭐⭐ SAMO AKO ALARM NIJE PAUZIRAN ⭐⭐
				if (!alarm_paused && !orange_fade_active && !waiting_for_btnR
						&& !snooze_active) {
					HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
					__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 32876 / 64);
					WS2812_SetAll(255, 0, 0);
					WS2812_Render();
					HAL_Delay(100);

					__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
					WS2812_SetAll(0, 0, 0);
					WS2812_Render();

					set_wakeup_timer_seconds(4);
					HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_1);
					HAL_GPIO_WritePin(LED_SW_GPIO_Port, LED_SW_Pin,
							GPIO_PIN_SET);
					HAL_GPIO_WritePin(LPD_GPIO_Port, LPD_Pin, GPIO_PIN_SET);
					HAL_Delay(100);
					HAL_SuspendTick();
					HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON,
					PWR_STOPENTRY_WFI);

					SystemClock_Config();
					HAL_ResumeTick();
					HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);
					HAL_GPIO_WritePin(LED_SW_GPIO_Port, LED_SW_Pin,
							GPIO_PIN_RESET);
					HAL_GPIO_WritePin(LPD_GPIO_Port, LPD_Pin, GPIO_PIN_RESET);
				}
				alarm_paused = 0;
				// ⭐ ANIMACIJA NARANČASTOG FADE-OUT-a
				if (orange_fade_active) {
					alarm_paused = 1;  //
					uint32_t fade_elapsed = current_time - orange_fade_start;

					if (fade_elapsed < 1000) {
						uint8_t red = 50;
						uint8_t green = 25 - (fade_elapsed * 25 / 1000);
						WS2812_SetAll(red, green, 0);
						WS2812_Render();
					} else {
						orange_fade_active = 0;
						alarm_paused = 0;
						WS2812_SetAll(0, 0, 0);
						WS2812_Render();
					}
				}

				// ⭐ PROVJERA L GUMBA
				if (HAL_GPIO_ReadPin(BTN_L_GPIO_Port, BTN_L_Pin)
						== GPIO_PIN_SET) {
					static uint32_t last_btnL_alarm_time = 0;
					if ((current_time - last_btnL_alarm_time) > 50) {
						alarm_paused = 1;
						btnL_press_time = current_time;
						waiting_for_btnR = 1;
						btnL_was_pressed = 1;
						orange_fade_active = 1;
						orange_fade_start = current_time;
						last_btnL_alarm_time = current_time;

						__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
						WS2812_SetAll(50, 25, 0);
						WS2812_Render();
						__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1,
								21818 / 128);
						HAL_Delay(80);
						__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
					}
				}

				// ⭐ PROVJERA TIMEOUTA ZA L+R
				if (waiting_for_btnR
						&& (current_time - btnL_press_time > 1000)) {
					waiting_for_btnR = 0;
					btnL_was_pressed = 0;
					orange_fade_active = 0;
					alarm_paused = 0;

					for (int blink = 0; blink < 2; blink++) {
						HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
						WS2812_SetAll(50, 0, 0);
						WS2812_Render();
						__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1,
								21818 / 128);
						HAL_Delay(100);
						WS2812_SetAll(0, 0, 0);
						WS2812_Render();
						__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
						if (blink < 1)
							HAL_Delay(50);
					}
					HAL_Delay(1000);
				}

				// ⭐ PROVJERA R GUMBA AKO JE L BIO PRITISNUT
				if (waiting_for_btnR
						&& HAL_GPIO_ReadPin(BTN_R_GPIO_Port, BTN_R_Pin)
								== GPIO_PIN_SET) {
					static uint32_t last_btnR_alarm_time = 0;
					if ((current_time - last_btnR_alarm_time) > 50) {
						last_btnR_alarm_time = current_time;
						HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
						__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
						WS2812_SetAll(0, 0, 0);
						WS2812_Render();

						for (int blink = 0; blink < 2; blink++) {
							WS2812_SetAll(0, 50, 0);
							WS2812_Render();
							__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1,
									18382 / 128);
							HAL_Delay(100);
							WS2812_SetAll(0, 0, 0);
							WS2812_Render();
							__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
							if (blink < 1)
								HAL_Delay(50);
						}

						while (HAL_GPIO_ReadPin(BTN_R_GPIO_Port, BTN_R_Pin)
								== GPIO_PIN_SET) {
							HAL_Delay(10);
						}

						static uint16_t last_saved_value = 0;
						casa_vode++;

						if (is_day_time()) {
							daily_alarm_count++;
							if (daily_alarm_count >= target_leds) {
								stop_wakeup_timer();
							} else if (daily_alarm_count <= target_leds) {
								set_wakeup_timer_minutes(interval_leds * 15);
							}
						}

						if (casa_vode != last_saved_value) {
							save_casa_to_flash();
							last_saved_value = casa_vode;
						}

						rtc_wakeup_flag = 0;
						break;
					}
				}

				// ⭐ PROVJERA SAMO R GUMBA ZA SNOOZE
				if (!btnL_was_pressed
						&& HAL_GPIO_ReadPin(BTN_R_GPIO_Port, BTN_R_Pin)
								== GPIO_PIN_SET) {
					static uint32_t last_btnR_solo_time = 0;
					if ((current_time - last_btnR_solo_time) > 50) {
						last_btnR_solo_time = current_time;

						snooze_active = 1;
						alarm_paused = 1;
						HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
						__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
						WS2812_SetAll(0, 0, 0);
						WS2812_Render();

						WS2812_SetAll(50, 0, 20);
						WS2812_Render();
						__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1,
								24489 / 128);
						HAL_Delay(200);
						__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
						WS2812_SetAll(0, 0, 0);
						WS2812_Render();

						while (HAL_GPIO_ReadPin(BTN_R_GPIO_Port, BTN_R_Pin)
								== GPIO_PIN_SET) {
							HAL_Delay(10);
						}

						// SNOOZE SLEEP
						set_wakeup_timer_seconds(10);
						HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_1);
						HAL_GPIO_WritePin(LED_SW_GPIO_Port, LED_SW_Pin,
								GPIO_PIN_SET);
						HAL_GPIO_WritePin(LPD_GPIO_Port, LPD_Pin, GPIO_PIN_SET);
						HAL_Delay(100);
						HAL_SuspendTick();
						HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON,
						PWR_STOPENTRY_WFI);

						SystemClock_Config();
						HAL_ResumeTick();
						HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);
						HAL_GPIO_WritePin(LED_SW_GPIO_Port, LED_SW_Pin,
								GPIO_PIN_RESET);
						HAL_GPIO_WritePin(LPD_GPIO_Port, LPD_Pin,
								GPIO_PIN_RESET);
						snooze_active = 0;
						continue;
					}
				}

				// ⭐ PROVJERA SAMO L GUMBA (bez R)
				if (!waiting_for_btnR
						&& HAL_GPIO_ReadPin(BTN_L_GPIO_Port, BTN_L_Pin)
								== GPIO_PIN_SET) {
					static uint32_t last_btnL_solo_time = 0;
					if ((current_time - last_btnL_solo_time) > 50) {
						last_btnL_solo_time = current_time;
						alarm_paused = 1;


						HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
						__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
						WS2812_SetAll(30, 0, 0);
						WS2812_Render();

						while (HAL_GPIO_ReadPin(BTN_L_GPIO_Port, BTN_L_Pin)
								== GPIO_PIN_SET) {
							HAL_Delay(10);
						}

						alarm_paused = 0;
						WS2812_SetAll(0, 0, 0);
						WS2812_Render();
					}
				}

				HAL_Delay(10);
			}
		}

		// --- OBRADA GPO (NFC) ---
		if (gpo_state_immediate == 0) {
			HAL_GPIO_WritePin(LPD_GPIO_Port, LPD_Pin, GPIO_PIN_RESET);
			HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
			HAL_GPIO_WritePin(LED_SW_GPIO_Port, LED_SW_Pin, GPIO_PIN_RESET);
			WS2812_SetAll(50, 0, 50);
			WS2812_Render();
			__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 32876 / 128);
			HAL_Delay(100);
			WS2812_SetAll(0, 0, 0);
			WS2812_Render();

			for (int brightness = 50; brightness >= 0; brightness -= 2) {
				WS2812_SetAll(brightness, 0, brightness);
				WS2812_Render();
				HAL_Delay(5);
			}

			__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
			send_casa_to_nfc();
			gpo_prev = 1;

		} else {
			gpo_prev = gpo_state_immediate;
		}

		static uint32_t btnL_press_time = 0;
		static uint8_t waiting_for_btnR = 0;
		static uint8_t btnL_was_pressed = 0;
		static uint8_t btnR_already_processed = 0;

		//  DEBOUNCE
		static uint32_t last_btnL_time = 0;
		uint32_t now = HAL_GetTick();

		if (btn_prev == 1 && BTNL_state_immediate == 1) {
			// provjera je li prošlo dovoljno vremena
			if ((now - last_btnL_time) > 50) {
				btnL_press_time = now;
				waiting_for_btnR = 1;
				btnL_was_pressed = 1;
				btnR_already_processed = 0;  // Reset
				last_btnL_time = now;

				// čekam BTN_R unutar 2 sekunde
				HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
				HAL_GPIO_WritePin(LED_SW_GPIO_Port, LED_SW_Pin, GPIO_PIN_RESET);

				WS2812_SetAll(40, 40, 0);
				WS2812_Render();
				__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 32876 / 128);
				HAL_Delay(180);

				for (int brightness = 40; brightness >= 0; brightness -= 2) {
					WS2812_SetAll(brightness, brightness, 0);
					WS2812_Render();
					HAL_Delay(5);
				}

				__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
				if (waiting_for_btnR) {
					while (waiting_for_btnR) {
						now = HAL_GetTick();

						if ((now - btnL_press_time) > 1000) {
							waiting_for_btnR = 0;

							HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
							HAL_GPIO_WritePin(LED_SW_GPIO_Port, LED_SW_Pin,
									GPIO_PIN_RESET);

							WS2812_SetAll(50, 0, 0);
							WS2812_Render();
							htim1.Instance->ARR = 21818;  // F#4 (370Hz)
							__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1,
									21818 / 128);
							HAL_Delay(180);

							__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
							HAL_Delay(40);

							htim1.Instance->ARR = 26042;  // A3 (220Hz)
							__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1,
									26042 / 128);
							HAL_Delay(80);

							for (int brightness = 50; brightness >= 0;
									brightness -= 2) {
								WS2812_SetAll(brightness, 0, 0);
								WS2812_Render();
								HAL_Delay(5);
							}

							__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
							break;
						}

						// Provjera je li BTN_R pritisnut
						BTNR_state_immediate = HAL_GPIO_ReadPin(BTN_R_GPIO_Port,
						BTN_R_Pin);

						// DEBOUNCE
						static uint32_t last_btnR_time = 0;
						if (BTNR_state_immediate == 1
								&& (now - last_btnR_time) > 50) {
							last_btnR_time = now;
							waiting_for_btnR = 0;
							btnR_already_processed = 1;

							HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
							HAL_GPIO_WritePin(LED_SW_GPIO_Port, LED_SW_Pin,
									GPIO_PIN_RESET);

							for (int i = 0; i < NUM_LEDS; i++) {
								WS2812_SetLED(i, 0, 0, 0);
							}
							WS2812_Render();

							// ----------- Faza 1: PALJENJE (od LED 2 do 1) -----------
							for (int idx = 0; idx < NUM_LEDS; idx++) {
								int led = (idx + 2) % NUM_LEDS;  //

								for (int svjetlina = 0; svjetlina <= 50;
										svjetlina += 2) {
									uint32_t musical_notes[] = { 36923, // C3 (130Hz)
											32876,  // D3 (146Hz)
											29268,  // E3 (164Hz)
											27586,  // F3 (174Hz)
											24489,  // G3 (196Hz)
											21818,  // A3 (220Hz)
											19512,  // B3 (246Hz)
											18382   // C4 (261Hz)
											};

									int note_idx = idx;
									if (note_idx < 8) {
										uint32_t arr_value =
												musical_notes[note_idx];
										htim1.Instance->ARR = arr_value;
										__HAL_TIM_SET_COMPARE(&htim1,
												TIM_CHANNEL_1, arr_value / 128);
									}

									WS2812_SetLED(led, 0, svjetlina, 0);

									for (int prev = 0; prev < idx; prev++) {
										int prev_led = (prev + 2) % NUM_LEDS;
										WS2812_SetLED(prev_led, 0, 50, 0);
									}

									WS2812_Render();
									HAL_Delay(1);
								}
							}

							// ----------- Faza 2: GAŠENJE (od LED 2 do 1) -----------
							for (int idx = 0; idx < NUM_LEDS; idx++) {
								int led = (idx + 2) % NUM_LEDS;

								for (int svjetlina = 50; svjetlina >= 0;
										svjetlina -= 2) {
									uint32_t musical_notes_reverse[] = { 18382, // C4 (261Hz)
											19512,  // B3 (246Hz)
											21818,  // A3 (220Hz)
											24489,  // G3 (196Hz)
											27586,  // F3 (174Hz)
											29268,  // E3 (164Hz)
											32876,  // D3 (146Hz)
											36923  // C3 (130Hz)
											};

									int note_idx = idx;
									if (note_idx < 8) {
										uint32_t arr_value =
												musical_notes_reverse[note_idx];
										htim1.Instance->ARR = arr_value;
										__HAL_TIM_SET_COMPARE(&htim1,
												TIM_CHANNEL_1, arr_value / 128);
									}

									WS2812_SetLED(led, 0, svjetlina, 0);

									for (int remaining = idx + 1;
											remaining < NUM_LEDS; remaining++) {
										int remaining_led = (remaining + 2)
												% NUM_LEDS;
										WS2812_SetLED(remaining_led, 0, 50, 0);
									}
									for (int already_off = 0; already_off < idx;
											already_off++) {
										int off_led = (already_off + 2)
												% NUM_LEDS;
										WS2812_SetLED(off_led, 0, 0, 0);
									}

									WS2812_Render();
									HAL_Delay(1);
								}
							}
							__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);

							// INKREMENTACIJA BROJA ČAŠA I DNEVNI BROJ ALARMA
							static uint16_t last_saved_value = 0;
							casa_vode++;

							if (is_day_time()) {
								daily_alarm_count++; // Samo danju se broji u dnevni limit

								// Provjeri je li dostignut limit
								if (daily_alarm_count >= target_leds) {
									stop_wakeup_timer();
								}
							}
							// Noću se samo casa_vode poveća, daily_alarm_count ne

							if (casa_vode != last_saved_value) {
								save_casa_to_flash();
								last_saved_value = casa_vode;
							}
							break;
						}

						HAL_Delay(500);

					}
				}
			}
		}

		// Ako je samo BTN_R pritisnut
		static uint32_t last_btnR_solo_time = 0;
		now = HAL_GetTick();

		if (BTNR_state_immediate == 1 && !btnL_was_pressed
				&& !btnR_already_processed) {
			// DEBOUNCE
			if ((now - last_btnR_solo_time) > 50) {
				last_btnR_solo_time = now;

				HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
				HAL_GPIO_WritePin(LED_SW_GPIO_Port, LED_SW_Pin, GPIO_PIN_RESET);

				WS2812_SetAll(50, 0, 0);
				WS2812_Render();
				htim1.Instance->ARR = 21818;  // F#4 (370Hz)
				__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 21818 / 128);
				HAL_Delay(180);

				__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
				HAL_Delay(40);

				htim1.Instance->ARR = 26042;  // A3 (220Hz)
				__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 26042 / 128);
				HAL_Delay(80);

				for (int brightness = 50; brightness >= 0; brightness -= 2) {
					WS2812_SetAll(brightness, 0, 0);
					WS2812_Render();
					HAL_Delay(5);
				}

				__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
			}
		}

		if (!waiting_for_btnR) {
			btnL_was_pressed = 0;
			btnR_already_processed = 0;
		}

		if ((now - last_btnL_time) > 50) {
			btn_prev = BTNL_state_immediate;
		}

		HAL_Delay(5);
		if (rtc_wakeup_flag == 0) {  // Samo ako nismo u RTC alarm sekvenci
			WS2812_SetAll(0, 0, 0);

			// JE LI POPUNJEN SVAKIH 8 LEDICA ZELENOM? (MAX DOSEGNUT)
			if (daily_alarm_count >= NUM_LEDS) {
				HAL_Delay(200);
				// BLINKAJ 2 PUTA ZELENO - MAKSIMUM DOSEGNUT!
				for (int blink = 0; blink < 2; blink++) {
					// Upali sve LEDice zeleno
					for (int i = 0; i < NUM_LEDS; i++) {
						int led = (i + 2) % NUM_LEDS;
						WS2812_SetLED(led, 0, 30, 0);
					}
					WS2812_Render();
					__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 18382 / 128); // C4 ton
					HAL_Delay(100);
					WS2812_SetAll(0, 0, 0);
					WS2812_Render();
					__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);

					if (blink < 1)
						HAL_Delay(50);
				}
			} else {
				for (int i = 0; i < daily_alarm_count && i < NUM_LEDS; i++) {
					int led = (i + 2) % NUM_LEDS;

					if (max_daily_alarms > 0 && i == max_daily_alarms - 1) {
						// OVO JE CILJNA LED
						WS2812_SetLED(led, 50, 0, 30);  // dostignut cilj
					} else {
						// Normalna zelena LED
						WS2812_SetLED(led, 0, 30, 50);
					}
				}

				// Pokaži maksimum (Tirkizna) - SAMO AKO NIJE DOSTIGNUT
				// Ako je daily_alarm_count < max_daily_alarms, pokaži tirkiznu
				if (max_daily_alarms > 0 && max_daily_alarms <= NUM_LEDS
						&& daily_alarm_count < max_daily_alarms) {
					int max_led = (max_daily_alarms - 1 + 2) % NUM_LEDS;
					WS2812_SetLED(max_led, 0, 30, 50); // Tirkizna = maksimum (još nije dostignut)
				}

				WS2812_Render();
				HAL_Delay(800);
			}
		}

		WS2812_SetAll(0, 0, 0);
		WS2812_Render();
		__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);

		/* USER CODE END WHILE */

		/* USER CODE BEGIN 3 */
	}
	/* USER CODE END 3 */
}

/**
 * @brief System Clock Configuration
 * @retval None
 */
void SystemClock_Config(void) {
	RCC_OscInitTypeDef RCC_OscInitStruct = { 0 };
	RCC_ClkInitTypeDef RCC_ClkInitStruct = { 0 };

	/** Configure the main internal regulator output voltage
	 */
	HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

	/** Configure LSE Drive Capability
	 */
	HAL_PWR_EnableBkUpAccess();
	__HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);

	/** Initializes the RCC Oscillators according to the specified parameters
	 * in the RCC_OscInitTypeDef structure.
	 */
	RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI
			| RCC_OSCILLATORTYPE_LSE;
	RCC_OscInitStruct.LSEState = RCC_LSE_ON;
	RCC_OscInitStruct.HSIState = RCC_HSI_ON;
	RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
	RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
	RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
	RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
	RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV1;
	RCC_OscInitStruct.PLL.PLLN = 9;
	RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
	RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV5;
	RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV3;
	if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
		Error_Handler();
	}

	/** Initializes the CPU, AHB and APB buses clocks
	 */
	RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
			| RCC_CLOCKTYPE_PCLK1;
	RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
	RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;

	if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK) {
		Error_Handler();
	}
}

/**
 * @brief NVIC Configuration.
 * @retval None
 */
static void MX_NVIC_Init(void) {
	/* EXTI0_1_IRQn interrupt configuration */
	HAL_NVIC_SetPriority(EXTI0_1_IRQn, 0, 0);
	HAL_NVIC_EnableIRQ(EXTI0_1_IRQn);
	/* EXTI4_15_IRQn interrupt configuration */
	HAL_NVIC_SetPriority(EXTI4_15_IRQn, 0, 0);
	HAL_NVIC_EnableIRQ(EXTI4_15_IRQn);
	/* RTC_TAMP_IRQn interrupt configuration */
	HAL_NVIC_SetPriority(RTC_TAMP_IRQn, 0, 0);
	HAL_NVIC_EnableIRQ(RTC_TAMP_IRQn);
}

/* USER CODE BEGIN 4 */
// ⭐ NOVA ALARM A CALLBACK
// ⭐ ALARM A CALLBACK - aktivira se kada je sat = 10 (bilo koja minuta)
void HAL_RTC_AlarmAEventCallback(RTC_HandleTypeDef *hrtc) {

	daily_alarm_count = 0;  // RESET dnevnog brojača!

	rtc_wakeup_flag = 1;  // Označi buđenje
}

// ⭐ ALARM B CALLBACK
void HAL_RTCEx_AlarmBEventCallback(RTC_HandleTypeDef *hrtc) {

	stop_wakeup_timer();    // Zaustavi intervale
}

void HAL_RTCEx_WakeUpTimerEventCallback(RTC_HandleTypeDef *hrtc) {
// ----------- INTERVALNI ALARM -----------

// Provjeri je li dan
	if (is_day_time()) {
		rtc_wakeup_flag = 1;
	}
}

/* USER CODE END 4 */

/**
 * @brief  This function is executed in case of error occurrence.
 * @retval None
 */
void Error_Handler(void) {
	/* USER CODE BEGIN Error_Handler_Debug */
	/* User can add his own implementation to report the HAL error return state */
	__disable_irq();
	while (1) {
	}
	/* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
