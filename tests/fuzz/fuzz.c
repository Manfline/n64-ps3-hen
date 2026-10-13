/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — fuzzing corto de lo que el frontend lee de archivos ajenos: el
 * decodificador de PNG de las carátulas (cover.c), los .cht y los activos
 * de los trucos (cheats.c, también aplicados a una RDRAM), el índice y la
 * configuración de las descargas (dlutil.c) y la búsqueda por nombre
 * parecido (namematch.c). Sin libFuzzer: un mutador propio con semilla fija
 * (se repite igual) sobre las semillas de semillas.h, más casos dirigidos
 * (cada truncado de cada PNG, longitudes de trozo enormes, cabecera zlib y
 * CRC dañados). Cada entrada va en un búfer de su tamaño exacto, sin '\0'
 * al final, para que ASan vea cualquier lectura de más.
 *
 *     tools/build.sh -C tests/fuzz run        (ASan + UBSan, ~1 min)
 *     tools/build.sh -C tests/fuzz run N=2000000 SEED=7
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/bus.h"
#include "frontend/cheats.h"
#include "frontend/cover.h"
#include "frontend/dlutil.h"
#include "frontend/namematch.h"

#include "semillas.h"

#define NSEEDS(a) (sizeof a / sizeof a[0])
#define MAX_IN    (1u << 20)

/* cheats.c avisa al bus de lo que escribe: aquí solo se cuenta. */
static unsigned long poked;
void salsa64_bus_rdram_patched(struct salsa64_bus *bus, uint32_t pa, uint32_t len)
{
	(void)bus;
	if (pa + len > 8u << 20)
		abort();
	poked++;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint32_t)(rng >> 16);
}

static uint8_t work[MAX_IN];

/* Una mutación sobre work[0..n) (n puede crecer hasta MAX_IN). */
static size_t mutate(size_t n, const struct seed *pool, size_t npool)
{
	static const uint32_t big[] = { 0, 1, 0x7F, 0x80, 0xFF, 0xFFFF, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFF4u };
	uint32_t pos = n ? rnd() % (uint32_t)n : 0;
	switch (rnd() % 9) {
	case 0:  /* un bit */
		if (n)
			work[pos] ^= (uint8_t)(1u << (rnd() & 7));
		break;
	case 1:  /* un byte cualquiera o uno de los de borde */
		if (n)
			work[pos] = rnd() & 1 ? (uint8_t)rnd() : (uint8_t)big[rnd() % 5];
		break;
	case 2:  /* 32 bits big-endian enormes (longitudes de trozo, anchos) */
		if (n >= 4) {
			uint32_t v = big[rnd() % NSEEDS(big)], p = rnd() % (uint32_t)(n - 3);
			work[p] = (uint8_t)(v >> 24);
			work[p + 1] = (uint8_t)(v >> 16);
			work[p + 2] = (uint8_t)(v >> 8);
			work[p + 3] = (uint8_t)v;
		}
		break;
	case 3:  /* truncar */
		n = n ? rnd() % (uint32_t)n : 0;
		break;
	case 4:  /* borrar un tramo */
		if (n) {
			size_t len = rnd() % (n - pos) + 1;
			memmove(work + pos, work + pos + len, n - pos - len);
			n -= len;
		}
		break;
	case 5:  /* duplicar un tramo */
		if (n) {
			size_t len = rnd() % (n - pos) % 256 + 1;
			if (n + len <= MAX_IN) {
				memmove(work + pos + len, work + pos, n - pos);
				n += len;
			}
		}
		break;
	case 6:  /* meter bytes al azar */
		{
			size_t len = rnd() % 32 + 1;
			if (n + len <= MAX_IN) {
				memmove(work + pos + len, work + pos, n - pos);
				for (size_t i = 0; i < len; i++)
					work[pos + i] = (uint8_t)rnd();
				n += len;
			}
		}
		break;
	case 7:  /* empalmar el final de otra semilla */
		{
			const struct seed *o = &pool[rnd() % npool];
			size_t from = o->n ? rnd() % o->n : 0, len = o->n - from;
			if (pos + len <= MAX_IN) {
				memcpy(work + pos, o->d + from, len);
				n = pos + len;
			}
		}
		break;
	default:  /* caracteres que los lectores de texto miran */
		if (n) {
			static const char special[] = "\n\r\t =+\"$[]-?\\:{}/.\0";
			work[pos] = (uint8_t)special[rnd() % (sizeof special - 1)];
		}
		break;
	}
	return n;
}

static uint8_t *exact(const uint8_t *d, size_t n)
{
	uint8_t *p = malloc(n ? n : 1);
	if (!p)
		abort();
	memcpy(p, d, n);
	return p;
}

/* ---- Objetivos ---- */

static uint8_t rgba[COVER_RGBA_BYTES];
static unsigned long png_ok, png_bad;

static void t_png(const uint8_t *d, size_t n)
{
	uint8_t *p = exact(d, n);
	unsigned w = 0, h = 0;
	enum cover_result r = cover_decode_png(p, n, rgba, &w, &h);
	if (r == COVER_OK) {
		if (!w || !h || w > COVER_MAX_W || h > COVER_MAX_H)
			abort();
		png_ok++;
	} else {
		png_bad++;
	}
	free(p);
}

static struct cheat_set cs;
static uint8_t rdram[8u << 20];
static struct salsa64_bus bus;
static char selbuf[1u << 17];

