/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Pruebas de las carátulas (src/frontend/cover.c): PNG armados aquí con
 * bloques «stored» de deflate (el decodificador no mira los CRC), todos los
 * tipos de color, entrelazado, reducción y el buscador de archivos.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "frontend/cover.h"
#include "test.h"

static uint8_t rgba[COVER_RGBA_BYTES];
static unsigned gw, gh;

static size_t put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
	return 4;
}

static size_t chunk(uint8_t *out, const char *type, const uint8_t *body, size_t n)
{
	size_t o = put32(out, (uint32_t)n);
	memcpy(out + o, type, 4);
	o += 4;
	if (n)
		memcpy(out + o, body, n);
	o += n;
	memset(out + o, 0, 4);  /* CRC: no se mira */
	return o + 4;
}

struct spec {
	unsigned w, h;
	int depth, ctype, interlace;
	const uint8_t *plte;
	size_t nplte;  /* bytes */
	const uint8_t *trns;
	size_t ntrns;
};

/* Devuelve el PNG (malloc) con `raw` (ya con sus bytes de filtro) en bloques stored. */
static uint8_t *mkpng(const struct spec *s, const uint8_t *raw, size_t rawn, size_t *out_n)
{
	size_t cap = rawn + rawn / 60000 * 5 + 1024 + s->nplte + s->ntrns;
	uint8_t *png = malloc(cap), *z = malloc(rawn + rawn / 60000 * 5 + 64);
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	size_t o = 0, zn = 0;
	memcpy(png, sig, 8);
	o = 8;
	uint8_t ih[13];
	put32(ih, s->w);
	put32(ih + 4, s->h);
	ih[8] = (uint8_t)s->depth;
	ih[9] = (uint8_t)s->ctype;
	ih[10] = ih[11] = 0;
	ih[12] = (uint8_t)s->interlace;
	o += chunk(png + o, "IHDR", ih, 13);
	if (s->nplte)
		o += chunk(png + o, "PLTE", s->plte, s->nplte);
	if (s->ntrns)
		o += chunk(png + o, "tRNS", s->trns, s->ntrns);
	z[zn++] = 0x78;
	z[zn++] = 0x01;
	size_t done = 0;
	do {
		size_t n = rawn - done > 60000 ? 60000 : rawn - done;
		z[zn++] = done + n == rawn ? 1 : 0;
		z[zn++] = (uint8_t)n;
		z[zn++] = (uint8_t)(n >> 8);
		z[zn++] = (uint8_t)~n;
		z[zn++] = (uint8_t)(~n >> 8);
		memcpy(z + zn, raw + done, n);
		zn += n;
		done += n;
	} while (done < rawn);
	memset(z + zn, 0, 4);  /* Adler: no se mira */
	zn += 4;
	/* En dos IDAT, para probar que se juntan. */
	size_t half = zn / 2;
	o += chunk(png + o, "IDAT", z, half);
	o += chunk(png + o, "IDAT", z + half, zn - half);
	o += chunk(png + o, "IEND", NULL, 0);
	free(z);
	*out_n = o;
	return png;
}

static enum cover_result run(const struct spec *s, const uint8_t *raw, size_t rawn)
{
	size_t n;
	uint8_t *png = mkpng(s, raw, rawn, &n);
	memset(rgba, 0xEE, 64);
	enum cover_result r = cover_decode_png(png, n, rgba, &gw, &gh);
	free(png);
	return r;
}

#define PX(i, r, g, b, a)                                                               \
	CHECK(rgba[(i) * 4] == (r) && rgba[(i) * 4 + 1] == (g) && rgba[(i) * 4 + 2] == (b) && \
	      rgba[(i) * 4 + 3] == (a))

