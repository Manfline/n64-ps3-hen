/*
 * Salsa64 — TMEM del RDP: cargas desde la RDRAM y decodificación de una
 * tile a RGBA8888 para el RSX.
 *
 * La TMEM son 4 KB en palabras de 64 bits. En las filas impares de una
 * textura las dos mitades de 32 bits de cada palabra van intercambiadas
 * (LOADTILE lo hace por fila; LOADBLOCK por el contador dxt). Las texturas
 * de 32 bits se parten: R y G en la mitad baja, B y A en la alta (+2 KB).
 * Las paletas (LOADTLUT) van en la mitad alta, cada entrada de 16 bits
 * repetida cuatro veces (8 bytes por entrada).
 *
 * Fuentes: n64brew.dev (Reality Display Processor/Commands: Load Block,
 * Load Tile, Load TLUT, Set Tile; TMEM) y el comportamiento descrito por
 * GLideN64 (docs/THIRD_PARTY.md). Código propio.
 *
 * Caché: cada carga marca los bloques de 64 bytes que escribe con una
 * identidad (dirección de origen y parámetros). La textura de una tile se
 * identifica por esas marcas y los parámetros de la tile; dentro de una
 * tarea, la misma identidad reutiliza la textura ya decodificada.
 */
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"

#define TMEM_MASK 0xFFFu

static uint32_t mix(uint32_t h, uint32_t v)
{
	h ^= v;
	h *= 0x01000193u;
	h ^= h >> 15;
	return h;
}

static void mark(struct gfx_state *g, uint32_t start, uint32_t bytes, uint32_t key)
{
	if (bytes >= 4096) {
		for (int i = 0; i < 64; i++)
			g->tmem_key[i] = key;
		return;
	}
	uint32_t first = (start & TMEM_MASK) >> 6, last = ((start + bytes - 1) & TMEM_MASK) >> 6;
	for (uint32_t b = first;; b = (b + 1) & 63) {
		g->tmem_key[b] = key;
		if (b == last)
			break;
	}
}

static uint32_t load_key(struct gfx_state *g, uint32_t op, uint32_t w0, uint32_t w1)
{
	uint32_t h = 0x811C9DC5u;
	h = mix(h, op);
	h = mix(h, g->timg_addr);
	h = mix(h, (uint32_t)g->timg_width << 8 | g->timg_siz);
	h = mix(h, w0);
	h = mix(h, w1 & 0x00FFFFFF);
	return h | 1;  /* 0 = nada cargado */
}

static void swap_halves(uint8_t *tmem, uint32_t addr)
{
	uint8_t *p = tmem + (addr & TMEM_MASK & ~7u);
	uint32_t a, b;
	memcpy(&a, p, 4);
	memcpy(&b, p + 4, 4);
	memcpy(p, &b, 4);
	memcpy(p + 4, &a, 4);
}

/* Bytes por texel ×2 (los de 4 bits ocupan medio byte). */
static uint32_t half_bytes(unsigned siz)
{
	return siz == G_IM_SIZ_4b ? 1 : siz == G_IM_SIZ_8b ? 2 : siz == G_IM_SIZ_16b ? 4 : 8;
}

/* Texel de 32 bits a las dos mitades de la TMEM; idx en unidades de 16 bits. */
static void put32(uint8_t *tmem, uint32_t idx, const uint8_t *src)
{
	idx &= 0x3FF;
	memcpy(tmem + idx * 2, src, 2);
	memcpy(tmem + 0x800 + idx * 2, src + 2, 2);
}