static void t_cht(const uint8_t *d, size_t n)
{
	uint8_t *p = exact(d, n);
	if (rnd() & 1)
		cheats_parse(&cs, (const char *)p, n);
	else
		cheats_parse_pj64(&cs, (const char *)p, n, rnd() & 1 ? 0x11111111u : rnd(), 0x22222222u);
	if (cs.n > CHEATS_MAX || cs.nlines > CHEATS_LINES_MAX)
		abort();
	for (unsigned k = 0; k < cs.n; k++) {
		if ((uint32_t)cs.c[k].first + cs.c[k].count > cs.nlines || !memchr(cs.c[k].desc, 0, sizeof cs.c[k].desc))
			abort();
		cs.c[k].on = rnd() & 1;
	}
	cheats_apply(&cs, &bus);
	int len = cheats_sel_format(&cs, selbuf, sizeof selbuf);
	if (len > 0) {
		uint8_t *s = exact((const uint8_t *)selbuf, (size_t)len);
		cheats_sel_parse(&cs, (const char *)s, (size_t)len);
		free(s);
	}
	free(p);
}

static void t_sel(const uint8_t *d, size_t n)
{
	uint8_t *p = exact(d, n);
	cheats_sel_parse(&cs, (const char *)p, n);
	free(p);
}

static char names[MAX_IN + 1];

static void t_idx(const uint8_t *d, size_t n)
{
	uint8_t *p = exact(d, n);
	size_t used = dl_index_names((const char *)p, n, rnd() & 1 ? ".png" : ".cht", names, n + 1);
	if (used > n || names[used] != '\0')
		abort();
	for (size_t i = 0; i < used; i++)
		if (names[i] == '/')
			abort();  /* un nombre del índice nunca trae carpetas */
	char found[256];
	if (name_find_list(names, used, ".png", "Juego de prueba (U).z64", "JUEGO DE PRUEBA", "NJPE", 4, found, sizeof found) &&
	    strchr(found, '/'))
		abort();
	struct dl_cfg cfg;
	dl_cfg_default(&cfg);
	dl_cfg_parse(&cfg, (const char *)p, n);
	if (!memchr(cfg.covers, 0, sizeof cfg.covers) || !memchr(cfg.cheats_index, 0, sizeof cfg.cheats_index))
		abort();
	char esc[600];
	dl_url_escape(n ? (const char *)names : "", esc, sizeof esc);
	free(p);
}

/* ---- Casos dirigidos de PNG ---- */

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void png_directed(void)
{
	static const uint32_t lens[] = { 0, 1, 13, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFF4u };
	for (size_t s = 0; s < NSEEDS(png_seeds); s++) {
		const struct seed *sd = &png_seeds[s];
		/* Cada truncado. */
		for (size_t n = 0; n <= sd->n; n++)
			t_png(sd->d, n);
		/* La longitud de cada trozo, enorme o mal puesta; su CRC dañado. */
		for (size_t pos = 8; pos + 12 <= sd->n;) {
			uint32_t len = be32(sd->d + pos);
			memcpy(work, sd->d, sd->n);
			for (size_t k = 0; k < NSEEDS(lens); k++) {
				work[pos] = (uint8_t)(lens[k] >> 24);
				work[pos + 1] = (uint8_t)(lens[k] >> 16);
				work[pos + 2] = (uint8_t)(lens[k] >> 8);
				work[pos + 3] = (uint8_t)lens[k];
				t_png(work, sd->n);
			}
			memcpy(work, sd->d, sd->n);
			work[pos + 8 + len] ^= 0xA5;  /* CRC (el decodificador no lo mira: no debe importar) */
			t_png(work, sd->n);
			if (!memcmp(sd->d + pos + 4, "IDAT", 4)) {
				/* zlib: método, FDICT y la suma de la cabecera; cada byte del flujo. */
				for (int b = 0; b < 2; b++)
					for (int v = 0; v < 256; v += 17) {
						memcpy(work, sd->d, sd->n);
						work[pos + 8 + (size_t)b] = (uint8_t)v;
						t_png(work, sd->n);
					}
				for (size_t z = pos + 8; z < pos + 8 + len; z++) {
					memcpy(work, sd->d, sd->n);
					work[z] ^= (uint8_t)(1u << (z & 7));
					t_png(work, sd->n);
				}
			}
			pos += 12 + (size_t)len;
		}
	}
}

/* ---- Bucle ---- */

static void campaign(const char *name, const struct seed *pool, size_t npool, void (*target)(const uint8_t *, size_t),
		     unsigned long iters)
{
	for (size_t s = 0; s < npool; s++)
		target(pool[s].d, pool[s].n);
	for (unsigned long i = 0; i < iters; i++) {
		const struct seed *sd = &pool[rnd() % npool];
		size_t n = sd->n;
		memcpy(work, sd->d, n);
		for (unsigned m = 1 + rnd() % 6; m; m--)
			n = mutate(n, pool, npool);
		target(work, n);
	}
	printf("fuzz %-4s %lu entradas\n", name, iters + npool);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	unsigned long n = argc > 1 ? strtoul(argv[1], NULL, 0) : 200000;
	if (argc > 2)
		rng ^= strtoull(argv[2], NULL, 0) * 0x2545F4914F6CDD1Dull;
	bus.rdram = rdram;  /* sin salsa64_bus_init: el fuzzer no enlaza el núcleo */
	bus.rdram_size = sizeof rdram;

	png_directed();
	printf("fuzz png  dirigidos: %lu bien, %lu rechazados\n", png_ok, png_bad);
	campaign("png", png_seeds, NSEEDS(png_seeds), t_png, n);
	printf("fuzz png  %lu bien, %lu rechazados en total\n", png_ok, png_bad);
	campaign("cht", cht_seeds, NSEEDS(cht_seeds), t_cht, n);
	campaign("sel", sel_seeds, NSEEDS(sel_seeds), t_sel, n / 4);
	campaign("idx", idx_seeds, NSEEDS(idx_seeds), t_idx, n);
	printf("fuzz: sin fallos (%lu escrituras de trucos en la RDRAM)\n", poked);
	return 0;
}
