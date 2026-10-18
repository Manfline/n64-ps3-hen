/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — caché de texturas decodificadas entre tareas y su hash.
 *
 * Identidad: un hash de 64 bits de los bytes de la TMEM que lee la
 * decodificación de una tile (texels y paleta) y de sus parámetros. Si la
 * RDRAM de origen o la paleta cambian, cambian los bytes cargados y con
 * ellos el hash: no hace falta vigilar las escrituras a la RDRAM (las del
 * CPU no pasan por ningún aviso) y el costo es leer lo que ya se cargó.
 *
 * Hash: ocho carriles de 32 bits con la ronda de xxHash32 (multiplicar,
 * rotar, multiplicar), dos bloques de 16 bytes por vuelta para no esperar la
 * latencia de la multiplicación. En el SPE va con SIMD; en el host y el PPE,
 * el mismo cálculo en C (mismo resultado). Constantes y ronda de xxHash
 * (Yann Collet, BSD-2; docs/THIRD_PARTY.md); código propio.
 *
 * Directorio (struct gfx_texcache, en el local store del SPE): entradas en
 * una cola en el orden en que ocupan la región, que se llena como anillo.
 * Al reservar se desaloja lo que hay delante de head_off; lo que aún puede
 * leer el consumidor (usado después de job->done_task: el RSX puede estar
 * dibujando con ello) no se toca: se salta y pasa al final de la cola. Si
 * no queda lugar en toda una vuelta, la textura va al arena de la tarea. El índice es una
 * tabla abierta (sondeo lineal, borrado con corrimiento hacia atrás).
 */
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"

#ifdef __SPU__
#include <spu_intrinsics.h>
#endif

#define P1 0x9E3779B1u
#define P2 0x85EBCA77u
#define P3 0xC2B2AE3Du
#define SEED 0x53344754u  /* "S4GT" */

static inline uint32_t rotl(uint32_t x, int r)
{
	return x << r | x >> (32 - r);
}

static uint32_t avalanche(uint32_t h)
{
	h ^= h >> 15;
	h *= P2;
	h ^= h >> 13;
	h *= P3;
	h ^= h >> 16;
	return h;
}

void gfx_hash_init(struct gfx_hash *h)
{
	for (int i = 0; i < 8; i++)
		h->acc[i] = SEED + (uint32_t)i * P1;
	h->len = 0;
}

#ifdef __SPU__

static inline vec_uint4 mul32(vec_uint4 a, vec_uint4 b)
{
	vec_uint4 hl = (vec_uint4)spu_mulh((vec_short8)a, (vec_short8)b);
	vec_uint4 lh = (vec_uint4)spu_mulh((vec_short8)b, (vec_short8)a);
	vec_uint4 ll = spu_mulo((vec_ushort8)a, (vec_ushort8)b);
	return spu_add(spu_add(hl, lh), ll);
}

static inline vec_uint4 round4(vec_uint4 acc, vec_uint4 v)
{
	acc = spu_add(acc, mul32(v, spu_splats(P2)));
	return mul32(spu_rl(acc, 13), spu_splats(P1));
}

/* Bloques de 16 bytes alineados: el par (2k, 2k + 1) va a los carriles
 * 0–3 y 4–7; si n es impar, el último va a 0–3. */
static void blocks(struct gfx_hash *h, const uint8_t *p, uint32_t n)
{
	vec_uint4 *acc = (vec_uint4 *)h->acc;
	vec_uint4 a = acc[0], b = acc[1];
	const vec_uint4 *q = (const vec_uint4 *)p;
	uint32_t i = 0;
	for (; i + 2 <= n; i += 2) {
		a = round4(a, q[i]);
		b = round4(b, q[i + 1]);
	}
	if (i < n)
		a = round4(a, q[i]);
	acc[0] = a;
	acc[1] = b;
}

#else

static inline uint32_t round1(uint32_t acc, uint32_t v)
{
	return rotl(acc + v * P2, 13) * P1;
}

static void blocks(struct gfx_hash *h, const uint8_t *p, uint32_t n)
{
	uint32_t *acc = h->acc;
	uint32_t i = 0;
	for (; i + 2 <= n; i += 2, p += 32)
		for (int l = 0; l < 4; l++) {
			acc[l] = round1(acc[l], salsa64_rd32(p + 4 * l));
			acc[4 + l] = round1(acc[4 + l], salsa64_rd32(p + 16 + 4 * l));
		}
	if (i < n)
		for (int l = 0; l < 4; l++)
			acc[l] = round1(acc[l], salsa64_rd32(p + 4 * l));
}

#endif

/* Bytes [a, b) de `base` (alineado a 16), sin dar la vuelta. Los bloques de
 * los bordes entran con los bytes de fuera en cero. */
