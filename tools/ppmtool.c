/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — utilidades de imágenes PPM para comparar capturas del host
 * (salsa64-run --gfx-frame) sin dependencias más allá de zlib:
 *
 *   ppmtool png ENTRADA.ppm SALIDA.png [ESCALA]
 *   (las salidas .png se escriben en PPM si el nombre termina en .ppm)
 *   ppmtool diff A.ppm B.ppm             píxeles distintos y diferencia media
 *   ppmtool crop ENTRADA.ppm SALIDA.png X Y ANCHO ALTO ESCALA
 *   ppmtool strip SALIDA.png COLUMNAS ESCALA A.ppm B.ppm …   mosaico (filas × COLUMNAS)
 *   ppmtool apng SALIDA.png MS ESCALA A.ppm B.ppm …          animación (APNG), MS por cuadro
 *
 * Compilar: cc -O2 -o build/ppmtool tools/ppmtool.c -lz
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

struct img {
	int w, h;
	uint8_t *p; /* RGB */
};

static int read_ppm(const char *path, struct img *im)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return -1;
	}
	char magic[3] = { 0 };
	int maxv;
	if (fscanf(f, "%2s %d %d %d", magic, &im->w, &im->h, &maxv) != 4 || strcmp(magic, "P6") || maxv != 255) {
		fprintf(stderr, "%s: no es un P6 de 8 bits\n", path);
		fclose(f);
		return -1;
	}
	fgetc(f);
	size_t n = (size_t)im->w * im->h * 3;
	im->p = malloc(n);
	if (fread(im->p, 1, n, f) != n) {
		fprintf(stderr, "%s: corto\n", path);
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

static void put32(uint8_t *b, uint32_t v)
{
	b[0] = v >> 24, b[1] = v >> 16, b[2] = v >> 8, b[3] = v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
	uint8_t hdr[8];
	put32(hdr, len);
	memcpy(hdr + 4, type, 4);
	fwrite(hdr, 1, 8, f);
	if (len)
		fwrite(data, 1, len, f);
	uint32_t crc = crc32(0, (const uint8_t *)type, 4);
	if (len)
		crc = crc32(crc, data, len);
	uint8_t c[4];
	put32(c, crc);
	fwrite(c, 1, 4, f);
}

/* Filas con el byte de filtro 0, comprimidas. */
static uint8_t *deflate_rgb(const struct img *im, uLongf *out_len)
{
	size_t raw_len = (size_t)(im->w * 3 + 1) * im->h;
	uint8_t *raw = malloc(raw_len);
	for (int y = 0; y < im->h; y++) {
		raw[y * (im->w * 3 + 1)] = 0;
		memcpy(raw + y * (im->w * 3 + 1) + 1, im->p + (size_t)y * im->w * 3, (size_t)im->w * 3);
	}
	*out_len = compressBound(raw_len);
	uint8_t *z = malloc(*out_len + 4);
	compress2(z + 4, out_len, raw, raw_len, 6);
	free(raw);
	return z; /* 4 bytes libres al principio (fdAT) */
}

static void ihdr(FILE *f, int w, int h)
{
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	fwrite(sig, 1, 8, f);
	uint8_t b[13];
	put32(b, w);
	put32(b + 4, h);
	b[8] = 8, b[9] = 2, b[10] = 0, b[11] = 0, b[12] = 0;
	chunk(f, "IHDR", b, 13);
}

static struct img scaled(const struct img *a, int k)
{
	struct img o = { a->w * k, a->h * k, NULL };
	o.p = malloc((size_t)o.w * o.h * 3);
	for (int y = 0; y < o.h; y++)
		for (int x = 0; x < o.w; x++)
			memcpy(o.p + ((size_t)y * o.w + x) * 3, a->p + ((size_t)(y / k) * a->w + x / k) * 3, 3);
	return o;
}

static int write_ppm_file(const char *path, const struct img *im)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror(path);
		return -1;
	}
	fprintf(f, "P6\n%d %d\n255\n", im->w, im->h);
	fwrite(im->p, 1, (size_t)im->w * im->h * 3, f);
	fclose(f);
	return 0;
}

