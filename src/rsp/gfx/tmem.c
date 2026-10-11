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
 * Caché: la textura de una tile se identifica por un hash de los bytes de
 * la TMEM que lee su decodificación (texels y paleta) y de sus parámetros
 * (texcache.c). La misma identidad reutiliza la textura ya decodificada, en
 * la misma tarea o en una anterior; una tile que no cambió desde la última
 * vez (sin cargas ni cambios de tiles) ni siquiera se vuelve a calcular.
 */
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"

#define TMEM_MASK 0xFFFu

/* Mitad de una palabra de 64 bits de la TMEM (alineada a 8). */
typedef uint32_t __attribute__((may_alias, aligned(4))) tword;

static inline void swap_halves(uint8_t *tmem, uint32_t addr)
{
	tword *q = (tword *)(void *)(tmem + (addr & TMEM_MASK & ~7u));
	uint32_t a = q[0];
	q[0] = q[1];
	q[1] = a;
}

/* `bytes` (múltiplo de 8) a la TMEM desde `dst` (alineado a 8); lo que pasa
 * de los 4 KB sigue desde el principio. */
static void tmem_copy(uint8_t *tmem, uint32_t dst, const uint8_t *src, uint32_t bytes)
{
	dst &= TMEM_MASK;
	uint32_t n = 4096 - dst < bytes ? 4096 - dst : bytes;
	memcpy(tmem + dst, src, n);
	memcpy(tmem, src + n, bytes - n);
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

/* Anota una carga: el tramo [tmem, tmem + bytes) de la TMEM vino de `addr`
 * en filas de `line` bytes, de una imagen con `row_bytes` por fila. */
#ifdef SALSA64_GFX_TRACE
/* Depuración en el host (src/host/run.c): cada carga a la TMEM. */
void salsa64_gfx_trace_load(const struct gfx_state *g, uint32_t addr, uint32_t bytes, uint32_t line);
#endif

static void note_load(struct gfx_state *g, uint32_t tmem, uint32_t bytes, uint32_t line, uint32_t addr,
		      uint32_t row_bytes)
{
#ifdef SALSA64_GFX_TRACE
	salsa64_gfx_trace_load(g, addr, bytes, line);
#endif
	struct gfx_load *l = &g->loads[g->load_next++ & (GFX_LOADS - 1)];
	l->addr = addr;
	l->row_bytes = row_bytes;
	l->tmem = (uint16_t)(tmem & TMEM_MASK);
	l->bytes = (uint16_t)(bytes > 4096 ? 4096 : bytes);
	l->line = (uint16_t)line;  /* 0: LOADBLOCK, texels seguidos */
	l->pitch = g->timg_width;
	l->siz = g->timg_siz;
	l->fmt = g->timg_fmt;
}

/* De qué carga lee la tile (la más reciente que escribió su inicio en la
 * TMEM con las mismas filas) y dónde está en la RDRAM su texel (0, 0). */
static void provenance(const struct gfx_state *g, const struct gfx_tile *t, struct gfx_tex *tex)
{
	for (uint32_t k = 1; k <= GFX_LOADS; k++) {
		const struct gfx_load *l = &g->loads[(g->load_next - k) & (GFX_LOADS - 1)];
		if (!l->bytes || l->siz != t->siz || !t->line)
			continue;
		if (t->tmem < l->tmem || t->tmem >= (uint32_t)l->tmem + l->bytes)
			continue;
		uint32_t off = t->tmem - l->tmem;
		if (l->line == 0) {
			/* LOADBLOCK: texels seguidos (libultra manda ancho 1 en SETTIMG):
			 * las filas son las de la tile que lee, y el paso de la imagen
			 * de origen, el ancho de esa fila. */
			tex->src_addr = l->addr + off;
			tex->src_pitch = (uint16_t)(t->line * 2 / half_bytes(t->siz));
		} else {
			if (l->line != t->line)
				continue;
			tex->src_addr = l->addr + (off / l->line) * l->row_bytes + off % l->line;
			tex->src_pitch = l->pitch;
		}
		tex->src_siz = l->siz;
		tex->src_fmt = l->fmt;
		return;
	}
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
	g->tex_gen++;
	g->res->loads++;
	g->res->load_reads++;
	const uint8_t *p = gfx_io_read(g, GFX_IO_TEX, src, bytes);
	if (!p) {
		g->res->dl_errors++;
		return;
	}
	/* Los texels van seguidos: línea 0 (la tile que lea decide las filas). */
	note_load(g, t->tmem, bytes, 0, src, 0);
	if (g->timg_siz == G_IM_SIZ_32b) {
		uint32_t base = t->tmem / 2;
		for (uint32_t k = 0; k < bytes / 8; k++) {
			uint32_t x = ((k * dxt) >> 11) & 1 ? 2 : 0;
			put32(g->tmem, (base + 2 * k) ^ x, p + 8 * k);
			put32(g->tmem, (base + 2 * k + 1) ^ x, p + 8 * k + 4);
		}
		return;
	}
	tmem_copy(g->tmem, t->tmem, p, bytes);
	if (dxt)
		for (uint32_t k = 0; k < bytes / 8; k++)
			if (((k * dxt) >> 11) & 1)
				swap_halves(g->tmem, t->tmem + 8 * k);
}

void gfx_load_tile(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	struct gfx_tile *t = &g->tiles[(w1 >> 24) & 7];
	g->tex_gen++;
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
	if (row_bytes > 4096)
		row_bytes = 4096;
	g->res->loads++;
	note_load(g, t->tmem, rows * t->line, t->line, g->timg_addr + ((y0 * g->timg_width + x0) * hb) / 2,
		  (uint32_t)g->timg_width * hb / 2);

	for (uint32_t r = 0; r < rows; r++) {
		uint32_t src = g->timg_addr + (((y0 + r) * g->timg_width + x0) * hb) / 2;
		g->res->load_reads++;
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
			continue;
		}
		uint32_t dst = t->tmem + r * t->line;
		tmem_copy(g->tmem, dst, p, row_bytes);
		if (r & 1)
			for (uint32_t k = 0; k < row_bytes; k += 8)
				swap_halves(g->tmem, dst + k);
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
	g->tex_gen++;
	g->res->loads++;
	g->res->load_reads++;
	const uint8_t *p = gfx_io_read(g, GFX_IO_TEX, src, count * 2);
	if (!p) {
		g->res->dl_errors++;
		return;
	}
	for (uint32_t e = 0; e < count; e++) {
		uint32_t v;
		memcpy(&v, p + 2 * e, 2);
		memcpy((uint8_t *)&v + 2, p + 2 * e, 2);
		tword *q = (tword *)(void *)(g->tmem + (0x800 | ((t->tmem + 8 * e) & 0x7FF)));
		q[0] = v;
		q[1] = v;
	}
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

/* Hash de lo que lee texel() para la tile: las filas de texels (con la
 * paleta, si se usa) y los parámetros que cambian el resultado. */
static void tile_key(const struct gfx_state *g, const struct gfx_tile *t, uint32_t w, uint32_t h, unsigned tt,
		     bool premul, uint32_t key[2])
{
	struct gfx_hash hs;
	gfx_hash_init(&hs);
	/* Versión de la decodificación (1) y si va premultiplicada. */
	gfx_hash_words(&hs, (uint32_t)t->fmt << 24 | (uint32_t)t->siz << 16 | (uint32_t)t->pal << 8 | tt,
		       (uint32_t)t->line << 16 | (t->tmem & 15), w << 16 | h, 1u | (uint32_t)premul << 31);
	/* texel() lee por fila 8 bytes enteros (la fila impar intercambia
	 * mitades dentro de cada palabra) y solo 256 filas distintas. */
	uint32_t row = t->siz == G_IM_SIZ_4b ? (w + 1) / 2 : t->siz == G_IM_SIZ_8b ? w : 2 * w;
	uint32_t rows = h < 256 ? h : 256;
	uint32_t len = (rows - 1) * t->line + ((row + 7) & ~7u);
	if (t->siz == G_IM_SIZ_32b) {
		/* R y G en la mitad baja, B y A en la alta, en el mismo lugar. */
		gfx_hash_ring(&hs, g->tmem, 0x800, t->tmem, len);
		gfx_hash_ring(&hs, g->tmem + 0x800, 0x800, t->tmem, len);
	} else {
		gfx_hash_ring(&hs, g->tmem, 4096, t->tmem, len);
	}
	if (tt != G_TT_NONE) {
		/* Paleta: el banco de la tile (4 bits) o toda (8 bits). */
		if (t->siz == G_IM_SIZ_4b)
			gfx_hash_ring(&hs, g->tmem, 4096, 0x800 + (uint32_t)t->pal * 128, 128);
		else
			gfx_hash_ring(&hs, g->tmem, 4096, 0x800, 0x800);
	}
	gfx_hash_final(&hs, key);
}

/* RGB·A (0xRRGGBBAA), redondeado. */
static uint32_t premultiply(uint32_t c)
{
	uint32_t a = c & 0xFF;
	uint32_t r = ((c >> 24) * a + 127) / 255, g = (((c >> 16) & 0xFF) * a + 127) / 255;
	uint32_t b = (((c >> 8) & 0xFF) * a + 127) / 255;
	return r << 24 | g << 16 | b << 8 | a;
}

static void decode(const struct gfx_state *g, const struct gfx_tile *t, uint32_t w, uint32_t h, uint32_t pitch,
		   unsigned tt, bool premul, uint8_t *out)
{
	for (uint32_t y = 0; y < h; y++) {
		uint8_t *row = out + y * pitch;
		for (uint32_t x = 0; x < w; x++) {
			uint32_t c = texel(g, t, x, y, tt);
			salsa64_wr32(row + 4 * x, premul ? premultiply(c) : c);
		}
	}
}

static bool resolve_content(struct gfx_state *g, const struct gfx_tile *t, unsigned tt, bool premul,
			    struct gfx_tex *tex);

/* La textura de la tile (contenido, de la caché o decodificado) y de
 * dónde vino. El origen se pone después: dos tiles con el mismo contenido
 * comparten textura pero pueden venir de distinto lugar. */
static bool resolve(struct gfx_state *g, const struct gfx_tile *t, unsigned tt, bool premul, struct gfx_tex *tex)
{
	if (!resolve_content(g, t, tt, premul, tex))
		return false;
	tex->src_addr = 0;
	tex->src_pitch = 0;
	tex->src_siz = tex->src_fmt = 0;
	provenance(g, t, tex);
	return true;
}

static bool resolve_content(struct gfx_state *g, const struct gfx_tile *t, unsigned tt, bool premul,
			    struct gfx_tex *tex)
{
	uint32_t w, h;
	gfx_tile_size(t, &w, &h);
	uint32_t pitch = (w * 4 + 63) & ~63u;
	if (pitch * h > GFX_TEX_MAX)
		h = GFX_TEX_MAX / pitch;
	if (h == 0)
		return false;
	uint32_t t0 = gfx_io_clock();
	uint32_t key[2];
	tile_key(g, t, w, h, tt, premul, key);
	memset(tex, 0, sizeof *tex);
	tex->width = (uint16_t)w;
	tex->height = (uint16_t)h;
	tex->pitch = (uint16_t)pitch;
	tex->key = key[0];

	/* Ya decodificada: en el arena de esta tarea o en la caché. */
	const struct gfx_tc_entry *found = NULL;
	bool old = false;
	for (uint32_t i = 0; i < g->narena_tex; i++)
		if (g->arena_tex[i].key[0] == key[0] && g->arena_tex[i].key[1] == key[1]) {
			*tex = g->arena_tex[i].tex;
			g->res->tex_reused++;
			g->res->t_hash += gfx_io_clock() - t0;
			return true;
		}
	if (g->tc_on)
		found = gfx_tc_find(g, key, &old);
	g->res->t_hash += gfx_io_clock() - t0;
	if (found) {
		tex->where = GFX_TEX_CACHE;
		tex->offset = found->off;
		g->res->tex_reused++;
		g->res->tex_old += old;
		return true;
	}

	uint32_t size = pitch * h;
	uint8_t *out = gfx_io_tex_begin(g, size);
	if (!out)
		return false;
	t0 = gfx_io_clock();
	decode(g, t, w, h, pitch, tt, premul, out);
	struct gfx_tc_entry *e = g->tc_on ? gfx_tc_insert(g, key, size) : NULL;
	if (e) {
		e->width = (uint16_t)w;
		e->height = (uint16_t)h;
		e->pitch = (uint16_t)pitch;
		gfx_io_tex_cache(g, e->off, size);
		tex->where = GFX_TEX_CACHE;
		tex->offset = e->off;
	} else {
		int32_t off = gfx_io_tex_commit(g, size);
		if (off < 0) {
			g->res->t_tex += gfx_io_clock() - t0;
			return false;
		}
		tex->where = GFX_TEX_ARENA;
		tex->offset = (uint32_t)off;
		g->res->tex_arena += g->tc_on;
		if (g->narena_tex < GFX_TEX_ARENA) {
			g->arena_tex[g->narena_tex].key[0] = key[0];
			g->arena_tex[g->narena_tex].key[1] = key[1];
			g->arena_tex[g->narena_tex].tex = *tex;
			g->narena_tex++;
		}
	}
	g->res->t_tex += gfx_io_clock() - t0;
	g->res->textures++;
	return true;
}

bool gfx_tile_texture(struct gfx_state *g, int tile, struct gfx_tex *tex, bool premul)
{
	const struct gfx_tile *t = &g->tiles[tile & 7];
	/* La paleta se usa con texturas de 4 y 8 bits si el othermode la activa. */
	unsigned tt = (g->oh >> G_MDSFT_TEXTLUT) & 3;
	if (t->siz > G_IM_SIZ_8b || tt < G_TT_RGBA16)
		tt = G_TT_NONE;
	struct gfx_tilememo *m = &g->tilememo[tile & 7];
	if (m->ok && m->gen == g->tex_gen && m->tt == tt && m->premul == premul) {
		*tex = m->tex;
		g->res->tex_reused++;
		return true;
	}
	if (!resolve(g, t, tt, premul, tex))
		return false;
	m->ok = 1;
	m->gen = g->tex_gen;
	m->tt = (uint8_t)tt;
	m->premul = premul;
	m->tex = *tex;
	return true;
}
