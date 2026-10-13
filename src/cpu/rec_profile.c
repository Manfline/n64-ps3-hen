/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — perfil persistente del recompilador. Ver rec_profile.h.
 *
 * Archivo (versión 2, big-endian, campo por campo; sin bytes del juego):
 *   cabecera (32): "SPRF" | versión u32 | entradas u32 | sitios u32 |
 *                  código del cartucho (4) | CRC1 u32 | CRC2 u32 | reservado u32
 *   por entrada (24): vpc u32 | huella u64 | puntaje u32 | n u16 | palabras u16 |
 *                     banderas u8 | sucesores u8 | reservado u16
 *     por sucesor (16): destino u32 | huella del bloque de ahí u64 | cuenta u32
 *   por sitio (16): entrada u16 (índice + 1) | k u8 | clase u8 | destino u32 |
 *                   tomado u32 | no tomado u32
 *   suma de verificación u64 (FNV-1a de todo lo anterior)
 * Lo que no pase las comprobaciones (magia, versión, identidad del juego,
 * cantidades, longitud, suma) se descarta entero.
 */
#include "cpu/rec_profile.h"

#include <string.h>

#define MAGIC    "SPRF"
#define VERSION  2u
#define HDR_LEN  32u
#define ENT_LEN  24u
#define SUCC_LEN 16u
#define SITE_LEN 16u
#define SUM_LEN  8u
#define MAX_SUCC 2u
#define CHUNK    512u    /* entradas o sitios por trozo escrito */

#define FNV_BASIS 0xCBF29CE484222325ull
#define FNV_PRIME 0x100000001B3ull

static uint64_t fnv_bytes(uint64_t h, const uint8_t *b, size_t len)
{
	for (size_t k = 0; k < len; k++)
		h = (h ^ b[k]) * FNV_PRIME;
	return h;
}

static uint32_t sat_add(uint32_t a, uint32_t b)
{
	uint64_t s = (uint64_t)a + b;
	return s > UINT32_MAX ? UINT32_MAX : (uint32_t)s;
}

void salsa64_rec_profile_init(struct salsa64_rec_profile *p)
{
	memset(p, 0, sizeof *p);
}

void salsa64_rec_profile_game_of(struct salsa64_rec_profile_game *g, const uint8_t *rom)
{
	/* Cabecera de la ROM: CRC1 en 0x10, CRC2 en 0x14, código en 0x3B–0x3E. */
	memcpy(g->code, rom + 0x3B, 4);
	g->crc1 = (uint32_t)rom[0x10] << 24 | (uint32_t)rom[0x11] << 16 | (uint32_t)rom[0x12] << 8 | rom[0x13];
	g->crc2 = (uint32_t)rom[0x14] << 24 | (uint32_t)rom[0x15] << 16 | (uint32_t)rom[0x16] << 8 | rom[0x17];
}

uint64_t salsa64_rec_profile_hash(const uint32_t *words, unsigned n)
{
	uint64_t h = FNV_BASIS ^ n;
	for (unsigned k = 0; k < n; k++) {
		uint32_t w = words[k];
		h = (h ^ (w & 0xFF)) * FNV_PRIME;
		h = (h ^ ((w >> 8) & 0xFF)) * FNV_PRIME;
		h = (h ^ ((w >> 16) & 0xFF)) * FNV_PRIME;
		h = (h ^ (w >> 24)) * FNV_PRIME;
	}
	/* Mezcla final: que los bits altos también dependan de todo. */
	h ^= h >> 32;
	h *= 0x9E3779B97F4A7C15ull;
	h ^= h >> 29;
	return h;
}

static unsigned slot_of(uint32_t vpc)
{
	return ((vpc >> 2) * 2654435761u >> 12) & (SALSA64_RPF_SLOTS - 1);
}

