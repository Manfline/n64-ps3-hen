/*
 * Salsa64 — pruebas del compresor de bloques de lib/spukit (spukit/lz.h,
 * formato de bloque de LZ4): ida y vuelta con distintos tipos de datos,
 * bloques escritos a mano como los de cualquier compresor de LZ4, datos
 * dañados (nada se sale de los búferes) y los encabezados de bloque.
 */
#include "test.h"

#include <string.h>

#include <spukit/lz.h>

static uint8_t in[SPUKIT_LZ_BLOCK + 16], out[SPUKIT_LZ_BOUND(SPUKIT_LZ_BLOCK) + 16], back[SPUKIT_LZ_BLOCK + 16];
static uint16_t table[SPUKIT_LZ_TABLE_ENTRIES];
static uint32_t rng = 0xC0FFEE;

static uint32_t rnd(void)
{
	rng = rng * 1664525u + 1013904223u;
	return rng >> 8;
}

static void fill(unsigned kind, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		switch (kind) {
		case 0: in[i] = (uint8_t)rnd(); break;                         /* sin patrón */
		case 1: in[i] = 0; break;                                      /* ceros */
		case 2: in[i] = (uint8_t)"abcabcabd"[i % 9]; break;            /* período corto */
		case 3: in[i] = i % 3 == 0 ? (uint8_t)rnd() : (uint8_t)(i >> 4); break;
		case 4: in[i] = rnd() % 10 == 0 ? (uint8_t)rnd() : (uint8_t)(i >= 100 ? in[i - 100] : 0); break;
		default: in[i] = rnd() % 4 == 0 ? (uint8_t)(rnd() & 15) : 0; break;  /* casi ceros */
		}
	}
}

static void lz_round_trip(void)
{
	for (unsigned it = 0; it < 600; it++) {
		uint32_t n = it < 40 ? it : it % 7 == 0 ? SPUKIT_LZ_BLOCK : rnd() % (SPUKIT_LZ_BLOCK + 1);
		unsigned kind = rnd() % 6;
		fill(kind, n);
		size_t c = spukit_lz_compress(in, n, out, sizeof out, table);
		CHECK(c != 0);
		if (!c)
			continue;
		memset(back, 0xEE, sizeof back);
		CHECK(spukit_lz_decompress(out, c, back, n));
		CHECK(memcmp(in, back, n) == 0);
		CHECK(back[n] == 0xEE);  /* no escribió de más */
		if (kind == 1 && n > 4000)
			CHECK(c < n / 100 + 16);
	}
}

/* Bloques hechos a mano con la especificación (lo que escribiría cualquier
 * compresor de LZ4): solo literales, una coincidencia solapada de
 * desplazamiento 1, largos que pasan de 15 y de 255. */
static void lz_spec_blocks(void)
{
	static const uint8_t lit[] = { 0x50, 'h', 'o', 'l', 'a', '!' };
	CHECK(spukit_lz_decompress(lit, sizeof lit, back, 5));
	CHECK(memcmp(back, "hola!", 5) == 0);

	/* 'a' y una coincidencia de 4 + 6 = 10 bytes a distancia 1, y 5 literales. */
	static const uint8_t run[] = { 0x16, 'a', 0x01, 0x00, 0x50, 'b', 'c', 'd', 'e', 'f' };
	CHECK(spukit_lz_decompress(run, sizeof run, back, 16));
	CHECK(memcmp(back, "aaaaaaaaaaabcdef", 16) == 0);

	/* 20 literales (15 + 5) seguidos de una coincidencia larga de 4 + 15 + 255 + 1. */
	uint8_t big[64];
	uint32_t k = 0;
	big[k++] = 0xFF;
	big[k++] = 5;
	for (int i = 0; i < 20; i++)
		big[k++] = (uint8_t)('A' + i);
	big[k++] = 20;  /* distancia: toda la tira */
	big[k++] = 0;
	big[k++] = 255;  /* 4 + 15 + 255 + 3 */
	big[k++] = 3;
	big[k++] = 0x50;
	for (int i = 0; i < 5; i++)
		big[k++] = 'z';
	uint32_t n = 20 + 4 + 15 + 255 + 3 + 5;
	CHECK(spukit_lz_decompress(big, k, back, n));
	for (uint32_t i = 20; i < n - 5; i++)
		CHECK_EQ(back[i], back[i - 20]);
	CHECK(memcmp(back + n - 5, "zzzzz", 5) == 0);
}

static void lz_corrupt(void)
{
	fill(4, 40000);
	size_t c = spukit_lz_compress(in, 40000, out, sizeof out, table);
	CHECK(c > 100);
	/* Tamaño equivocado, cortado, desplazamiento 0 o más lejos que lo escrito. */
	CHECK(!spukit_lz_decompress(out, c, back, 39999));
	CHECK(!spukit_lz_decompress(out, c, back, 40001));
	CHECK(!spukit_lz_decompress(out, c - 3, back, 40000));
	CHECK(!spukit_lz_decompress(out, 0, back, 5));
	static const uint8_t off0[] = { 0x10, 'a', 0x00, 0x00, 0x50, 'b', 'c', 'd', 'e', 'f' };
	CHECK(!spukit_lz_decompress(off0, sizeof off0, back, 16));
	static const uint8_t far[] = { 0x10, 'a', 0x09, 0x00, 0x50, 'b', 'c', 'd', 'e', 'f' };
	CHECK(!spukit_lz_decompress(far, sizeof far, back, 16));
	/* Un bit cambiado en cualquier byte: nunca se sale del búfer (ASan lo vigila). */
	for (unsigned k = 0; k < 2000; k++) {
		uint32_t at = rnd() % c;
		uint8_t save = out[at];
		out[at] ^= (uint8_t)(1u << (rnd() & 7));
		spukit_lz_decompress(out, c, back, 40000);
		out[at] = save;
	}
}

static void lz_frames(void)
{
	uint8_t h[4];
	spukit_lz_frame(h, 65536, 0);
	CHECK_EQ(spukit_lz_frame_len(h), 65536);
	CHECK(!spukit_lz_frame_stored(h));
	spukit_lz_frame(h, 1234, 1);
	CHECK_EQ(spukit_lz_frame_len(h), 1234);
	CHECK(spukit_lz_frame_stored(h));
	CHECK_EQ(h[0], 0x80);
	/* Sin patrón no gana: lo decide quien llama; el compresor no se pasa de la cota. */
	fill(0, SPUKIT_LZ_BLOCK);
	size_t c = spukit_lz_compress(in, SPUKIT_LZ_BLOCK, out, sizeof out, table);
	CHECK(c != 0 && c <= SPUKIT_LZ_BOUND(SPUKIT_LZ_BLOCK));
	/* Capacidad menor que la cota: no escribe nada. */
	CHECK_EQ(spukit_lz_compress(in, 1000, out, 900, table), 0);
}

void t_suite_lz(void)
{
	RUN(lz_round_trip);
	RUN(lz_spec_blocks);
	RUN(lz_corrupt);
	RUN(lz_frames);
}
