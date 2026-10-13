/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — qué microcódigo JPEG trae una tarea (por el CRC-32 de su
 * código). Aparte de jpeg.c, que también se compila para el SPE.
 */
#include "core/rom.h"
#include "rsp/jpeg/jpeg.h"

/* Microcódigo de Zelda OoT (4 KB, sin texto de identificación): CRC-32. */
#define OOT_UCODE_CRC  0x761D8504u
#define OOT_UCODE_SIZE 0x1000u

enum salsa64_jpeg_kind salsa64_jpeg_detect(const uint8_t *ucode, uint32_t size)
{
	if (size == OOT_UCODE_SIZE && salsa64_crc32(ucode, size) == OOT_UCODE_CRC)
		return SALSA64_JPEG_OOT;
	return SALSA64_JPEG_NONE;
}
