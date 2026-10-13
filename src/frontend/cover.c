/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — carátulas (PNG → RGBA8888 reducido). Ver cover.h.
 *
 * El desinflado sigue el método de puff.c de Mark Adler (zlib/contrib/puff:
 * códigos de Huffman canónicos decodificados bit a bit, simple y sin tablas
 * grandes), escrito de nuevo aquí con códigos de error en vez de setjmp.
 */
#include "cover.h"
#include "namematch.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- Desinflado ----------------------------------------------------------------- */

#define MAXBITS   15
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES  (MAXLCODES + MAXDCODES)
#define FIXLCODES 288

struct inf {
	uint8_t *out;
	size_t outlen, outcnt;
	const uint8_t *in;
	size_t inlen, incnt;
	long bitbuf;
	int bitcnt;
};

struct huffman {
	short *count;   /* cuántos códigos hay de cada largo */
	short *symbol;  /* símbolos en orden canónico */
};

/* `need` bits (0 a 13), o -1 si se acabó la entrada. */
static int bits(struct inf *s, int need)
{
	long val = s->bitbuf;
	while (s->bitcnt < need) {
		if (s->incnt == s->inlen)
			return -1;
		val |= (long)s->in[s->incnt++] << s->bitcnt;
		s->bitcnt += 8;
	}
	s->bitbuf = val >> need;
	s->bitcnt -= need;
	return (int)(val & ((1L << need) - 1));
}

static int stored(struct inf *s)
{
	s->bitbuf = 0;
	s->bitcnt = 0;
	if (s->incnt + 4 > s->inlen)
		return 2;
	unsigned len = s->in[s->incnt++];
	len |= (unsigned)s->in[s->incnt++] << 8;
	unsigned nlen = s->in[s->incnt++];
	nlen |= (unsigned)s->in[s->incnt++] << 8;
	if (len != (~nlen & 0xFFFFu))
		return -2;
	if (s->incnt + len > s->inlen)
		return 2;
	if (s->outcnt + len > s->outlen)
		return 1;
	memcpy(s->out + s->outcnt, s->in + s->incnt, len);
	s->outcnt += len;
	s->incnt += len;
	return 0;
}

/* Decodifica un símbolo: >= 0, o un error negativo. */
static int decode(struct inf *s, const struct huffman *h)
{
	int code = 0, first = 0, index = 0;
	for (int len = 1; len <= MAXBITS; len++) {
		int b = bits(s, 1);
		if (b < 0)
			return -1;
		code |= b;
		int count = h->count[len];
		if (code - count < first)
			return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -2;  /* se acabaron los largos: código inválido */
}

/* Arma las tablas de `n` largos. 0 si el código es completo, > 0 si quedó
 * incompleto, < 0 si está sobre-suscrito. */
static int construct(struct huffman *h, const short *length, int n)
{
	short offs[MAXBITS + 1];
	for (int len = 0; len <= MAXBITS; len++)
		h->count[len] = 0;
	for (int symbol = 0; symbol < n; symbol++)
		h->count[length[symbol]]++;
	if (h->count[0] == n)
		return 0;
	int left = 1;
	for (int len = 1; len <= MAXBITS; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0)
			return left;
	}
	offs[1] = 0;
	for (int len = 1; len < MAXBITS; len++)
		offs[len + 1] = (short)(offs[len] + h->count[len]);
	for (int symbol = 0; symbol < n; symbol++)
		if (length[symbol] != 0)
			h->symbol[offs[length[symbol]]++] = (short)symbol;
	return left;
}

