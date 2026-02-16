/*
 * st25dv.c
 *
 *  Created on: Dec 7, 2025
 *      Author: Admin
 */
/**
 * @file    st25dv.c
 * @brief   ST25DV NFC driver
 */

/* Includes ------------------------------------------------------------------*/
#include "st25dv.h"

/* Private variables ---------------------------------------------------------*/
static I2C_HandleTypeDef *hi2c_st25dv = NULL;  /* Pokazivač na I2C strukturu */

/* Private function prototypes -----------------------------------------------*/
static HAL_StatusTypeDef st25dv_write_chunk(uint16_t addr, uint8_t *data,
		uint16_t len, uint32_t timeout);  /* Upis jednog bloka u memoriju */

/* Public functions ----------------------------------------------------------*/

/**
 * @brief   Inicijalizira ST25DV uređaj
 * @param   hi2c - Pokazivač na I2C strukturu
 * @retval  HAL_OK ako je uređaj spreman, inače HAL_ERROR
 */
HAL_StatusTypeDef ST25DV_Init(I2C_HandleTypeDef *hi2c) {
	if (hi2c == NULL) {
		return HAL_ERROR;
	}

	hi2c_st25dv = hi2c;

	// Provjera prisutnosti uređaja na I2C sabirnici
	if (HAL_I2C_IsDeviceReady(hi2c_st25dv, ST25DV_DEV_ADDR, 1, 200) != HAL_OK) {
		return HAL_ERROR;
	}

	return HAL_OK;
}

/**
 * @brief   Upisuje više bajtova u ST25DV memoriju
 * @param   addr - Početna adresa upisa
 * @param   data - Pokazivač na podatke za upis
 * @param   len - Duljina podataka u bajtovima
 * @param   timeout - Maksimalno vrijeme čekanja u ms
 * @retval  HAL_OK ako je upis uspio, inače HAL_ERROR ili HAL_TIMEOUT
 */
HAL_StatusTypeDef ST25DV_WriteBytes(uint16_t addr, uint8_t *data, uint16_t len,
		uint32_t timeout) {
	if (hi2c_st25dv == NULL || data == NULL || len == 0) {
		return HAL_ERROR;
	}

	// Provjera adresnog područja
	if (addr + len - 1 > ST25DV_MAX_ADDR) {
		return HAL_ERROR;
	}

	uint16_t offset = 0;
	while (len > 0) {
		uint16_t chunk_len = (len > CHUNK_MAX) ? CHUNK_MAX : len;

		HAL_StatusTypeDef status = st25dv_write_chunk(addr + offset,
				data + offset, chunk_len, timeout);
		if (status != HAL_OK) {
			return status;
		}

		// Čekanje da EEPROM završi upis
		if (ST25DV_WaitReady(3000) != HAL_OK) {
			return HAL_TIMEOUT;
		}

		offset += chunk_len;
		len -= chunk_len;
	}

	return HAL_OK;
}

/**
 * @brief   Čita više bajtova iz ST25DV memorije
 * @param   addr - Početna adresa čitanja
 * @param   rx - Pokazivač na spremnik za primljene podatke
 * @param   len - Broj bajtova za čitanje
 * @param   timeout - Maksimalno vrijeme čekanja u ms
 * @retval  HAL_OK ako je čitanje uspjelo, inače HAL_ERROR
 */
HAL_StatusTypeDef ST25DV_ReadBytes(uint16_t addr, uint8_t *rx, uint16_t len,
		uint32_t timeout) {
	if (hi2c_st25dv == NULL || rx == NULL || len == 0) {
		return HAL_ERROR;
	}

	uint8_t addrbuf[2];
	addrbuf[0] = (uint8_t) (addr >> 8);    /* Visoki bajt adrese */
	addrbuf[1] = (uint8_t) (addr & 0xFF);  /* Niski bajt adrese */

	// Slanje adrese za čitanje
	if (HAL_I2C_Master_Transmit(hi2c_st25dv, ST25DV_DEV_ADDR, addrbuf, 2,
			timeout) != HAL_OK) {
		return HAL_ERROR;
	}

	// Primanje podataka
	return HAL_I2C_Master_Receive(hi2c_st25dv, ST25DV_DEV_ADDR, rx, len,
			timeout);
}

/**
 * @brief   Čeka da ST25DV bude spreman za novu operaciju
 * @param   timeout_ms - Maksimalno vrijeme čekanja u ms
 * @retval  HAL_OK ako je uređaj spreman, HAL_TIMEOUT ako nije
 */
HAL_StatusTypeDef ST25DV_WaitReady(uint32_t timeout_ms) {
	if (hi2c_st25dv == NULL) {
		return HAL_ERROR;
	}

	uint32_t start = HAL_GetTick();
	while ((HAL_GetTick() - start) < timeout_ms) {
		// I2C adresiranje uspijeva samo kad je EEPROM spreman
		if (HAL_I2C_IsDeviceReady(hi2c_st25dv, ST25DV_DEV_ADDR, 1, 10)
				== HAL_OK) {
			return HAL_OK;
		}
		HAL_Delay(5);
	}
	return HAL_TIMEOUT;
}

/**
 * @brief   Upisuje NDEF tekstualni zapis u ST25DV memoriju
 * @param   text - Tekst za pohranu
 * @retval  HAL_OK ako je upis uspio, inače HAL_ERROR
 */