unsigned salsa64_rec_profile_lookup(struct salsa64_rec_profile *p, uint32_t vpc, uint64_t hash, unsigned n,
				    unsigned words, bool add)
{
	unsigned s = slot_of(vpc);
	for (;;) {
		unsigned i = p->index[s];
		if (!i)
			break;
		const struct salsa64_rec_profile_entry *e = &p->entries[i - 1];
		if (e->vpc == vpc && e->hash == hash && e->n == n)
			return i;
		s = (s + 1) & (SALSA64_RPF_SLOTS - 1);
	}
	if (!add)
		return 0;
	if (p->count >= SALSA64_RPF_ENTRIES) {
		p->full++;
		return 0;
	}
	struct salsa64_rec_profile_entry *e = &p->entries[p->count];
	memset(e, 0, sizeof *e);
	e->hash = hash;
	e->vpc = vpc;
	e->n = (uint16_t)n;
	e->words = (uint16_t)(words > 0xFFFF ? 0xFFFF : words);
	/* Primero la entrada y el índice, después la cuenta: quien escribe el
	 * archivo desde otro hilo lee `count` una vez y nunca ve una entrada a
	 * medias. */
	p->index[s] = (uint16_t)(p->count + 1);
	__sync_synchronize();
	p->count++;
	p->dirty = true;
	return p->count;
}

void salsa64_rec_profile_set_words(struct salsa64_rec_profile *p, unsigned idx, unsigned words)
{
	if (idx && idx <= p->count)
		p->entries[idx - 1].words = (uint16_t)(words > 0xFFFF ? 0xFFFF : words);
}

void salsa64_rec_profile_set_flags(struct salsa64_rec_profile *p, unsigned idx, unsigned flags)
{
	if (idx && idx <= p->count)
		p->entries[idx - 1].flags |= (uint8_t)flags;
}

void salsa64_rec_profile_count(struct salsa64_rec_profile *p, unsigned idx, uint32_t hits)
{
	if (!idx || idx > p->count || !hits)
		return;
	struct salsa64_rec_profile_entry *e = &p->entries[idx - 1];
	e->hits = sat_add(e->hits, hits);
	p->session += hits;
	p->dirty = true;
}

unsigned salsa64_rec_profile_site(struct salsa64_rec_profile *p, unsigned idx, unsigned k, unsigned kind,
				  uint32_t target)
{
	if (!idx || idx > p->count || k > 255 || kind >= SALSA64_RPF_SITE_KINDS)
		return 0;
	struct salsa64_rec_profile_entry *e = &p->entries[idx - 1];
	for (unsigned j = e->site; j; j = p->sites[j - 1].next) {
		const struct salsa64_rec_profile_site *s = &p->sites[j - 1];
		if (s->k == k && s->kind == kind && s->target == target)
			return j;
	}
	if (e->nsites >= SALSA64_RPF_BLOCK_SITES) {
		e->flags |= SALSA64_RPF_SITES_OVER;
		return 0;
	}
	if (p->nsites >= SALSA64_RPF_SITES) {
		p->sites_full++;
		return 0;
	}
	struct salsa64_rec_profile_site *s = &p->sites[p->nsites];
	memset(s, 0, sizeof *s);
	s->target = target;
	s->entry = (uint16_t)idx;
	s->k = (uint8_t)k;
	s->kind = (uint8_t)kind;
	s->next = e->site;
	__sync_synchronize();  /* el sitio completo antes de que lo vea quien escribe (lee nsites una vez) */
	p->nsites++;
	e->site = (uint16_t)p->nsites;
	e->nsites++;
	p->dirty = true;
	return p->nsites;
}

uint32_t salsa64_rec_profile_merge(uint32_t score, uint32_t hits, bool long_session)
{
	if (!long_session)
		return score > hits ? score : hits;
	uint32_t avg = (uint32_t)(((uint64_t)score + hits) / 2);
	return avg > hits ? avg : hits;
}

/* Palabras que suman las entradas con puntaje ≥ t. */
static uint64_t words_at_least(const struct salsa64_rec_profile *p, uint32_t t)
{
	uint64_t w = 0;
	for (uint32_t k = 0; k < p->count; k++)
		if (p->entries[k].score >= t)
			w += p->entries[k].words;
	return w;
}

void salsa64_rec_profile_mark_hot(struct salsa64_rec_profile *p, uint32_t cap_words)
{
	uint32_t max = 0;
	p->hot_entries = p->hot_words = p->hot_threshold = 0;
	for (uint32_t k = 0; k < p->count; k++) {
		p->entries[k].hot = 0;
		if (p->entries[k].score > max)
			max = p->entries[k].score;
	}
	if (!cap_words || !max)
		return;
	/* El menor umbral t ≥ 1 con el que lo que queda cabe (las palabras son
	 * decrecientes en t). */
	uint32_t lo = 1, hi = max;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (words_at_least(p, mid) <= cap_words)
			hi = mid;
		else
			lo = mid + 1;
	}
	uint64_t sum = 0;
	for (uint32_t k = 0; k < p->count; k++) {
		struct salsa64_rec_profile_entry *e = &p->entries[k];
		if (e->score < lo || sum + e->words > cap_words)
			continue;
		e->hot = 1;
		sum += e->words;
		p->hot_entries++;
	}
	p->hot_words = (uint32_t)sum;
	p->hot_threshold = lo;
}

