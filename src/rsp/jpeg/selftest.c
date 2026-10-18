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
#include "rsp/jpeg/re2.h"
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

/* ---- Video de Resident Evil 2 ----
 *   0x0100  segmento de datos de RESIZE        0x1000  su cuadro de origen (8 filas de 960 bytes)
 *   0x0200  de DECODE                           0x3000  Y, 0x3408 Cb, 0x3610 Cr
 *   0x0300  de FILL                             0x4008  FILL: origen; 0x8000: destino
 *   0x0400  del segundo FILL                    0x4400  su origen; 0x8400: su destino
 *   0x5008  RESIZE: salida (8 filas de 72 bytes cada 80)
 *   0x6008  DECODE: salida (2 pares de filas, geometría del juego: 240 × 4, paso 2112) */
#define V_RZ_DATA 0x0100u
#define V_DC_DATA 0x0200u
#define V_FL_DATA 0x0300u
#define V_FA_DATA 0x0400u

static void d16(uint8_t *ram, uint32_t at, uint32_t v)
{
	salsa64_wr16(ram + at, (uint16_t)v);
}

static void d32(uint8_t *ram, uint32_t at, uint32_t v)
{
	salsa64_wr32(ram + at, v);
}

void re2_selftest_build(uint8_t *ram, uint32_t seed)
{
	uint32_t s = seed;
	memset(ram, 0, RE2_SELFTEST_RAM);
	/* RESIZE: 36 píxeles, 8 filas, pasos de 0,4375 en x y 0,6016 en y, desde la fila 1 con
	 * parte fraccionaria, difuminado con umbral 5. */
	const uint32_t xr = 0x7000, yr = 0x9A00;
	uint8_t *d = ram + V_RZ_DATA;
	d32(d, 0x00, 0x80001000u);
	d32(d, 0x04, 0x80005008u);
	d32(d, 0x08, 36);
	d32(d, 0x0C, 8);
	d32(d, 0x10, xr);
	d32(d, 0x14, yr);
	d32(d, 0x18, 80);
	d32(d, 0x1C, 0x00133290u);
	d32(d, 0x20, 0);
	d32(d, 0x24, 0x00012345u);
	static const uint16_t v29[8] = { 0xFFFF, 0x7FFF, 3, 4, 1, 0xF8, 0x4000, 8 };
	for (unsigned i = 0; i < 8; i++)
		d16(d, 0x40 + 2 * i, v29[i]);
	d16(d, 0x50, 0x80);
	d16(d, 0x52, 7);
	d16(d, 0x54, 0x8000);
	d16(d, 0x56, 0xFF);
	d16(d, 0x58, (8 * xr) >> 16);
	d16(d, 0x5A, (8 * xr) & 0xFFFF);
	d16(d, 0x5E, yr);
	d16(d, 0x60, 960);
	d16(d, 0x62, 5);
	/* El cuadro: un degradado con ruido de 0 a 3 (que el difuminado vea vecinos parecidos). */
	for (unsigned y = 0; y < 8; y++)
		for (unsigned x = 0; x < 320; x++)
			for (unsigned k = 0; k < 3; k++) {
				uint32_t v = 40 + 70 * k + (x * 3 + y * 5) / 8 + (next(&s) & 3);
				ram[0x1000 + y * 960 + 3 * x + k] = (uint8_t)(v > 255 ? 255 : v);
			}
	/* DECODE: 240 × 4, planos aleatorios con Cr y Cb cerca de 128, paso 2112 (salto de 96). */
	d = ram + V_DC_DATA;
	d32(d, 0x00, 0x80003000u);
	d32(d, 0x04, 0x80003408u);
	d32(d, 0x08, 0x80003610u);
	d32(d, 0x0C, 0x80006008u);
	d32(d, 0x10, 240);
	d32(d, 0x14, 4);
	d32(d, 0x18, 2);
	d32(d, 0x1C, 2);
	d32(d, 0x20, 959u | (1u << 12) | (96u << 20));
	d32(d, 0x24, 2112);
	static const uint16_t k[8] = { 0xB375, 0x5B69, 0x2C0D, 0xE2D1, 0x950B, 0x1000, 0x200, 0x80 };
	static const uint16_t v2[8] = { 0x4000, 0x7C00, 1, 0x7F80, 0, 0, 0, 0 };
	for (unsigned i = 0; i < 8; i++) {
		d16(d, 0x40 + 2 * i, k[i]);
		d16(d, 0x50 + 2 * i, v2[i]);
	}
	for (unsigned i = 0; i < 960; i++)
		ram[0x3000 + i] = (uint8_t)(next(&s));
	for (unsigned i = 0; i < 360; i++) {
		ram[0x3408 + i] = (uint8_t)(100 + next(&s) % 56);
		ram[0x3610 + i] = (uint8_t)(100 + next(&s) % 56);
	}
	/* FILL: 3 pasadas de 2 filas de 64 bytes cada 80, origen y destino aleatorios. */
	d = ram + V_FL_DATA;
	d32(d, 0x00, 0x80004008u);
	d32(d, 0x04, 0x80008000u);
	d32(d, 0x08, 128);
	d32(d, 0x0C, 2);
	d32(d, 0x10, 3);
	d32(d, 0x18, 63u | (16u << 20));
	d32(d, 0x1C, 160);
	d32(d, 0x20, 0xF80007C0u);
	d32(d, 0x24, 0x003E0001u);
	d32(d, 0x28, 0x80000800u);
	d32(d, 0x2C, 0x00400002u);
	for (unsigned i = 0; i < 480; i++) {
		ram[0x4008 + i] = (uint8_t)(next(&s));
		ram[0x8000 + i] = (uint8_t)(next(&s));
	}
	/* El segundo FILL, alineado a 16 en origen y destino y con filas de 128 bytes cada 160 (el
	 * SPE usa 16 bytes por instrucción): 2 pasadas de 2 filas. */
	d = ram + V_FA_DATA;
	d32(d, 0x00, 0x80004400u);
	d32(d, 0x04, 0x80008400u);
	d32(d, 0x08, 256);
	d32(d, 0x0C, 2);
	d32(d, 0x10, 2);
	d32(d, 0x18, 127u | (32u << 20));
	d32(d, 0x1C, 320);
	d32(d, 0x20, 0xF80007C0u);
	d32(d, 0x24, 0x003E0001u);
	d32(d, 0x28, 0x80000800u);
	d32(d, 0x2C, 0x00400002u);
	for (unsigned i = 0; i < 640; i++) {
		ram[0x4400 + i] = (uint8_t)(next(&s));
		ram[0x8400 + i] = (uint8_t)(next(&s));
	}
}

uint32_t re2_selftest_task(uint32_t which)
{
	static const uint32_t at[RE2_SELFTEST_TASKS] = { V_RZ_DATA, V_DC_DATA, V_FL_DATA, V_FA_DATA };
	return 0x80000000u | at[which % RE2_SELFTEST_TASKS];
}
