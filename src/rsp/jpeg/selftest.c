/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — tareas JPEG de prueba. Ver selftest.h.
 *
 *   0x000  tarea 0: modo 2, JPEG_SELFTEST_MBS macrobloques en 0x200
 *   0x028  tarea 1: modo 0, los suyos detrás (alineada a 8, no a 16: el
 *          SPE la trae con spukit_get_unaligned)
 *   0x048  tablas de cuantización Y, U, V (128 bytes cada una, a 8)
 *   0x200  macrobloques
 */
#include <string.h>

#include "core/endian.h"
#include "rsp/jpeg/jpeg.h"
#include "rsp/jpeg/selftest.h"

#define Q_ADDR  0x048u
#define MB_ADDR 0x200u

static const uint32_t task_addr[2] = { 0x000u, 0x028u };

static uint32_t next(uint32_t *s)
{
	*s = *s * 1103515245u + 12345u;
	return *s >> 8;
}

/* Un coeficiente con amplitud que baja con la frecuencia, como en un JPEG
 * real; en los macrobloques «extremos», valores que saturan al
 * multiplicar por la cuantización y desbordan al ×16. */
static int16_t coef(uint32_t *s, unsigned i, bool extreme)
{
	uint32_t r = next(s);
	if (extreme) {
		static const int16_t big[4] = { 32767, -32768, 2047, -2048 };
		return r % 3 ? big[r >> 4 & 3] : (int16_t)(r % 4001 - 2000);
	}
	int amp = i == 0 ? 120 : 60 / (1 + (int)i / 4);
	if (i && r % 4 == 0)
		return 0;  /* muchos ceros, como tras cuantizar */
	return (int16_t)((int)(r % (2u * amp + 1)) - amp);
}

static void task(uint8_t *ram, uint32_t at, uint32_t addr, uint32_t mode)
{
	salsa64_wr32(ram + at, 0x80000000u | addr);
	salsa64_wr32(ram + at + 4, JPEG_SELFTEST_MBS);
	salsa64_wr32(ram + at + 8, mode);
	for (int t = 0; t < 3; t++)
		salsa64_wr32(ram + at + 12 + 4 * t, 0x80000000u | (Q_ADDR + 128 * t));
}

void jpeg_selftest_build(uint8_t *ram, uint32_t seed)
{
	uint32_t s = seed;
	memset(ram, 0, JPEG_SELFTEST_RAM);
	uint32_t addr1 = MB_ADDR + JPEG_SELFTEST_MBS * SALSA64_JPEG_MB_IN(2);
	task(ram, task_addr[0], MB_ADDR, 2);
	task(ram, task_addr[1], addr1, 0);
	for (int t = 0; t < 3; t++)
		for (int i = 0; i < 64; i++) {
			uint32_t r = next(&s);
			/* De 1 a 40, y alguna grande (satura con coeficientes grandes). */
			salsa64_wr16(ram + Q_ADDR + 128 * t + 2 * i, (uint16_t)(r % 23 == 0 ? 255 : 1 + r % 40));
		}
	for (uint32_t k = 0; k < 2; k++) {
		uint32_t mode = k ? 0 : 2, base = k ? addr1 : MB_ADDR, blocks = mode + 4;
		for (uint32_t m = 0; m < JPEG_SELFTEST_MBS; m++)
			for (uint32_t b = 0; b < blocks; b++)
				for (unsigned i = 0; i < 64; i++) {
					uint8_t *p = ram + base + m * SALSA64_JPEG_MB_IN(mode) + b * 128 + 2 * i;
					salsa64_wr16(p, (uint16_t)coef(&s, i, m == JPEG_SELFTEST_MBS - 1 - k));
				}
	}
}

uint32_t jpeg_selftest_task(uint32_t which)
{
	return 0x80000000u | task_addr[which & 1];
}