/* ---- Lo que se guarda de cada sitio ---------------------------------------------- */

/* Cuántas veces se tomó el sitio en total (archivo más esta sesión). */
static uint32_t site_taken(const struct salsa64_rec_profile_site *s)
{
	return sat_add(s->taken0, s->taken);
}

/* Lo no tomado en total de un salto condicional en el sitio `j` (índice +
 * 1) de la entrada `e`: lo del archivo más lo de esta sesión. El salto
 * final no tomado tiene su propio sitio (FALL); un salto interno no, y se
 * deriva: las entradas al bloque menos lo que salió por los saltos
 * anteriores (aproximado si el bloque tiene saltos a su propio interior). */
static uint32_t site_not_taken(const struct salsa64_rec_profile *p, const struct salsa64_rec_profile_entry *e,
			       unsigned j)
{
	const struct salsa64_rec_profile_site *s = &p->sites[j - 1];
	if (s->kind != SALSA64_RPF_SITE_COND)
		return 0;
	uint32_t session;
	unsigned fall = 0;
	for (unsigned i = e->site; i; i = p->sites[i - 1].next)
		if (p->sites[i - 1].kind == SALSA64_RPF_SITE_FALL && p->sites[i - 1].k == s->k)
			fall = i;
	if (fall) {
		session = p->sites[fall - 1].taken;
	} else {
		uint64_t reach = e->hits;
		for (unsigned i = e->site; i; i = p->sites[i - 1].next) {
			const struct salsa64_rec_profile_site *o = &p->sites[i - 1];
			if (o->kind == SALSA64_RPF_SITE_COND && o->k < s->k)
				reach = reach > o->taken ? reach - o->taken : 0;
		}
		session = reach > s->taken ? (uint32_t)(reach - s->taken) : 0;
	}
	return sat_add(s->not_taken0, session);
}

/* Los MAX_SUCC sucesores más frecuentes de la entrada: sitios con destino
 * resuelto fuera del bloque, por cuenta total decreciente. Devuelve cuántos. */
static unsigned successors(const struct salsa64_rec_profile *p, const struct salsa64_rec_profile_entry *e,
			   unsigned out[MAX_SUCC])
{
	unsigned n = 0;
	for (unsigned i = e->site; i; i = p->sites[i - 1].next) {
		const struct salsa64_rec_profile_site *s = &p->sites[i - 1];
		if (!s->succ_hash || !site_taken(s) || (s->target - e->vpc) < 4u * e->n)
			continue;
		unsigned pos = n;
		while (pos > 0 && site_taken(&p->sites[out[pos - 1] - 1]) < site_taken(s))
			pos--;
		if (pos >= MAX_SUCC)
			continue;
		for (unsigned q = (n < MAX_SUCC ? n : MAX_SUCC - 1); q > pos; q--)
			out[q] = out[q - 1];
		out[pos] = i;
		if (n < MAX_SUCC)
			n++;
	}
	return n;
}

/* ---- Archivo ----------------------------------------------------------------- */

static void put32(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)(v >> 24);
	b[1] = (uint8_t)(v >> 16);
	b[2] = (uint8_t)(v >> 8);
	b[3] = (uint8_t)v;
}

static void put64(uint8_t *b, uint64_t v)
{
	put32(b, (uint32_t)(v >> 32));
	put32(b + 4, (uint32_t)v);
}