void gfx_load_block(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	struct gfx_tile *t = &g->tiles[(w1 >> 24) & 7];
	uint32_t uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF;
	uint32_t lrs = (w1 >> 12) & 0xFFF, dxt = w1 & 0xFFF;
	if (lrs < uls)
		return;
	uint32_t texels = lrs - uls + 1;
	uint32_t hb = half_bytes(g->timg_siz);
	uint32_t src = g->timg_addr + ((ult * g->timg_width + uls) * hb) / 2;
	uint32_t bytes = (texels * hb / 2 + 7) & ~7u;
	if (bytes > 4096)
		bytes = 4096;
	const uint8_t *p = gfx_io_read(g, GFX_IO_TEX, src, bytes);
	if (!p) {
		g->res->dl_errors++;
		return;
	}
	if (g->timg_siz == G_IM_SIZ_32b) {
		uint32_t base = t->tmem / 2;
		for (uint32_t k = 0; k < bytes / 8; k++) {
			uint32_t x = ((k * dxt) >> 11) & 1 ? 2 : 0;
			put32(g->tmem, (base + 2 * k) ^ x, p + 8 * k);
			put32(g->tmem, (base + 2 * k + 1) ^ x, p + 8 * k + 4);
		}
		mark(g, t->tmem, bytes / 2, load_key(g, 0xF3, w0, w1));
		mark(g, t->tmem + 0x800, bytes / 2, load_key(g, 0xF3, w0, w1));
		return;
	}
	for (uint32_t k = 0; k < bytes / 8; k++) {
		uint32_t dst = (t->tmem + 8 * k) & TMEM_MASK;
		memcpy(g->tmem + dst, p + 8 * k, 8);
		if (((k * dxt) >> 11) & 1)
			swap_halves(g->tmem, dst);
	}
	mark(g, t->tmem, bytes, load_key(g, 0xF3, w0, w1));
}

void gfx_load_tile(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	struct gfx_tile *t = &g->tiles[(w1 >> 24) & 7];
	t->uls = (w0 >> 12) & 0xFFF;
	t->ult = w0 & 0xFFF;
	t->lrs = (w1 >> 12) & 0xFFF;
	t->lrt = w1 & 0xFFF;
	uint32_t x0 = t->uls >> 2, y0 = t->ult >> 2, x1 = t->lrs >> 2, y1 = t->lrt >> 2;
	if (x1 < x0 || y1 < y0)
		return;
	uint32_t width = x1 - x0 + 1, rows = y1 - y0 + 1;
	uint32_t hb = half_bytes(g->timg_siz);
	uint32_t row_bytes = (width * hb / 2 + 7) & ~7u;
	uint32_t key = load_key(g, 0xF4, w0, w1);
	if (row_bytes > 4096)
		row_bytes = 4096;

	for (uint32_t r = 0; r < rows; r++) {
		uint32_t src = g->timg_addr + (((y0 + r) * g->timg_width + x0) * hb) / 2;
		const uint8_t *p = gfx_io_read(g, GFX_IO_TEX, src, row_bytes);
		if (!p) {
			g->res->dl_errors++;
			return;
		}
		if (g->timg_siz == G_IM_SIZ_32b) {
			uint32_t base = t->tmem / 2 + r * (t->line / 2);
			uint32_t x = (r & 1) ? 2 : 0;
			for (uint32_t j = 0; j < width; j++)
				put32(g->tmem, (base + j) ^ x, p + 4 * j);
			mark(g, t->tmem + r * t->line, width * 2, key);
			mark(g, 0x800 + t->tmem + r * t->line, width * 2, key);
			continue;
		}
		uint32_t dst = t->tmem + r * t->line;
		for (uint32_t k = 0; k < row_bytes; k += 8) {
			uint32_t d = (dst + k) & TMEM_MASK;
			memcpy(g->tmem + d, p + k, 8);
			if (r & 1)
				swap_halves(g->tmem, d);
		}
		mark(g, dst, row_bytes, key);
	}
}