static void span(struct gfx_hash *h, const uint8_t *base, uint32_t a, uint32_t b)
{
	uint8_t edge[16] __attribute__((aligned(16)));
	uint32_t qa = a & ~15u, qb = (b + 15) & ~15u;
	if (qa != a) {
		memset(edge, 0, sizeof edge);
		uint32_t end = b < qa + 16 ? b : qa + 16;
		memcpy(edge + (a - qa), base + a, end - a);
		blocks(h, edge, 1);
		qa += 16;
		if (qa >= b)
			return;
	}
	uint32_t full = (b & ~15u) > qa ? ((b & ~15u) - qa) / 16 : 0;
	if (full)
		blocks(h, base + qa, full);
	qa += full * 16;
	if (qa < qb) {
		memset(edge, 0, sizeof edge);
		memcpy(edge, base + qa, b - qa);
		blocks(h, edge, 1);
	}
}

void gfx_hash_ring(struct gfx_hash *h, const uint8_t *ring, uint32_t size, uint32_t start, uint32_t len)
{
	if (len > size)
		len = size;
	start &= size - 1;
	h->len += len;
	while (len) {
		uint32_t n = size - start < len ? size - start : len;
		span(h, ring, start, start + n);
		len -= n;
		start = 0;
	}
}

void gfx_hash_words(struct gfx_hash *h, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	uint8_t blk[16] __attribute__((aligned(16)));
	salsa64_wr32(blk, a);
	salsa64_wr32(blk + 4, b);
	salsa64_wr32(blk + 8, c);
	salsa64_wr32(blk + 12, d);
	blocks(h, blk, 1);
	h->len += 16;
}

void gfx_hash_final(const struct gfx_hash *h, uint32_t key[2])
{
	uint32_t k0 = SEED ^ h->len, k1 = P3 + h->len;
	for (int i = 0; i < 8; i++) {
		k0 = avalanche(k0 ^ rotl(h->acc[i], 3 * i + 1)) + P1;
		k1 = avalanche(k1 + h->acc[7 - i] * P2) ^ P3;
	}
	key[0] = k0;
	key[1] = k1;
}

/* ---- Directorio ------------------------------------------------------------ */

static void tc_reset(struct gfx_texcache *c, uint64_t ea, uint32_t cap)
{
	memset(c->index, 0, sizeof c->index);
	c->ea = ea;
	c->cap = cap;
	c->head_off = 0;
	c->tail = c->count = 0;
	c->fence_on = 0;
}

void gfx_tc_fence(struct gfx_state *g, uint64_t ea, uint32_t cap, uint32_t task)
{
	struct gfx_texcache *c = &g->tc;
	tc_reset(c, ea, cap & ~127u);
	c->fence = task;
	c->fence_on = 1;
}

void gfx_tc_begin(struct gfx_state *g)
{
	struct gfx_texcache *c = &g->tc;
	const struct gfx_job *j = g->job;
	uint32_t cap = j->cache_cap & ~127u;
	g->tc_on = j->cache_ea && cap && !(j->cache_ea & 127) && !(j->flags & GFX_JOB_NOCACHE);
	if (!g->tc_on)
		return;
	if (c->ea != j->cache_ea || c->cap != cap)
		tc_reset(c, j->cache_ea, cap);
	c->task = j->task;
	/* Lo de esta tarea nunca se desaloja, diga lo que diga el consumidor. */
	c->done_task = (int32_t)(j->done_task - (j->task - 1)) > 0 ? j->task - 1 : j->done_task;
	if (c->fence_on && (int32_t)(c->done_task - c->fence) >= 0)
		c->fence_on = 0;
}

static uint32_t home(const uint32_t key[2])
{
	return key[0] & (GFX_TC_SLOTS - 1);
}

/* El consumidor aún puede leerla. */
static bool busy(const struct gfx_texcache *c, const struct gfx_tc_entry *e)
{
	return (int32_t)(e->last_task - c->done_task) > 0;
}

const struct gfx_tc_entry *gfx_tc_find(struct gfx_state *g, const uint32_t key[2], bool *old)
{
	struct gfx_texcache *c = &g->tc;
	for (uint32_t s = home(key);; s = (s + 1) & (GFX_TC_SLOTS - 1)) {
		uint32_t i = c->index[s];
		if (!i)
			return NULL;
		struct gfx_tc_entry *e = &c->e[i - 1];
		if (e->key[0] == key[0] && e->key[1] == key[1]) {
			*old = e->last_task != c->task;
			e->last_task = c->task;
			return e;
		}
	}
}