static int codes(struct inf *s, const struct huffman *lencode, const struct huffman *distcode)
{
	static const short lens[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
					35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
	static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
					3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
	static const short dists[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
					 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
	static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
					7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
	int symbol;
	do {
		symbol = decode(s, lencode);
		if (symbol < 0)
			return symbol;
		if (symbol < 256) {
			if (s->outcnt == s->outlen)
				return 1;
			s->out[s->outcnt++] = (uint8_t)symbol;
		} else if (symbol > 256) {
			symbol -= 257;
			if (symbol >= 29)
				return -10;
			int e = bits(s, lext[symbol]);
			if (e < 0)
				return -1;
			unsigned len = (unsigned)lens[symbol] + (unsigned)e;
			symbol = decode(s, distcode);
			if (symbol < 0)
				return symbol;
			e = bits(s, dext[symbol]);
			if (e < 0)
				return -1;
			size_t dist = (size_t)dists[symbol] + (size_t)e;
			if (dist > s->outcnt)
				return -11;
			if (s->outcnt + len > s->outlen)
				return 1;
			while (len--) {
				s->out[s->outcnt] = s->out[s->outcnt - dist];
				s->outcnt++;
			}
		}
	} while (symbol != 256);
	return 0;
}

static int fixed(struct inf *s)
{
	static bool built;
	static short lencnt[MAXBITS + 1], lensym[FIXLCODES], distcnt[MAXBITS + 1], distsym[MAXDCODES];
	static struct huffman lencode = { lencnt, lensym }, distcode = { distcnt, distsym };
	if (!built) {
		short lengths[FIXLCODES];
		int symbol;
		for (symbol = 0; symbol < 144; symbol++)
			lengths[symbol] = 8;
		for (; symbol < 256; symbol++)
			lengths[symbol] = 9;
		for (; symbol < 280; symbol++)
			lengths[symbol] = 7;
		for (; symbol < FIXLCODES; symbol++)
			lengths[symbol] = 8;
		construct(&lencode, lengths, FIXLCODES);
		for (symbol = 0; symbol < MAXDCODES; symbol++)
			lengths[symbol] = 5;
		construct(&distcode, lengths, MAXDCODES);
		built = true;
	}
	return codes(s, &lencode, &distcode);
}

static int dynamic(struct inf *s)
{
	static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
	short lengths[MAXCODES];
	short lencnt[MAXBITS + 1], lensym[MAXLCODES], distcnt[MAXBITS + 1], distsym[MAXDCODES];
	struct huffman lencode = { lencnt, lensym }, distcode = { distcnt, distsym };

	int nlen = bits(s, 5), ndist = bits(s, 5), ncode = bits(s, 4);
	if (nlen < 0 || ndist < 0 || ncode < 0)
		return -1;
	nlen += 257;
	ndist += 1;
	ncode += 4;
	if (nlen > MAXLCODES || ndist > MAXDCODES)
		return -3;
	int index;
	for (index = 0; index < ncode; index++) {
		int v = bits(s, 3);
		if (v < 0)
			return -1;
		lengths[order[index]] = (short)v;
	}
	for (; index < 19; index++)
		lengths[order[index]] = 0;
	if (construct(&lencode, lengths, 19) != 0)
		return -4;  /* el código de los largos tiene que ser completo */
	index = 0;
	while (index < nlen + ndist) {
		int symbol = decode(s, &lencode);
		if (symbol < 0)
			return symbol;
		if (symbol < 16) {
			lengths[index++] = (short)symbol;
		} else {
			int len = 0, rep;
			if (symbol == 16) {
				if (index == 0)
					return -5;
				len = lengths[index - 1];
				rep = bits(s, 2);
				rep = rep < 0 ? rep : 3 + rep;
			} else if (symbol == 17) {
				rep = bits(s, 3);
				rep = rep < 0 ? rep : 3 + rep;
			} else {
				rep = bits(s, 7);
				rep = rep < 0 ? rep : 11 + rep;
			}
			if (rep < 0)
				return -1;
			if (index + rep > nlen + ndist)
				return -6;
			while (rep--)
				lengths[index++] = (short)len;
		}
	}
	if (lengths[256] == 0)
		return -9;  /* sin fin de bloque */
	int err = construct(&lencode, lengths, nlen);
	if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1]))
		return -7;
	err = construct(&distcode, lengths + nlen, ndist);
	if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1]))
		return -8;
	return codes(s, &lencode, &distcode);
}