void gfx_load_tlut(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	struct gfx_tile *t = &g->tiles[(w1 >> 24) & 7];
	uint32_t uls = ((w0 >> 12) & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2;
	uint32_t lrs = ((w1 >> 12) & 0xFFF) >> 2;
	if (lrs < uls)
		return;
	uint32_t count = lrs - uls + 1;
	if (count > 256)
		count = 256;
	uint32_t src = g->timg_addr + (ult * g->timg_width + uls) * 2;
	const uint8_t *p = gfx_io_read(g, GFX_IO_TEX, src, count * 2);
	if (!p) {
		g->res->dl_errors++;
		return;
	}
	for (uint32_t e = 0; e < count; e++) {
		uint32_t d = 0x800 | ((t->tmem + 8 * e) & 0x7FF);
		for (int k = 0; k < 4; k++)
			memcpy(g->tmem + d + 2 * k, p + 2 * e, 2);
	}
	mark(g, 0x800 | (t->tmem & 0x7FF), count * 8, load_key(g, 0xF0, w0, w1));
}

void gfx_tile_size(const struct gfx_tile *t, uint32_t *w, uint32_t *h)
{
	uint32_t tw = t->lrs >= t->uls ? ((t->lrs - t->uls) >> 2) + 1 : 1;
	uint32_t th = t->lrt >= t->ult ? ((t->lrt - t->ult) >> 2) + 1 : 1;
	*w = t->masks && !(t->cms & 2) ? 1u << (t->masks > 10 ? 10 : t->masks) : tw;
	*h = t->maskt && !(t->cmt & 2) ? 1u << (t->maskt > 10 ? 10 : t->maskt) : th;
	if (*w > 1024)
		*w = 1024;
	if (*h > 1024)
		*h = 1024;
}

static uint32_t expand5(uint32_t v)
{
	return v << 3 | v >> 2;
}

static uint32_t rgba16(uint32_t p)
{
	return expand5(p >> 11) << 24 | expand5((p >> 6) & 31) << 16 | expand5((p >> 1) & 31) << 8 |
	       ((p & 1) ? 0xFF : 0);
}

static uint32_t ia16(uint32_t p)
{
	uint32_t i = p >> 8;
	return i << 24 | i << 16 | i << 8 | (p & 0xFF);
}

static uint32_t gray(uint32_t i, uint32_t a)
{
	return i << 24 | i << 16 | i << 8 | a;
}

/* Entrada de la paleta: 8 bytes por entrada desde 0x800. */
static uint32_t tlut(const struct gfx_state *g, uint32_t index, unsigned type)
{
	uint32_t p = salsa64_rd16(g->tmem + 0x800 + ((index * 8) & 0x7FF));
	return type == G_TT_IA16 ? ia16(p) : rgba16(p);
}

/* Texel (x, y) de la tile en RGBA8888 (0xRRGGBBAA). */
static uint32_t texel(const struct gfx_state *g, const struct gfx_tile *t, uint32_t x, uint32_t y, unsigned tt)
{
	uint32_t odd = (y & 1) ? 4 : 0;
	uint32_t row = t->tmem + (y & 0xFF) * t->line;
	switch (t->siz) {
	case G_IM_SIZ_4b: {
		uint8_t b = g->tmem[((row + x / 2) ^ odd) & TMEM_MASK];
		uint32_t n = (x & 1) ? b & 15 : b >> 4;
		if (tt != G_TT_NONE)
			return tlut(g, (uint32_t)t->pal << 4 | n, tt);
		switch (t->fmt) {
		case G_IM_FMT_IA: {
			uint32_t i = n >> 1;
			return gray(i << 5 | i << 2 | i >> 1, (n & 1) ? 0xFF : 0);
		}
		case G_IM_FMT_CI: {
			uint32_t i = (uint32_t)t->pal << 4 | n;
			return gray(i, i);
		}
		default:
			return gray(n << 4 | n, n << 4 | n);
		}
	}
	case G_IM_SIZ_8b: {
		uint32_t b = g->tmem[((row + x) ^ odd) & TMEM_MASK];
		if (tt != G_TT_NONE)
			return tlut(g, b, tt);
		if (t->fmt == G_IM_FMT_IA)
			return gray((b & 0xF0) | b >> 4, (b & 15) << 4 | (b & 15));
		return gray(b, b);
	}
	case G_IM_SIZ_16b: {
		uint32_t p = salsa64_rd16(g->tmem + (((row + 2 * x) ^ odd) & TMEM_MASK & ~1u));
		if (t->fmt == G_IM_FMT_IA || t->fmt == G_IM_FMT_I)
			return ia16(p);
		if (t->fmt == G_IM_FMT_YUV)
			return 0x000000FF;
		return rgba16(p);
	}
	default: {
		uint32_t idx = ((t->tmem / 2 + (y & 0xFF) * (t->line / 2) + x) ^ (odd ? 2 : 0)) & 0x3FF;
		uint32_t rg = salsa64_rd16(g->tmem + idx * 2), ba = salsa64_rd16(g->tmem + 0x800 + idx * 2);
		return rg << 16 | ba;
	}
	}
}

static uint32_t tile_key(const struct gfx_state *g, const struct gfx_tile *t, uint32_t w, uint32_t h, unsigned tt)
{
	uint32_t hk = 0x811C9DC5u;
	hk = mix(hk, (uint32_t)t->fmt << 24 | (uint32_t)t->siz << 16 | t->pal << 8 | tt);
	hk = mix(hk, (uint32_t)t->line << 16 | t->tmem);
	hk = mix(hk, w << 16 | h);
	uint32_t bytes = h * t->line;
	if (bytes == 0 || bytes > 4096)
		bytes = 4096;
	uint32_t first = (t->tmem & TMEM_MASK) >> 6, nblocks = (bytes + 63) / 64 + 1;
	if (nblocks > 64)
		nblocks = 64;
	for (uint32_t i = 0; i < nblocks; i++)
		hk = mix(hk, g->tmem_key[(first + i) & 63]);
	if (t->siz == G_IM_SIZ_32b)
		for (uint32_t i = 0; i < nblocks && i < 32; i++)
			hk = mix(hk, g->tmem_key[32 + ((first + i) & 31)]);
	if (tt != G_TT_NONE) {
		/* Paleta: el banco de la tile (4 bits) o toda (8 bits). */
		uint32_t b0 = t->siz == G_IM_SIZ_4b ? 32 + t->pal * 2 : 32, nb = t->siz == G_IM_SIZ_4b ? 2 : 32;
		for (uint32_t i = 0; i < nb; i++)
			hk = mix(hk, g->tmem_key[(b0 + i) & 63]);
	}
	return hk | 1;
}

bool gfx_tile_texture(struct gfx_state *g, int tile, struct gfx_tex *tex)
{
	const struct gfx_tile *t = &g->tiles[tile & 7];
	uint32_t w, h;
	gfx_tile_size(t, &w, &h);
	uint32_t pitch = (w * 4 + 63) & ~63u;
	if (pitch * h > GFX_TEX_MAX)
		h = GFX_TEX_MAX / pitch;
	if (h == 0)
		return false;
	/* La paleta se usa con texturas de 4 y 8 bits si el othermode la activa. */
	unsigned tt = (g->oh >> G_MDSFT_TEXTLUT) & 3;
	if (t->siz > G_IM_SIZ_8b || tt < G_TT_RGBA16)
		tt = G_TT_NONE;
	uint32_t key = tile_key(g, t, w, h, tt);

	for (uint32_t i = 0; i < g->ntexcache; i++)
		if (g->texcache[i].key == key) {
			*tex = g->texcache[i].tex;
			g->res->tex_reused++;
			return true;
		}

	uint32_t size = pitch * h;
	uint8_t *out = gfx_io_tex_begin(g, size);
	if (!out)
		return false;
	for (uint32_t y = 0; y < h; y++) {
		uint8_t *row = out + y * pitch;
		for (uint32_t x = 0; x < w; x++)
			salsa64_wr32(row + 4 * x, texel(g, t, x, y, tt));
	}
	int32_t off = gfx_io_tex_commit(g, size);
	if (off < 0)
		return false;
	memset(tex, 0, sizeof *tex);
	tex->offset = (uint32_t)off;
	tex->width = (uint16_t)w;
	tex->height = (uint16_t)h;
	tex->pitch = (uint16_t)pitch;
	tex->key = key;
	g->res->textures++;
	if (g->ntexcache < GFX_TEX_CACHE) {
		g->texcache[g->ntexcache].key = key;
		g->texcache[g->ntexcache].tex = *tex;
		g->ntexcache++;
	}
	return true;
}
