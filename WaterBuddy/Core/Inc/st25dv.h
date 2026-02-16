/*
 * st25dv.h
 *
 *  Created on: Dec 7, 2025
 *      Author: Admin
 */
#ifndef ST25DV_H
#define ST25DV_H
#ifdef __cplusplus
extern "C" {
#endif
/* Includes ------------------------------------------------------------------*/
#include "main.h"        /* Glavne HAL biblioteke */
#include <string.h>      /* Funkcije za rad sa stringovima */
#include <stdio.h>       /* Standardne ulazno-izlazne funkcije */

/* Defines -------------------------------------------------------------------*/
#define ST25DV_7BIT_ADDR    0x53        /* 7-bitna I2C adresa ST25DV čipa */
#define ST25DV_DEV_ADDR     (ST25DV_7BIT_ADDR << 1)  /* 8-bitna I2C adresa za HAL */
#define NDEF_START_ADDR     0x0004      /* Početna adresa NDEF podataka u EEPROM-u */
#define CHUNK_MAX           32          /* Maksimalna veličina jednog I2C upisa */

/* Granice adresnog prostora za različite modele ST25DV */
#define ST25DV04_MAX_ADDR   0x01FF      /* Zadnja adresa za ST25DV04KC (4 Kbit = 512 B) */
#define ST25DV16_MAX_ADDR   0x07FF      /* Zadnja adresa za ST25DV16KC (16 Kbit = 2 KB) */
#define ST25DV64_MAX_ADDR   0x1FFF      /* Zadnja adresa za ST25DV64KC (64 Kbit = 8 KB) */

/* Odabir maksimalne adrese ovisno o korištenom modelu */
#define ST25DV_MAX_ADDR     ST25DV04_MAX_ADDR  /* <-- PROMJENI OVDJE za drugi model */

/* Function prototypes -------------------------------------------------------*/
/**
 * @brief   Inicijalizacija ST25DV uređaja
 * @param   hi2c - Pokazivač na I2C strukturu
 * @retval  HAL_OK ako je inicijalizacija uspjela
 */
HAL_StatusTypeDef ST25DV_Init(I2C_HandleTypeDef *hi2c);
/**
 * @brief   Upis podataka u ST25DV memoriju
 * @param   addr - Početna adresa upisa
 * @param   data - Pokazivač na podatke
 * @param   len - Broj bajtova za upis
 * @param   timeout - Maksimalno vrijeme čekanja u ms
 */
HAL_StatusTypeDef ST25DV_WriteBytes(uint16_t addr, uint8_t *data, uint16_t len, uint32_t timeout);
/**
 * @brief   Čitanje podataka iz ST25DV memorije
 * @param   addr - Početna adresa čitanja
 * @param   rx - Pokazivač na spremnik za primljene podatke
 * @param   len - Broj bajtova za čitanje
 * @param   timeout - Maksimalno vrijeme čekanja u ms
 */
HAL_StatusTypeDef ST25DV_ReadBytes(uint16_t addr, uint8_t *rx, uint16_t len, uint32_t timeout);
/**
 * @brief   Čekanje da ST25DV bude spreman
 * @param   timeout_ms - Maksimalno vrijeme čekanja u ms
 */
HAL_StatusTypeDef ST25DV_WaitReady(uint32_t timeout_ms);

/**
 * @brief   Upis NDEF tekstualnog zapisa
 * @param   text - Tekst za pohranu na NFC tag
 */
HAL_StatusTypeDef ST25DV_WriteNDEFText(const char *text);

/**
 * @brief   Pomoćna funkcija za slanje broja čaša vode
 * @param   casa_vode - Broj čaša za pohranu
 */
void ST25DV_SendCasaCount(uint16_t casa_vode);
#ifdef __cplusplus
}
#endif

#endif /* ST25DV_H */