/* Datos deflate crudos a `out` (de `outlen` bytes exactos como máximo). */
static bool inflate_raw(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen, size_t *used)
{
	struct inf s = { .out = out, .outlen = outlen, .in = in, .inlen = inlen };
	int last;
	do {
		last = bits(&s, 1);
		int type = bits(&s, 2);
		if (last < 0 || type < 0)
			return false;
		int err = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
		if (err)
			return false;
	} while (!last);
	*used = s.outcnt;
	return true;
}

/* ---- PNG ------------------------------------------------------------------------- */

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

struct png {
	uint32_t w, h;
	int depth, ctype, interlace;
	unsigned bits_pp;      /* bits por píxel */
	uint8_t plte[256 * 3];
	unsigned nplte;
	uint8_t trns_a[256];   /* alfa de la paleta */
	unsigned ntrns;
	bool key;              /* tRNS de gris o RGB: un color transparente */
	unsigned key_v[3];
};

static bool mode_valid(int depth, int ctype)
{
	switch (ctype) {
	case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
	case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
	case 2: case 4: case 6: return depth == 8 || depth == 16;
	default: return false;
	}
}

static unsigned channels(int ctype)
{
	return ctype == 0 || ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 2 ? 3 : 4;
}

static int paeth(int a, int b, int c)
{
	int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* Deshace el filtro de una línea en su sitio. `prev` es la anterior ya
 * deshecha (o NULL para la primera de la pasada). */
static bool unfilter(uint8_t *row, const uint8_t *prev, size_t n, unsigned bpp, int type)
{
	switch (type) {
	case 0:
		break;
	case 1:
		for (size_t i = bpp; i < n; i++)
			row[i] = (uint8_t)(row[i] + row[i - bpp]);
		break;
	case 2:
		if (prev)
			for (size_t i = 0; i < n; i++)
				row[i] = (uint8_t)(row[i] + prev[i]);
		break;
	case 3:
		for (size_t i = 0; i < n; i++) {
			int a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0;
			row[i] = (uint8_t)(row[i] + ((a + b) >> 1));
		}
		break;
	case 4:
		for (size_t i = 0; i < n; i++) {
			int a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0, c = (prev && i >= bpp) ? prev[i - bpp] : 0;
			row[i] = (uint8_t)(row[i] + paeth(a, b, c));
		}
		break;
	default:
		return false;
	}
	return true;
}

/* La muestra `i` de una línea de profundidad 1, 2, 4, 8 o 16 (cruda). */
static unsigned sample(const uint8_t *row, unsigned i, int depth)
{
	if (depth == 8)
		return row[i];
	if (depth == 16)
		return (unsigned)row[2 * i] << 8 | row[2 * i + 1];
	unsigned bitpos = i * (unsigned)depth;
	unsigned shift = 8u - (unsigned)depth - (bitpos & 7u);
	return (row[bitpos >> 3] >> shift) & ((1u << depth) - 1u);
}

/* El píxel `i` de una línea como RGBA8. */
static void pixel(const struct png *p, const uint8_t *row, unsigned i, uint8_t out[4])
{
	unsigned a = 255;
	switch (p->ctype) {
	case 0: {
		unsigned v = sample(row, i, p->depth), g = p->depth == 16 ? v >> 8 : p->depth == 8 ? v : v * 255u / ((1u << p->depth) - 1u);
		if (p->key && v == p->key_v[0])
			a = 0;
		out[0] = out[1] = out[2] = (uint8_t)g;
		break;
	}
	case 2: {
		unsigned v[3];
		for (int c = 0; c < 3; c++)
			v[c] = sample(row, 3 * i + (unsigned)c, p->depth);
		if (p->key && v[0] == p->key_v[0] && v[1] == p->key_v[1] && v[2] == p->key_v[2])
			a = 0;
		for (int c = 0; c < 3; c++)
			out[c] = (uint8_t)(p->depth == 16 ? v[c] >> 8 : v[c]);
		break;
	}
	case 3: {
		unsigned idx = sample(row, i, p->depth);
		if (idx < p->nplte) {
			out[0] = p->plte[3 * idx];
			out[1] = p->plte[3 * idx + 1];
			out[2] = p->plte[3 * idx + 2];
		} else {
			out[0] = out[1] = out[2] = 0;
		}
		a = idx < p->ntrns ? p->trns_a[idx] : 255;
		break;
	}
	case 4: {
		unsigned g = sample(row, 2 * i, p->depth), al = sample(row, 2 * i + 1, p->depth);
		out[0] = out[1] = out[2] = (uint8_t)(p->depth == 16 ? g >> 8 : g);
		a = p->depth == 16 ? al >> 8 : al;
		break;
	}
	default: {
		for (int c = 0; c < 3; c++) {
			unsigned v = sample(row, 4 * i + (unsigned)c, p->depth);
			out[c] = (uint8_t)(p->depth == 16 ? v >> 8 : v);
		}
		unsigned al = sample(row, 4 * i + 3, p->depth);
		a = p->depth == 16 ? al >> 8 : al;
		break;
	}
	}
	out[3] = (uint8_t)a;
}

/* Tamaño de la línea de `pw` píxeles, sin el byte del filtro. */
static size_t row_bytes(const struct png *p, uint32_t pw)
{
	return ((size_t)pw * p->bits_pp + 7) / 8;
}

static const struct { uint8_t x0, y0, dx, dy; } adam7[7] = {
	{ 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 }, { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 },
};

/* Bytes desinflados que trae la imagen (con los de filtro), o 0 si no cabe. */
static uint64_t raw_size(const struct png *p)
{
	uint64_t total = 0;
	if (!p->interlace)
		return (uint64_t)(row_bytes(p, p->w) + 1) * p->h;
	for (int k = 0; k < 7; k++) {
		if (p->w <= adam7[k].x0 || p->h <= adam7[k].y0)
			continue;
		uint32_t pw = (p->w - adam7[k].x0 + adam7[k].dx - 1) / adam7[k].dx;
		uint32_t ph = (p->h - adam7[k].y0 + adam7[k].dy - 1) / adam7[k].dy;
		total += (uint64_t)(row_bytes(p, pw) + 1) * ph;
	}
	return total;
}

enum cover_result cover_decode_png(const uint8_t *data, size_t size, uint8_t *rgba, unsigned *w_out, unsigned *h_out)
{
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	if (size < 8 + 25 || memcmp(data, sig, 8) != 0)
		return COVER_BAD;
	struct png p;
	memset(&p, 0, sizeof p);
	bool have_ihdr = false;
	size_t idat_total = 0, pos = 8;
	/* Primera pasada: encabezado, paleta, transparencia y cuánto suman los IDAT. */
	while (pos + 12 <= size) {
		uint32_t len = be32(data + pos);
		const uint8_t *type = data + pos + 4, *body = data + pos + 8;
		if (len > size - pos - 12)
			return COVER_BAD;
		if (!memcmp(type, "IHDR", 4)) {
			if (len != 13 || have_ihdr)
				return COVER_BAD;
			p.w = be32(body);
			p.h = be32(body + 4);
			p.depth = body[8];
			p.ctype = body[9];
			p.interlace = body[12];
			if (!p.w || !p.h || p.w > 65535 || p.h > 65535 || body[10] != 0 || body[11] != 0 || p.interlace > 1)
				return COVER_BAD;
			if (!mode_valid(p.depth, p.ctype))
				return COVER_UNSUPPORTED;
			p.bits_pp = (unsigned)p.depth * channels(p.ctype);
			have_ihdr = true;
		} else if (!memcmp(type, "PLTE", 4)) {
			if (len % 3 || len > 256 * 3)
				return COVER_BAD;
			memcpy(p.plte, body, len);
			p.nplte = len / 3;
		} else if (!memcmp(type, "tRNS", 4) && have_ihdr) {
			if (p.ctype == 3) {
				p.ntrns = len > 256 ? 256 : len;
				memcpy(p.trns_a, body, p.ntrns);
			} else if (p.ctype == 0 && len >= 2) {
				p.key = true;
				p.key_v[0] = (unsigned)body[0] << 8 | body[1];
			} else if (p.ctype == 2 && len >= 6) {
				p.key = true;
				for (int c = 0; c < 3; c++)
					p.key_v[c] = (unsigned)body[2 * c] << 8 | body[2 * c + 1];
			}
		} else if (!memcmp(type, "IDAT", 4)) {
			idat_total += len;
		} else if (!memcmp(type, "IEND", 4)) {
			break;
		}
		pos += 12 + (size_t)len;
	}
	if (!have_ihdr || !idat_total || (p.ctype == 3 && !p.nplte))
		return COVER_BAD;
	uint64_t raw_n = raw_size(&p);
	if (raw_n > COVER_MAX_RAW_BYTES)
		return COVER_TOO_BIG;

	uint8_t *zbuf = malloc(idat_total), *raw = malloc((size_t)raw_n);
	if (!zbuf || !raw) {
		free(zbuf);
		free(raw);
		return COVER_TOO_BIG;
	}
	/* Segunda: los IDAT juntos. */
	size_t zn = 0;
	pos = 8;
	while (pos + 12 <= size) {
		uint32_t len = be32(data + pos);
		if (!memcmp(data + pos + 4, "IDAT", 4) && zn + len <= idat_total) {
			memcpy(zbuf + zn, data + pos + 8, len);
			zn += len;
		}
		pos += 12 + (size_t)len;
	}

	enum cover_result res = COVER_BAD;
	size_t used = 0;
	uint32_t *sum = NULL, *cnt = NULL;
	uint16_t *xmap = NULL;
	/* zlib: CM = 8, sin diccionario, y la suma de control de la cabecera. */
	if (zn < 6 || (zbuf[0] & 0x0F) != 8 || (zbuf[1] & 0x20) || (((unsigned)zbuf[0] << 8) | zbuf[1]) % 31 != 0)
		goto done;
	if (!inflate_raw(zbuf + 2, zn - 2, raw, (size_t)raw_n, &used) || used != raw_n)
		goto done;

	/* Tamaño final: a lo más el máximo, sin cambiar la proporción. */
	uint32_t dw = p.w, dh = p.h;
	if (p.w > COVER_MAX_W || p.h > COVER_MAX_H) {
		if ((uint64_t)p.w * COVER_MAX_H >= (uint64_t)p.h * COVER_MAX_W) {
			dw = COVER_MAX_W;
			dh = (uint32_t)((uint64_t)p.h * COVER_MAX_W / p.w);
		} else {
			dh = COVER_MAX_H;
			dw = (uint32_t)((uint64_t)p.w * COVER_MAX_H / p.h);
		}
		if (!dw)
			dw = 1;
		if (!dh)
			dh = 1;
	}
	sum = calloc((size_t)dw * dh * 4, sizeof *sum);
	cnt = calloc((size_t)dw * dh, sizeof *cnt);
	xmap = malloc(p.w * sizeof *xmap);
	if (!sum || !cnt || !xmap) {
		res = COVER_TOO_BIG;
		goto done;
	}
	for (uint32_t x = 0; x < p.w; x++)
		xmap[x] = (uint16_t)((uint64_t)x * dw / p.w);

	/* Cada pasada: líneas deshechas y cada píxel a su caja del tamaño final. */
	size_t off = 0;
	unsigned bpp = (p.bits_pp + 7) / 8;
	for (int k = 0; k < (p.interlace ? 7 : 1); k++) {
		uint32_t x0 = p.interlace ? adam7[k].x0 : 0, y0 = p.interlace ? adam7[k].y0 : 0;
		uint32_t dx = p.interlace ? adam7[k].dx : 1, dy = p.interlace ? adam7[k].dy : 1;
		if (p.w <= x0 || p.h <= y0)
			continue;
		uint32_t pw = (p.w - x0 + dx - 1) / dx, ph = (p.h - y0 + dy - 1) / dy;
		size_t rb = row_bytes(&p, pw);
		const uint8_t *prev = NULL;
		for (uint32_t r = 0; r < ph; r++) {
			uint8_t *row = raw + off + 1;
			if (!unfilter(row, prev, rb, bpp, raw[off]))
				goto done;
			uint32_t y = y0 + r * dy;
			uint32_t ty = (uint32_t)((uint64_t)y * dh / p.h);
			for (uint32_t i = 0; i < pw; i++) {
				uint8_t px[4];
				pixel(&p, row, i, px);
				size_t idx = (size_t)ty * dw + xmap[x0 + i * dx];
				sum[idx * 4 + 0] += px[0];
				sum[idx * 4 + 1] += px[1];
				sum[idx * 4 + 2] += px[2];
				sum[idx * 4 + 3] += px[3];
				cnt[idx]++;
			}
			prev = row;
			off += rb + 1;
		}
	}
	for (size_t i = 0; i < (size_t)dw * dh; i++) {
		uint32_t n = cnt[i];
		for (int c = 0; c < 4; c++)
			rgba[i * 4 + (size_t)c] = n ? (uint8_t)((sum[i * 4 + (size_t)c] + n / 2) / n) : 0;
	}
	*w_out = dw;
	*h_out = dh;
	res = COVER_OK;
done:
	free(xmap);
	free(sum);
	free(cnt);
	free(zbuf);
	free(raw);
	return res;
}

enum cover_result cover_load_png(const char *path, uint8_t *rgba, unsigned *w, unsigned *h)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return COVER_NO_FILE;
	uint8_t *buf = malloc(COVER_MAX_FILE_BYTES + 1);
	if (!buf) {
		fclose(f);
		return COVER_TOO_BIG;
	}
	size_t n = fread(buf, 1, COVER_MAX_FILE_BYTES + 1, f);
	fclose(f);
	enum cover_result r = n > COVER_MAX_FILE_BYTES ? COVER_TOO_BIG : cover_decode_png(buf, n, rgba, w, h);
	free(buf);
	return r;
}