HAL_StatusTypeDef ST25DV_WriteNDEFText(const char *text) {
	if (text == NULL || hi2c_st25dv == NULL) {
		return HAL_ERROR;
	}

	size_t text_len = strlen(text);
	if (text_len == 0) {
		return HAL_ERROR;
	}

	const char lang[] = "hr";              /* Jezična oznaka */
	uint8_t lang_len = (uint8_t) strlen(lang);
	uint32_t payload_len = 1 + lang_len + (uint32_t) text_len;
	uint8_t SR = (payload_len <= 255) ? 1 : 0;  /* Short Record zastavica */

	// Izračun veličina NDEF zapisa
	uint32_t ndef_record_overhead = 1 + 1 + (SR ? 1 : 4) + 1;
	uint32_t ndef_len = ndef_record_overhead + payload_len;

	// Provjera prostora
	if (NDEF_START_ADDR + ndef_len + 1 > ST25DV_MAX_ADDR) {
		return HAL_ERROR;
	}

	uint8_t header[16];
	uint32_t h = 0;

	// TLV format: Tip
	header[h++] = 0x03;  /* NDEF poruka TLV tip */

	// TLV format: Duljina
	if (ndef_len < 0xFF) {
		header[h++] = (uint8_t) ndef_len;
	} else {
		header[h++] = 0xFF;
		header[h++] = (uint8_t) ((ndef_len >> 8) & 0xFF);
		header[h++] = (uint8_t) (ndef_len & 0xFF);
	}

	// NDEF zaglavlje zapisa
	uint8_t rec_header = 0;
	rec_header |= (1 << 7);  /* MB - početak poruke */
	rec_header |= (1 << 6);  /* ME - kraj poruke */
	if (SR)
		rec_header |= (1 << 4);  /* SR - kratka duljina */
	rec_header |= 0x01;  /* TNF = 0x01 (NFC forum tip) */
	header[h++] = rec_header;

	header[h++] = 0x01;  /* Duljina tipa (T) */

	// Duljina payloada
	if (SR) {
		header[h++] = (uint8_t) payload_len;
	} else {
		header[h++] = (uint8_t) ((payload_len >> 24) & 0xFF);
		header[h++] = (uint8_t) ((payload_len >> 16) & 0xFF);
		header[h++] = (uint8_t) ((payload_len >> 8) & 0xFF);
		header[h++] = (uint8_t) (payload_len & 0xFF);
	}

	header[h++] = 'T';  /* Tip zapisa: tekst */
	header[h++] = (uint8_t) (lang_len & 0x3F);  /* Duljina jezika */

	// Jezična oznaka
	for (uint8_t i = 0; i < lang_len; ++i) {
		header[h++] = (uint8_t) lang[i];
	}

	uint16_t addr = NDEF_START_ADDR;
	HAL_StatusTypeDef st;

	// Upis zaglavlja
	st = ST25DV_WriteBytes(addr, header, h, 100);
	if (st != HAL_OK)
		return st;

	addr += h;

	// Upis teksta u dijelovima
	const uint8_t *tp = (const uint8_t*) text;
	size_t rem = text_len;
	uint8_t chunk_buf[CHUNK_MAX];

	while (rem) {
		uint16_t c = (rem > CHUNK_MAX) ? CHUNK_MAX : (uint16_t) rem;
		memcpy(chunk_buf, tp, c);

		st = ST25DV_WriteBytes(addr, chunk_buf, c, 100);
		if (st != HAL_OK)
			return st;

		addr += c;
		tp += c;
		rem -= c;
	}

	// TLV terminator (kraj)
	uint8_t term = 0xFE;
	st = ST25DV_WriteBytes(addr, &term, 1, 100);
	if (st != HAL_OK)
		return st;

	return HAL_OK;
}

/**
 * @brief   Pomoćna funkcija za slanje broja čaša vode na NFC tag
 * @param   casa_vode - Broj čaša za pohranu
 */
void ST25DV_SendCasaCount(uint16_t casa_vode) {
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "%u čaša vode", casa_vode);
	ST25DV_WriteNDEFText(buffer);
}

/* Private functions ---------------------------------------------------------*/

/**
 * @brief   Upisuje jedan blok podataka u ST25DV memoriju
 * @param   addr - Adresa upisa
 * @param   data - Pokazivač na podatke
 * @param   len - Duljina podataka (maksimalno CHUNK_MAX)
 * @param   timeout - Maksimalno vrijeme čekanja u ms
 * @retval  HAL_OK ako je upis uspio, inače HAL_ERROR
 */
static HAL_StatusTypeDef st25dv_write_chunk(uint16_t addr, uint8_t *data,
		uint16_t len, uint32_t timeout) {
	if (len > CHUNK_MAX || len == 0) {
		return HAL_ERROR;
	}

	uint8_t buf[2 + CHUNK_MAX];
	buf[0] = (uint8_t) (addr >> 8);    /* Visoki bajt adrese */
	buf[1] = (uint8_t) (addr & 0xFF);  /* Niski bajt adrese */
	memcpy(&buf[2], data, len);        /* Kopiranje podataka iza adrese */

	return HAL_I2C_Master_Transmit(hi2c_st25dv, ST25DV_DEV_ADDR, buf, 2 + len,
			timeout);
}