static void color_types(void)
{
	/* RGB 8 bits, sin filtro. */
	struct spec s = { .w = 2, .h = 1, .depth = 8, .ctype = 2 };
	static const uint8_t rgb[] = { 0, 255, 0, 0, 0, 0, 255 };
	CHECK_EQ(run(&s, rgb, 7), COVER_OK);
	CHECK(gw == 2 && gh == 1);
	PX(0, 255, 0, 0, 255);
	PX(1, 0, 0, 255, 255);
	/* RGBA 8 bits. */
	s.ctype = 6;
	static const uint8_t rgba8[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
	CHECK_EQ(run(&s, rgba8, 9), COVER_OK);
	PX(0, 1, 2, 3, 4);
	PX(1, 5, 6, 7, 8);
	/* Gris 8 y gris con alfa. */
	s.ctype = 0;
	static const uint8_t g8[] = { 0, 40, 200 };
	CHECK_EQ(run(&s, g8, 3), COVER_OK);
	PX(0, 40, 40, 40, 255);
	PX(1, 200, 200, 200, 255);
	s.ctype = 4;
	static const uint8_t ga[] = { 0, 9, 99, 8, 88 };
	CHECK_EQ(run(&s, ga, 5), COVER_OK);
	PX(0, 9, 9, 9, 99);
	PX(1, 8, 8, 8, 88);
	/* Gris de 16 bits: manda el byte alto. */
	s.ctype = 0;
	s.depth = 16;
	static const uint8_t g16[] = { 0, 0x80, 0xFF, 0x12, 0x34 };
	CHECK_EQ(run(&s, g16, 5), COVER_OK);
	PX(0, 0x80, 0x80, 0x80, 255);
	PX(1, 0x12, 0x12, 0x12, 255);
	/* Gris de 1 bit: dos píxeles en un byte. */
	s.depth = 1;
	static const uint8_t g1[] = { 0, 0x40 };  /* 0 1 */
	CHECK_EQ(run(&s, g1, 2), COVER_OK);
	PX(0, 0, 0, 0, 255);
	PX(1, 255, 255, 255, 255);
	/* Paleta de 8 y de 2 bits, con tRNS. */
	static const uint8_t pal[] = { 255, 0, 0, 0, 255, 0, 0, 0, 255 };
	static const uint8_t trn[] = { 255, 128 };
	s.ctype = 3;
	s.depth = 8;
	s.plte = pal;
	s.nplte = sizeof pal;
	s.trns = trn;
	s.ntrns = sizeof trn;
	static const uint8_t p8[] = { 0, 1, 2 };
	CHECK_EQ(run(&s, p8, 3), COVER_OK);
	PX(0, 0, 255, 0, 128);
	PX(1, 0, 0, 255, 255);  /* sin entrada en tRNS: opaco */
	s.depth = 2;
	static const uint8_t p2[] = { 0, 0x60 };  /* 01 10 */
	CHECK_EQ(run(&s, p2, 2), COVER_OK);
	PX(0, 0, 255, 0, 128);
	PX(1, 0, 0, 255, 255);
	/* RGB con color transparente (tRNS de 6 bytes). */
	static const uint8_t key[] = { 0, 0, 0, 0, 0, 255 };
	s = (struct spec){ .w = 2, .h = 1, .depth = 8, .ctype = 2, .trns = key, .ntrns = 6 };
	static const uint8_t k[] = { 0, 0, 0, 255, 5, 6, 7 };
	CHECK_EQ(run(&s, k, 7), COVER_OK);
	PX(0, 0, 0, 255, 0);
	PX(1, 5, 6, 7, 255);
}

static void filters(void)
{
	/* Cuatro filas de 2 píxeles de gris con filtro Sub, Up, Average y Paeth. */
	struct spec s = { .w = 2, .h = 4, .depth = 8, .ctype = 0 };
	static const uint8_t raw[] = {
		1, 10, 5,    /* Sub:     10, 15 */
		2, 1, 1,     /* Up:      11, 16 */
		3, 1, 0,     /* Average: 1+(0+11)/2 = 6 ; 0+(6+16)/2 = 11 */
		4, 1, 1,     /* Paeth */
	};
	CHECK_EQ(run(&s, raw, sizeof raw), COVER_OK);
	CHECK_EQ(rgba[0], 10);
	CHECK_EQ(rgba[4], 15);
	CHECK_EQ(rgba[8], 11);
	CHECK_EQ(rgba[12], 16);
	CHECK_EQ(rgba[16], 6);
	CHECK_EQ(rgba[20], 11);
	/* Paeth con a = 0, b = 6, c = 0 → predice b (6): 1 + 6 = 7. Segundo: a = 7, b = 11, c = 6 → p = 12, pa = 5, pb = 1, pc = 6 → b: 1 + 11 = 12. */
	CHECK_EQ(rgba[24], 7);
	CHECK_EQ(rgba[28], 12);
	/* Un filtro que no existe. */
	static const uint8_t bad[] = { 9, 1, 2 };
	s.h = 1;
	CHECK_EQ(run(&s, bad, 3), COVER_BAD);
}

static const struct { unsigned x0, y0, dx, dy; } a7[7] = {
	{ 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 }, { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 },
};

static void interlaced(void)
{
	/* 5×5 de gris 8 con valor = 10·y + x, entrelazado a mano. */
	enum { W = 5, H = 5 };
	uint8_t raw[7 * H * (W + 1)];
	size_t n = 0;
	for (int k = 0; k < 7; k++) {
		if (a7[k].x0 >= W || a7[k].y0 >= H)
			continue;
		unsigned pw = (W - a7[k].x0 + a7[k].dx - 1) / a7[k].dx;
		for (unsigned y = a7[k].y0; y < H; y += a7[k].dy) {
			raw[n++] = 0;
			for (unsigned i = 0; i < pw; i++)
				raw[n++] = (uint8_t)(10 * y + a7[k].x0 + i * a7[k].dx);
		}
	}
	struct spec s = { .w = W, .h = H, .depth = 8, .ctype = 0, .interlace = 1 };
	CHECK_EQ(run(&s, raw, n), COVER_OK);
	CHECK(gw == W && gh == H);
	int ok = 1;
	for (unsigned y = 0; y < H; y++)
		for (unsigned x = 0; x < W; x++)
			ok &= rgba[(y * W + x) * 4] == 10 * y + x;
	CHECK(ok);
}

static void downscale(void)
{
	/* 640×896 gris: columnas pares 0 e impares 200 → cada píxel final, 100. */
	enum { W = 640, H = 896 };
	size_t rb = W + 1;
	uint8_t *raw = malloc(rb * H);
	for (unsigned y = 0; y < H; y++) {
		raw[y * rb] = 0;
		for (unsigned x = 0; x < W; x++)
			raw[y * rb + 1 + x] = x & 1 ? 200 : 0;
	}
	struct spec s = { .w = W, .h = H, .depth = 8, .ctype = 0 };
	CHECK_EQ(run(&s, raw, rb * H), COVER_OK);
	CHECK(gw == 320 && gh == 448);
	int ok = 1;
	for (size_t i = 0; i < (size_t)gw * gh; i++)
		ok &= rgba[i * 4] == 100 && rgba[i * 4 + 3] == 255;
	CHECK(ok);
	free(raw);
	/* Ancha: manda el ancho y se conserva la proporción (800×200 → 320×80). */
	raw = calloc(801, 200);
	s.w = 800;
	s.h = 200;
	CHECK_EQ(run(&s, raw, 801u * 200), COVER_OK);
	CHECK(gw == 320 && gh == 80);
	free(raw);
	/* Chica: se queda como está. */
	uint8_t one[] = { 0, 77 };
	s = (struct spec){ .w = 1, .h = 1, .depth = 8, .ctype = 0 };
	CHECK_EQ(run(&s, one, 2), COVER_OK);
	CHECK(gw == 1 && gh == 1);
}

static void errors(void)
{
	uint8_t junk[64] = "esto no es un PNG, ni de lejos, ni por casualidad.......";
	CHECK_EQ(cover_decode_png(junk, sizeof junk, rgba, &gw, &gh), COVER_BAD);
	CHECK_EQ(cover_decode_png(junk, 3, rgba, &gw, &gh), COVER_BAD);
	/* Profundidad que no va con el tipo. */
	struct spec s = { .w = 1, .h = 1, .depth = 3, .ctype = 2 };
	uint8_t raw[8] = { 0 };
	CHECK_EQ(run(&s, raw, 4), COVER_UNSUPPORTED);
	s = (struct spec){ .w = 1, .h = 1, .depth = 16, .ctype = 3 };
	CHECK_EQ(run(&s, raw, 3), COVER_UNSUPPORTED);
	/* Paleta sin PLTE. */
	s = (struct spec){ .w = 1, .h = 1, .depth = 8, .ctype = 3 };
	CHECK_EQ(run(&s, raw, 2), COVER_BAD);
	/* Datos de menos o de más. */
	s = (struct spec){ .w = 2, .h = 2, .depth = 8, .ctype = 0 };
	CHECK_EQ(run(&s, raw, 5), COVER_BAD);
	CHECK_EQ(run(&s, raw, 8), COVER_BAD);
	/* Enorme: se rechaza antes de pedir memoria. */
	s = (struct spec){ .w = 65535, .h = 65535, .depth = 8, .ctype = 6 };
	CHECK_EQ(run(&s, raw, 2), COVER_TOO_BIG);
	/* PNG cortado a la mitad: nunca se sale del buffer (ASAN). */
	s = (struct spec){ .w = 4, .h = 4, .depth = 8, .ctype = 2 };
	uint8_t body[4 * 13];
	memset(body, 0, sizeof body);
	size_t n;
	uint8_t *png = mkpng(&s, body, sizeof body, &n);
	for (size_t cut = 0; cut < n; cut += 3) {
		enum cover_result r = cover_decode_png(png, cut, rgba, &gw, &gh);
		CHECK(r != COVER_OK || cut >= n - 12);
	}
	CHECK_EQ(cover_decode_png(png, n, rgba, &gw, &gh), COVER_OK);
	/* Un bit cambiado en cada posición del cuerpo: puede fallar, pero no romper. */
	for (size_t i = 8; i < n; i++) {
		png[i] ^= 0x5A;
		(void)cover_decode_png(png, n, rgba, &gw, &gh);
		png[i] ^= 0x5A;
	}
	free(png);
	CHECK(!strcmp(cover_result_name(COVER_OK), cover_result_name(COVER_OK)));
	CHECK(cover_result_name(COVER_BAD) != NULL && cover_result_name(COVER_TOO_BIG)[0]);
}

static void put_file(const char *path, const void *data, size_t n)
{
	FILE *f = fopen(path, "wb");
	CHECK(f != NULL);
	if (f) {
		fwrite(data, 1, n, f);
		fclose(f);
	}
}

static void files(void)
{
	char dir[] = "/tmp/salsa64-covers-XXXXXX";
	CHECK(mkdtemp(dir) != NULL);
	char path[300], found[300];
	CHECK(!cover_find(dir, "Star Fox 64 (U).z64", NULL, "NFXE", found, sizeof found));
	CHECK_EQ(cover_load_png(dir, rgba, &gw, &gh), COVER_BAD);  /* un directorio */
	snprintf(path, sizeof path, "%s/falta.png", dir);
	CHECK_EQ(cover_load_png(path, rgba, &gw, &gh), COVER_NO_FILE);

	struct spec s = { .w = 1, .h = 1, .depth = 8, .ctype = 0 };
	uint8_t one[] = { 0, 123 };
	size_t n;
	uint8_t *png = mkpng(&s, one, 2, &n);
	/* Por el código del juego, sin importar mayúsculas. */
	snprintf(path, sizeof path, "%s/nfxe.PNG", dir);
	put_file(path, png, n);
	CHECK(cover_find(dir, "Star Fox 64 (U).z64", NULL, "NFXE", found, sizeof found));
	CHECK(!strcmp(found, path));
	CHECK_EQ(cover_load_png(found, rgba, &gw, &gh), COVER_OK);
	CHECK(gw == 1 && rgba[0] == 123);
	/* El nombre de la ROM gana al código. */
	snprintf(path, sizeof path, "%s/Star Fox 64 (U).png", dir);
	put_file(path, png, n);
	CHECK(cover_find(dir, "Star Fox 64 (U).z64", NULL, "NFXE", found, sizeof found));
	CHECK(!strcmp(found, path));
	/* Otra ROM sin carátula y sin código. */
	CHECK(!cover_find(dir, "Otra.z64", NULL, NULL, found, sizeof found));
	CHECK(!cover_find(dir, "Otra.z64", NULL, "", found, sizeof found));
	/* Nombres de RetroArch: sin región ni etiquetas, y por el título. */
	snprintf(path, sizeof path, "%s/Mario Kart 64 (Europe) (Rev 1).png", dir);
	put_file(path, png, n);
	CHECK(cover_find(dir, "Mario Kart 64 (E).z64", NULL, NULL, found, sizeof found));
	CHECK(strstr(found, "Mario Kart 64 (Europe)") != NULL);
	snprintf(path, sizeof path, "%s/007 - GoldenEye (USA).png", dir);
	put_file(path, png, n);
	CHECK(cover_find(dir, "ge.z64", "GOLDENEYE", NULL, found, sizeof found));
	CHECK(strstr(found, "GoldenEye") != NULL);
	CHECK(!cover_find(dir, "ge.z64", "Gold", NULL, found, sizeof found));  /* muy corto */
	/* Región: la de Europa no sirve a una ROM de USA, y sin etiquetas vale cualquiera. */
	snprintf(path, sizeof path, "%s/Wave Race 64 (Europe).png", dir);
	put_file(path, png, n);
	CHECK(!cover_find(dir, "Wave Race 64 (U).z64", NULL, NULL, found, sizeof found));
	CHECK(!cover_find(dir, "wr64.z64", "WAVE RACE 64", "NWRE", found, sizeof found));
	CHECK(cover_find(dir, "wr64.z64", "WAVE RACE 64", "NWRP", found, sizeof found));
	/* Títulos de cabecera: «MACE» por el principio, «MARIOGOLF64» dentro de «Mario Golf». */
	snprintf(path, sizeof path, "%s/Mace - The Dark Age (USA).png", dir);
	put_file(path, png, n);
	snprintf(path, sizeof path, "%s/Mario Golf (USA).png", dir);
	put_file(path, png, n);
	CHECK(cover_find(dir, "zzz.z64", "MACE", NULL, found, sizeof found));
	CHECK(strstr(found, "Mace") != NULL);
	CHECK(cover_find(dir, "zzz.z64", "MARIOGOLF64", NULL, found, sizeof found));
	CHECK(strstr(found, "Mario Golf") != NULL);
	CHECK(!cover_find(dir, "zzz.z64", "MAC", NULL, found, sizeof found));
	CHECK(!cover_find(dir, "zzz.z64", "MARIO", NULL, found, sizeof found));  /* ni «Mario Golf» ni «Mario Kart» */
	CHECK(!cover_find(dir, "Zelda.z64", "Zelda", NULL, found, sizeof found));
	/* Un PNG enorme se descarta por el tamaño del archivo. */
	snprintf(path, sizeof path, "%s/Grande.png", dir);
	FILE *f = fopen(path, "wb");
	CHECK(f != NULL);
	if (f) {
		fwrite(png, 1, 8, f);
		fseek(f, (long)COVER_MAX_FILE_BYTES + 10, SEEK_SET);
		fputc(0, f);
		fclose(f);
	}
	CHECK_EQ(cover_load_png(path, rgba, &gw, &gh), COVER_TOO_BIG);
	free(png);
	char cmd[400];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
	CHECK_EQ(system(cmd), 0);
}

void t_suite_cover(void)
{
	RUN(color_types);
	RUN(filters);
	RUN(interlaced);
	RUN(downscale);
	RUN(errors);
	RUN(files);
}