const char *cover_result_name(enum cover_result r)
{
	switch (r) {
	case COVER_OK: return "bien";
	case COVER_NO_FILE: return "no hay archivo";
	case COVER_BAD: return "PNG dañado";
	case COVER_TOO_BIG: return "imagen demasiado grande";
	default: return "modo de PNG no admitido";
	}
}

/* ---- Búsqueda del archivo ------------------------------------------------------------ */

/* <dir>/<base>.png tal cual o, si no, con otras mayúsculas. */
static bool find_png(const char *dir, const char *base, size_t base_len, char *path, size_t size)
{
	snprintf(path, size, "%s/%.*s.png", dir, (int)base_len, base);
	FILE *f = fopen(path, "rb");
	if (f) {
		fclose(f);
		return true;
	}
	DIR *d = opendir(dir);
	if (!d)
		return false;
	bool found = false;
	struct dirent *de;
	while (!found && (de = readdir(d)) != NULL) {
		size_t n = strlen(de->d_name);
		if (n == base_len + 4 && !strncasecmp(de->d_name, base, base_len) && !strcasecmp(de->d_name + base_len, ".png")) {
			snprintf(path, size, "%s/%s", dir, de->d_name);
			found = true;
		}
	}
	closedir(d);
	return found;
}

bool cover_find(const char *dir, const char *rom_name, const char *title, const char *code, char *path, size_t size)
{
	const char *dot = strrchr(rom_name, '.');
	size_t stem = dot && dot > rom_name ? (size_t)(dot - rom_name) : strlen(rom_name);
	if (stem && find_png(dir, rom_name, stem, path, size))
		return true;
	if (code && code[0] && strlen(code) <= 8 && find_png(dir, code, strlen(code), path, size))
		return true;
	return name_find(dir, ".png", rom_name, title, code, 4, path, size);
}
