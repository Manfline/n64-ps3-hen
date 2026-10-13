/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — qué microcódigo JPEG trae una tarea (por el CRC-32 de su
 * código). Aparte de jpeg.c, que también se compila para el SPE.
 */
#include "core/rom.h"
#include "rsp/jpeg/jpeg.h"

/* Microcódigo de Zelda OoT, Pokémon Stadium y Pokémon Stadium 2 (sin
 * texto de identificación): el mismo código, de 0xAF0 bytes (termina en
 * `jr ra` y `mtc0 $0, SP_SEMAPHORE`). La tarea pide 4 KB (ucode_size) y lo
 * que sigue al código es lo que el juego tenga detrás en la RDRAM: distinto
 * en cada juego. Se reconoce por el CRC-32 del código. */
#define PS_UCODE_CODE 0xAF0u
#define PS_UCODE_CRC  0xF055B8ABu

enum salsa64_jpeg_kind salsa64_jpeg_detect(const uint8_t *ucode, uint32_t size)
{
	if (size >= PS_UCODE_CODE && salsa64_crc32(ucode, PS_UCODE_CODE) == PS_UCODE_CRC)
		return SALSA64_JPEG_OOT;
	return SALSA64_JPEG_NONE;
}
