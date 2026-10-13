/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — perfil persistente del recompilador. Ver rec_profile.h.
 *
 * Archivo (big-endian, campo por campo):
 *   "SPRF" | versión u32 | entradas u32 | reservado u32
 *   por entrada: vpc u32 | huella u64 | puntaje u32 | n u16 | palabras u16
 *   suma de verificación u64 (FNV-1a de todo lo anterior)
 * Lo que no pase las comprobaciones (magia, versión, cantidad, longitud,
 * suma) se descarta entero.
 */
#include "cpu/rec_profile.h"

#include <string.h>

#define MAGIC    "SPRF"
#define VERSION  1u
#define HDR_LEN  16u
#define ENT_LEN  20u
#define SUM_LEN  8u
#define CHUNK    1024u   /* entradas por trozo escrito */

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

void salsa64_rec_profile_count(struct salsa64_rec_profile *p, unsigned idx, uint32_t hits)
{
	if (!idx || idx > p->count || !hits)
		return;
	struct salsa64_rec_profile_entry *e = &p->entries[idx - 1];
	e->hits = sat_add(e->hits, hits);
	p->session += hits;
	p->dirty = true;
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

bool salsa64_rec_profile_load(struct salsa64_rec_profile *p, const char *path)
{
	uint8_t b[HDR_LEN > ENT_LEN ? HDR_LEN : ENT_LEN];
	salsa64_rec_profile_init(p);
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	bool ok = fread(b, HDR_LEN, 1, f) == 1 && memcmp(b, MAGIC, 4) == 0 && get32(b + 4) == VERSION;
	uint32_t count = ok ? get32(b + 8) : 0;
	uint64_t sum = ok ? fnv_bytes(FNV_BASIS, b, HDR_LEN) : 0;
	ok = ok && count <= SALSA64_RPF_ENTRIES;
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
		if (!n) {
			ok = false;
			break;
		}
		unsigned i = salsa64_rec_profile_lookup(p, vpc, hash, n, words, true);
		if (i)
			p->entries[i - 1].score = score;  /* repetida: se queda la última */
	}
	if (ok) {
		uint8_t s[SUM_LEN], extra;
		ok = fread(s, SUM_LEN, 1, f) == 1 && get64(s) == sum && fread(&extra, 1, 1, f) == 0;
	}
	fclose(f);
	if (!ok) {
		salsa64_rec_profile_init(p);
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
	__sync_synchronize();  /* las entradas hasta count ya están completas */
	w->long_session = p->session >= SALSA64_RPF_LONG_SESSION;
	w->sum = FNV_BASIS;
	w->f = fopen(w->tmp, "wb");
	if (!w->f)
		return false;
	memcpy(b, MAGIC, 4);
	put32(b + 4, VERSION);
	put32(b + 8, w->count);
	put32(b + 12, 0);
	if (!write_sum(w->f, &w->sum, b, HDR_LEN)) {
		salsa64_rec_profile_save_abort(w);
		return false;
	}
	return true;
}

int salsa64_rec_profile_save_step(const struct salsa64_rec_profile *p, struct salsa64_rec_profile_writer *w,
				  unsigned max_entries)
{
	uint8_t buf[CHUNK * ENT_LEN];
	if (!w->f)
		return -1;
	while (w->pos < w->count && max_entries) {
		unsigned n = w->count - w->pos;
		if (n > CHUNK)
			n = CHUNK;
		if (n > max_entries)
			n = max_entries;
		for (unsigned k = 0; k < n; k++) {
			const struct salsa64_rec_profile_entry *e = &p->entries[w->pos + k];
			uint8_t *b = buf + k * ENT_LEN;
			put32(b, e->vpc);
			put64(b + 4, e->hash);
			put32(b + 12, salsa64_rec_profile_merge(e->score, e->hits, w->long_session));
			put32(b + 16, (uint32_t)e->n << 16 | e->words);
		}
		if (!write_sum(w->f, &w->sum, buf, (size_t)n * ENT_LEN)) {
			salsa64_rec_profile_save_abort(w);
			return -1;
		}
		w->pos += n;
		max_entries -= n;
	}
	if (w->pos < w->count)
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