/* PNG, o PPM si el nombre termina en .ppm. */
static int write_png(const char *path, const struct img *im)
{
	size_t ln = strlen(path);
	if (ln > 4 && !strcmp(path + ln - 4, ".ppm"))
		return write_ppm_file(path, im);
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror(path);
		return -1;
	}
	ihdr(f, im->w, im->h);
	uLongf zl;
	uint8_t *z = deflate_rgb(im, &zl);
	chunk(f, "IDAT", z + 4, (uint32_t)zl);
	chunk(f, "IEND", NULL, 0);
	free(z);
	fclose(f);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "png")) {
		struct img a;
		if (read_ppm(argv[2], &a))
			return 1;
		int k = argc > 4 ? atoi(argv[4]) : 1;
		struct img s = k > 1 ? scaled(&a, k) : a;
		return write_png(argv[3], &s) ? 1 : 0;
	}
	if (argc == 9 && !strcmp(argv[1], "crop")) {
		struct img a;
		if (read_ppm(argv[2], &a))
			return 1;
		int x0 = atoi(argv[4]), y0 = atoi(argv[5]), w = atoi(argv[6]), h = atoi(argv[7]), k = atoi(argv[8]);
		struct img c = { w, h, malloc((size_t)w * h * 3) };
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				int sx = x0 + x < a.w ? x0 + x : a.w - 1, sy = y0 + y < a.h ? y0 + y : a.h - 1;
				memcpy(c.p + ((size_t)y * w + x) * 3, a.p + ((size_t)sy * a.w + sx) * 3, 3);
			}
		struct img s = k > 1 ? scaled(&c, k) : c;
		return write_png(argv[3], &s) ? 1 : 0;
	}
	if (argc == 4 && !strcmp(argv[1], "diff")) {
		struct img a, b;
		if (read_ppm(argv[2], &a) || read_ppm(argv[3], &b))
			return 1;
		if (a.w != b.w || a.h != b.h) {
			printf("tamaños distintos\n");
			return 2;
		}
		long n = (long)a.w * a.h, changed = 0;
		double sum = 0;
		for (long i = 0; i < n; i++) {
			int d = abs(a.p[i * 3] - b.p[i * 3]) + abs(a.p[i * 3 + 1] - b.p[i * 3 + 1]) + abs(a.p[i * 3 + 2] - b.p[i * 3 + 2]);
			changed += d > 0;
			sum += d / 3.0;
		}
		printf("%.2f %% de píxeles distintos, diferencia media %.3f/255\n", 100.0 * changed / n, sum / n);
		return 0;
	}
	if (argc >= 6 && (!strcmp(argv[1], "strip") || !strcmp(argv[1], "apng"))) {
		int anim = !strcmp(argv[1], "apng");
		int param = atoi(argv[3]), k = atoi(argv[4]);
		int n = argc - 5;
		struct img *im = calloc(n, sizeof *im);
		for (int i = 0; i < n; i++) {
			struct img a;
			if (read_ppm(argv[5 + i], &a))
				return 1;
			im[i] = k > 1 ? scaled(&a, k) : a;
		}
		if (!anim) {
			int cols = param, rows = (n + cols - 1) / cols, gap = 4;
			int cw = im[0].w, ch = im[0].h;
			struct img o = { cols * cw + (cols - 1) * gap, rows * ch + (rows - 1) * gap, NULL };
			o.p = calloc((size_t)o.w * o.h, 3);
			memset(o.p, 40, (size_t)o.w * o.h * 3);
			for (int i = 0; i < n; i++) {
				int ox = (i % cols) * (cw + gap), oy = (i / cols) * (ch + gap);
				for (int y = 0; y < im[i].h && y < ch; y++)
					memcpy(o.p + ((size_t)(oy + y) * o.w + ox) * 3, im[i].p + (size_t)y * im[i].w * 3,
					       (size_t)(im[i].w < cw ? im[i].w : cw) * 3);
			}
			return write_png(argv[2], &o) ? 1 : 0;
		}
		FILE *f = fopen(argv[2], "wb");
		if (!f) {
			perror(argv[2]);
			return 1;
		}
		ihdr(f, im[0].w, im[0].h);
		uint8_t actl[8];
		put32(actl, n);
		put32(actl + 4, 0);
		chunk(f, "acTL", actl, 8);
		uint32_t seq = 0;
		for (int i = 0; i < n; i++) {
			uint8_t fc[26];
			put32(fc, seq++);
			put32(fc + 4, im[0].w);
			put32(fc + 8, im[0].h);
			put32(fc + 12, 0);
			put32(fc + 16, 0);
			fc[20] = param >> 8, fc[21] = param & 0xFF; /* delay_num (ms) */
			fc[22] = 1000 >> 8, fc[23] = 1000 & 0xFF;   /* delay_den */
			fc[24] = 0, fc[25] = 0;
			chunk(f, "fcTL", fc, 26);
			uLongf zl;
			uint8_t *z = deflate_rgb(&im[i], &zl);
			if (i == 0) {
				chunk(f, "IDAT", z + 4, (uint32_t)zl);
			} else {
				put32(z, seq++);
				chunk(f, "fdAT", z, (uint32_t)zl + 4);
			}
			free(z);
		}
		chunk(f, "IEND", NULL, 0);
		fclose(f);
		return 0;
	}
	fprintf(stderr, "uso: ppmtool png|diff|strip|apng … (ver el encabezado)\n");
	return 3;
}