static uint32_t get32(const uint8_t *b)
{
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static uint64_t get64(const uint8_t *b)
{
	return (uint64_t)get32(b) << 32 | get32(b + 4);
}

static bool game_is_set(const struct salsa64_rec_profile_game *g)
{
	return g->crc1 || g->crc2 || memcmp(g->code, "\0\0\0\0", 4) != 0;
}

bool salsa64_rec_profile_load(struct salsa64_rec_profile *p, const char *path,
			      const struct salsa64_rec_profile_game *game)
{
	uint8_t b[HDR_LEN];
	salsa64_rec_profile_init(p);
	if (game)
		p->game = *game;
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	bool ok = fread(b, HDR_LEN, 1, f) == 1 && memcmp(b, MAGIC, 4) == 0 && get32(b + 4) == VERSION;
	uint32_t count = ok ? get32(b + 8) : 0, nsites = ok ? get32(b + 12) : 0;
	uint64_t sum = ok ? fnv_bytes(FNV_BASIS, b, HDR_LEN) : 0;
	ok = ok && count <= SALSA64_RPF_ENTRIES && nsites <= SALSA64_RPF_SITES;
	if (ok && game && game_is_set(game))
		ok = memcmp(b + 16, game->code, 4) == 0 && get32(b + 20) == game->crc1 && get32(b + 24) == game->crc2;
	if (ok && !(game && game_is_set(game))) {
		memcpy(p->game.code, b + 16, 4);
		p->game.crc1 = get32(b + 20);
		p->game.crc2 = get32(b + 24);
	}
	for (uint32_t k = 0; ok && k < count; k++) {
		if (fread(b, ENT_LEN, 1, f) != 1) {
			ok = false;
			break;
		}
		sum = fnv_bytes(sum, b, ENT_LEN);
		uint32_t vpc = get32(b);
		uint64_t hash = get64(b + 4);
		uint32_t score = get32(b + 12);
		unsigned n = get32(b + 16) >> 16, words = get32(b + 16) & 0xFFFF;
		unsigned flags = b[20], nsucc = b[21];
		if (!n || nsucc > MAX_SUCC) {
			ok = false;
			break;
		}
		unsigned i = salsa64_rec_profile_lookup(p, vpc, hash, n, words, true);
		if (i) {
			p->entries[i - 1].score = score;  /* repetida: se queda la última */
			p->entries[i - 1].flags = (uint8_t)flags;
		}
		for (unsigned q = 0; q < nsucc; q++) {  /* los sucesores se derivan de los sitios: solo se validan */
			if (fread(b, SUCC_LEN, 1, f) != 1) {
				ok = false;
				break;
			}
			sum = fnv_bytes(sum, b, SUCC_LEN);
		}
	}
	for (uint32_t k = 0; ok && k < nsites; k++) {
		if (fread(b, SITE_LEN, 1, f) != 1) {
			ok = false;
			break;
		}
		sum = fnv_bytes(sum, b, SITE_LEN);
		unsigned entry = get32(b) >> 16, sk = b[2], kind = b[3];
		if (!entry || entry > p->count || kind >= SALSA64_RPF_SITE_KINDS) {
			ok = false;
			break;
		}
		unsigned j = salsa64_rec_profile_site(p, entry, sk, kind, get32(b + 4));
		if (j) {
			p->sites[j - 1].taken0 = get32(b + 8);
			p->sites[j - 1].not_taken0 = get32(b + 12);
		}
	}
	if (ok) {
		uint8_t s[SUM_LEN], extra;
		ok = fread(s, SUM_LEN, 1, f) == 1 && get64(s) == sum && fread(&extra, 1, 1, f) == 0;
	}
	fclose(f);
	if (!ok) {
		salsa64_rec_profile_init(p);
		if (game)
			p->game = *game;
		return false;
	}
	p->loaded = p->count;
	p->dirty = false;
	return true;
}

static bool write_sum(FILE *f, uint64_t *sum, const uint8_t *b, size_t len)
{
	*sum = fnv_bytes(*sum, b, len);
	return fwrite(b, len, 1, f) == 1;
}

bool salsa64_rec_profile_save_begin(const struct salsa64_rec_profile *p, struct salsa64_rec_profile_writer *w,
				    const char *path)
{
	uint8_t b[HDR_LEN];
	if (w->f)
		salsa64_rec_profile_save_abort(w);
	memset(w, 0, sizeof *w);
	if (snprintf(w->path, sizeof w->path, "%s", path) >= (int)sizeof w->path)
		return false;
	snprintf(w->tmp, sizeof w->tmp, "%s.tmp", w->path);
	w->count = p->count;
	w->nsites = p->nsites;
	__sync_synchronize();  /* las entradas y los sitios hasta ahí ya están completos */
	w->long_session = p->session >= SALSA64_RPF_LONG_SESSION;
	w->sum = FNV_BASIS;
	w->f = fopen(w->tmp, "wb");
	if (!w->f)
		return false;
	memcpy(b, MAGIC, 4);
	put32(b + 4, VERSION);
	put32(b + 8, w->count);
	put32(b + 12, w->nsites);
	memcpy(b + 16, p->game.code, 4);
	put32(b + 20, p->game.crc1);
	put32(b + 24, p->game.crc2);
	put32(b + 28, 0);
	if (!write_sum(w->f, &w->sum, b, HDR_LEN)) {
		salsa64_rec_profile_save_abort(w);
		return false;
	}
	return true;
}

/* Una entrada con sus sucesores en `b`; devuelve los bytes. */
static unsigned put_entry(const struct salsa64_rec_profile *p, uint32_t k, bool long_session, uint8_t *b)
{
	const struct salsa64_rec_profile_entry *e = &p->entries[k];
	unsigned succ[MAX_SUCC], nsucc = successors(p, e, succ);
	put32(b, e->vpc);
	put64(b + 4, e->hash);
	put32(b + 12, salsa64_rec_profile_merge(e->score, e->hits, long_session));
	put32(b + 16, (uint32_t)e->n << 16 | e->words);
	b[20] = e->flags;
	b[21] = (uint8_t)nsucc;
	b[22] = b[23] = 0;
	for (unsigned q = 0; q < nsucc; q++) {
		const struct salsa64_rec_profile_site *s = &p->sites[succ[q] - 1];
		uint8_t *c = b + ENT_LEN + q * SUCC_LEN;
		put32(c, s->target);
		put64(c + 4, s->succ_hash);
		put32(c + 12, site_taken(s));
	}
	return ENT_LEN + nsucc * SUCC_LEN;
}

static void put_site(const struct salsa64_rec_profile *p, uint32_t j, uint8_t *b)
{
	const struct salsa64_rec_profile_site *s = &p->sites[j];
	put32(b, (uint32_t)s->entry << 16 | (uint32_t)s->k << 8 | s->kind);
	put32(b + 4, s->target);
	put32(b + 8, site_taken(s));
	put32(b + 12, site_not_taken(p, &p->entries[s->entry - 1], j + 1));
}

int salsa64_rec_profile_save_step(const struct salsa64_rec_profile *p, struct salsa64_rec_profile_writer *w,
				  unsigned max_items)
{
	uint8_t buf[CHUNK * (ENT_LEN + MAX_SUCC * SUCC_LEN)];
	if (!w->f)
		return -1;
	while (max_items) {
		uint32_t total = w->phase == 0 ? w->count : w->nsites;
		if (w->pos >= total) {
			if (w->phase == 1)
				break;
			w->phase = 1;
			w->pos = 0;
			continue;
		}
		unsigned n = total - w->pos;
		if (n > CHUNK)
			n = CHUNK;
		if (n > max_items)
			n = max_items;
		size_t len = 0;
		for (unsigned k = 0; k < n; k++) {
			if (w->phase == 0)
				len += put_entry(p, w->pos + k, w->long_session, buf + len);
			else
				put_site(p, w->pos + k, buf + len), len += SITE_LEN;
		}
		if (!write_sum(w->f, &w->sum, buf, len)) {
			salsa64_rec_profile_save_abort(w);
			return -1;
		}
		w->pos += n;
		max_items -= n;
	}
	if (w->phase == 0 || w->pos < w->nsites)
		return 0;
	put64(buf, w->sum);
	bool ok = fwrite(buf, SUM_LEN, 1, w->f) == 1;
	ok = fclose(w->f) == 0 && ok;
	w->f = NULL;
	if (ok) {
		remove(w->path);
		ok = rename(w->tmp, w->path) == 0;
	}
	if (!ok)
		remove(w->tmp);
	return ok ? 1 : -1;
}

void salsa64_rec_profile_save_abort(struct salsa64_rec_profile_writer *w)
{
	if (!w->f)
		return;
	fclose(w->f);
	w->f = NULL;
	remove(w->tmp);
}

bool salsa64_rec_profile_save(const struct salsa64_rec_profile *p, const char *path)
{
	struct salsa64_rec_profile_writer w;
	memset(&w, 0, sizeof w);
	if (!salsa64_rec_profile_save_begin(p, &w, path))
		return false;
	int r;
	while ((r = salsa64_rec_profile_save_step(p, &w, CHUNK)) == 0)
		;
	return r == 1;
}