static void index_put(struct gfx_texcache *c, uint32_t pos)
{
	struct gfx_tc_entry *e = &c->e[pos];
	uint32_t s = home(e->key);
	while (c->index[s])
		s = (s + 1) & (GFX_TC_SLOTS - 1);
	c->index[s] = (uint16_t)(pos + 1);
	e->slot = (uint16_t)s;
}

/* Quita del índice la ranura s y corre hacia atrás las que dependían de
 * ella para que el sondeo lineal las siga encontrando. */
static void index_del(struct gfx_texcache *c, uint32_t s)
{
	const uint32_t m = GFX_TC_SLOTS - 1;
	c->index[s] = 0;
	for (uint32_t j = (s + 1) & m; c->index[j]; j = (j + 1) & m) {
		struct gfx_tc_entry *e = &c->e[c->index[j] - 1];
		uint32_t k = home(e->key);
		/* Se mueve si su lugar ideal no está en (s, j]. */
		bool stay = s <= j ? (k > s && k <= j) : (k > s || k <= j);
		if (stay)
			continue;
		c->index[s] = c->index[j];
		e->slot = (uint16_t)s;
		c->index[j] = 0;
		s = j;
	}
}

static struct gfx_tc_entry *tail_entry(struct gfx_texcache *c)
{
	return &c->e[c->tail];
}

static void evict_tail(struct gfx_state *g)
{
	struct gfx_texcache *c = &g->tc;
	index_del(c, tail_entry(c)->slot);
	c->tail = (c->tail + 1) % GFX_TC_ENTRIES;
	c->count--;
	g->res->tex_evicted++;
}

/* La entrada de la cola pasa al final, sin moverse de la región. */
static void rotate_tail(struct gfx_texcache *c)
{
	struct gfx_tc_entry *t = tail_entry(c);
	uint32_t pos = (c->tail + c->count) % GFX_TC_ENTRIES;
	if (pos != c->tail) {
		c->e[pos] = *t;
		c->index[t->slot] = (uint16_t)(pos + 1);
	}
	c->tail = (c->tail + 1) % GFX_TC_ENTRIES;
}

/* Una textura de `size` bytes cabe en head_off: no pisa la siguiente
 * entrada en la región ni pasa del final, y hay lugar en el directorio. Por
 * el orden de la cola basta con mirar la primera (las demás están después;
 * si la primera está detrás de head_off, todas lo están). */
static bool fits(struct gfx_texcache *c, uint32_t size)
{
	if (c->head_off + size > c->cap)
		return false;
	if (!c->count)
		return true;
	const struct gfx_tc_entry *t = tail_entry(c);
	return c->count < GFX_TC_ENTRIES && !(t->off >= c->head_off && t->off < c->head_off + size);
}

struct gfx_tc_entry *gfx_tc_insert(struct gfx_state *g, const uint32_t key[2], uint32_t size)
{
	struct gfx_texcache *c = &g->tc;
	size = (size + 127) & ~127u;
	if (!size || size > c->cap || c->fence_on)
		return NULL;
	/* Invariante: la cola, de tail en adelante, sigue el orden de la región
	 * empezando en head_off (lo de la vuelta anterior y luego lo de esta), y
	 * ninguna entrada cruza head_off. */
	for (uint32_t guard = 0; !fits(c, size); guard++) {
		if (guard > 2 * GFX_TC_ENTRIES + 4)
			return NULL;  /* todo en uso: al arena */
		if (c->head_off + size > c->cap) {
			/* Fin de la región: lo que queda delante se desaloja, salvo lo
			 * que está en uso, que pasa al final de la cola (sigue siendo
			 * lo último en el orden de la región), y se vuelve al principio. */
			for (uint32_t n = c->count; n && tail_entry(c)->off >= c->head_off; n--) {
				if (busy(c, tail_entry(c)))
					rotate_tail(c);
				else
					evict_tail(g);
			}
			c->head_off = 0;
			continue;
		}
		struct gfx_tc_entry *t = tail_entry(c);
		if (!busy(c, t)) {
			evict_tail(g);
		} else if (t->off >= c->head_off) {
			/* En uso: se salta por encima. */
			c->head_off = t->off + t->size;
			rotate_tail(c);
		} else {
			/* Directorio lleno y nada delante: se da la vuelta antes. */
			c->head_off = 0;
		}
	}
	uint32_t pos = (c->tail + c->count) % GFX_TC_ENTRIES;
	struct gfx_tc_entry *e = &c->e[pos];
	e->key[0] = key[0];
	e->key[1] = key[1];
	e->off = c->head_off;
	e->size = size;
	e->last_task = c->task;
	c->count++;
	c->head_off += size;
	index_put(c, pos);
	return e;
}
