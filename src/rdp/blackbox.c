/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — caja negra: anillo en memoria y archivo «S4BB». Ver blackbox.h.
 */
#include "rdp/blackbox.h"

#include <string.h>

#include <spukit/lz.h>

#include "core/endian.h"

#define ALIGN16(x) (((x) + 15u) & ~15u)

static struct bb_rec *rec_at(const struct bb *b, uint32_t pos)
{
	return (struct bb_rec *)(b->mem + pos);
}

void bb_init(struct bb *b, uint8_t *mem, uint32_t cap)
{
	memset(b, 0, sizeof *b);
	b->mem = mem;
	b->cap = cap & ~15u;
	b->last_task = BB_TASK_NONE;
}

void bb_clear(struct bb *b)
{
	b->head = b->tail = b->used = 0;
	b->nrec = b->ntask = b->nfield = b->nnote = 0;
	b->dropped_tasks = 0;
	b->last_task = BB_TASK_NONE;
	b->undo_ok = false;
}

static void count(struct bb *b, uint32_t kind, int d)
{
	if (kind == BB_PAD)
		return;
	b->nrec += (uint32_t)d;
	if (kind == BB_TASK)
		b->ntask += (uint32_t)d;
	else if (kind == BB_FIELD)
		b->nfield += (uint32_t)d;
	else
		b->nnote += (uint32_t)d;
}

static void evict_oldest(struct bb *b)
{
	const struct bb_rec *r = rec_at(b, b->tail);
	uint32_t size = r->size;
	if (r->kind == BB_TASK)
		b->dropped_tasks++;
	count(b, r->kind, -1);
	b->tail += size;
	if (b->tail >= b->cap)
		b->tail = 0;
	b->used -= size;
	if (b->used == 0)
		b->head = b->tail = 0;
}

static void put_header(struct bb *b, uint32_t pos, uint32_t size, uint32_t kind, uint32_t id, uint32_t len)
{
	struct bb_rec *r = rec_at(b, pos);
	r->size = size;
	r->kind = kind;
	r->id = id;
	r->len = len;
}

uint8_t *bb_alloc(struct bb *b, uint32_t kind, uint32_t id, uint32_t len)
{
	uint32_t sz = ALIGN16(BB_REC_HEADER + len);
	if (!b->mem || sz < len || sz > b->cap)
		return NULL;
	b->undo_ok = false;
	for (;;) {
		if (b->used == 0)
			b->head = b->tail = 0;
		uint32_t free_end, free_wrap = 0;
		if (b->used == 0)
			free_end = b->cap;
		else if (b->head > b->tail)
			free_end = b->cap - b->head, free_wrap = b->tail;
		else if (b->head < b->tail)
			free_end = b->tail - b->head;
		else
			free_end = 0;  /* head == tail y used > 0: lleno */
		if (sz > free_end && sz <= free_wrap) {
			/* No cabe al final pero sí al principio: el final queda de relleno. */
			uint32_t padsz = b->cap - b->head;
			if (padsz) {
				put_header(b, b->head, padsz, BB_PAD, 0, 0);
				b->used += padsz;
			}
			b->head = 0;
			free_end = free_wrap;
		}
		if (sz <= free_end) {
			b->undo_head = b->head;
			b->undo_used = b->used;
			put_header(b, b->head, sz, kind, id, len);
			b->head += sz;
			if (b->head >= b->cap)
				b->head = 0;
			b->used += sz;
			count(b, kind, 1);
			b->undo_ok = true;
			return b->mem + (b->undo_head + BB_REC_HEADER);
		}
		evict_oldest(b);
	}
}

void bb_cancel(struct bb *b)
{
	if (!b->undo_ok)
		return;
	const struct bb_rec *r = rec_at(b, b->undo_head);
	count(b, r->kind, -1);
	b->head = b->undo_head;
	b->used = b->undo_used;
	b->undo_ok = false;
	if (b->used == 0)
		b->head = b->tail = 0;
}

static void put32(uint8_t *p, uint32_t v)
{
	salsa64_wr32(p, v);
}

bool bb_add_task(struct bb *b, uint32_t task, uint32_t field, const struct gfx_dump_src *src,
		 const struct gfx_result *res, const struct gfx_dump_work *work)
{
	uint32_t blob = gfx_dump_measure(src, work);
	if (!blob)
		return false;
	uint8_t *p = bb_alloc(b, BB_TASK, task, BB_TASK_WORDS * 4 + blob);
	if (!p)
		return false;
	if (gfx_dump_build(src, work, p + BB_TASK_WORDS * 4, blob) != blob) {
		bb_cancel(b);
		return false;
	}
	uint32_t prev = b->last_task;
	put32(p + 4 * BB_TASK_NUM, task);
	put32(p + 4 * BB_TASK_FIELD, field);
	put32(p + 4 * BB_TASK_PREV, prev);
	put32(p + 4 * BB_TASK_FLAGS, prev != BB_TASK_NONE && prev + 1 == task ? BB_TASK_ADJ : 0);
	put32(p + 4 * BB_TASK_DRAWS, res->draws);
	put32(p + 4 * BB_TASK_TRIANGLES, res->triangles);
	put32(p + 4 * BB_TASK_RECTS, res->rects);
	put32(p + 4 * BB_TASK_RES_FLAGS, res->flags);
	b->last_task = task;
	return true;
}

bool bb_add_field(struct bb *b, const uint32_t words[BB_FLD_WORDS])
{
	uint8_t *p = bb_alloc(b, BB_FIELD, words[BB_FLD_FIELD], BB_FLD_WORDS * 4);
	if (!p)
		return false;
	for (unsigned i = 0; i < BB_FLD_WORDS; i++)
		put32(p + 4 * i, words[i]);
	return true;
}

bool bb_add_note(struct bb *b, uint32_t tag, const void *data, uint32_t len)
{
	uint8_t *p = bb_alloc(b, BB_NOTE, tag, len);
	if (!p)
		return false;
	memcpy(p, data, len);
	return true;
}

void bb_iter_begin(const struct bb *b, struct bb_iter *it)
{
	it->pos = b->tail;
	it->left = b->used;
}

const struct bb_rec *bb_iter_next(const struct bb *b, struct bb_iter *it, const uint8_t **content)
{
	while (it->left) {
		const struct bb_rec *r = rec_at(b, it->pos);
		uint32_t at = it->pos;
		it->pos += r->size;
		if (it->pos >= b->cap)
			it->pos = 0;
		it->left -= r->size;
		if (r->kind == BB_PAD)
			continue;
		if (content)
			*content = b->mem + at + BB_REC_HEADER;
		return r;
	}
	return NULL;
}

/* ---- Escribir ------------------------------------------------------------------- */

bool bb_write(const struct bb *b, const struct bb_meta *m, bb_compress_fn compress, void *cctx, bb_write_fn write,
	      void *wctx, uint64_t *raw)
{
	uint8_t h[BB_HEADER];
	uint64_t total = BB_HEADER + BB_REC_HEADER;
	memset(h, 0, sizeof h);
	put32(h + 0, BB_MAGIC);
	put32(h + 4, BB_VERSION);
	put32(h + 8, BB_HEADER);
	put32(h + 12, compress ? BB_FLAG_LZ : 0);
	put32(h + 16, m->field);
	put32(h + 20, b->ntask);
	put32(h + 24, b->nfield);
	put32(h + 28, b->nnote);
	put32(h + 32, m->crc1);
	put32(h + 36, m->crc2);
	memcpy(h + 40, m->title, 20);
	put32(h + 60, b->dropped_tasks);
	if (!write(wctx, h, sizeof h))
		return false;

	struct bb_iter it;
	const struct bb_rec *r;
	const uint8_t *c;
	bb_iter_begin(b, &it);
	while ((r = bb_iter_next(b, &it, &c)) != NULL) {
		bool lz = compress && (r->kind == BB_TASK || r->len >= 4096);
		uint8_t rh[BB_REC_HEADER];
		put32(rh + 0, r->kind);
		put32(rh + 4, r->len);
		put32(rh + 8, lz ? BB_REC_LZ : 0);
		put32(rh + 12, r->id);
		if (!write(wctx, rh, sizeof rh))
			return false;
		if (r->len && !(lz ? compress(cctx, c, r->len, write, wctx) : write(wctx, c, r->len)))
			return false;
		total += BB_REC_HEADER + r->len;
	}
	uint8_t end[BB_REC_HEADER];
	memset(end, 0, sizeof end);
	put32(end, BB_END);
	if (!write(wctx, end, sizeof end))
		return false;
	if (raw)
		*raw = total;
	return true;
}

/* ---- Leer ----------------------------------------------------------------------- */

bool bb_file_info(const uint8_t *f, uint32_t len, struct bb_file_info *info)
{
	if (len < BB_HEADER || salsa64_rd32(f) != BB_MAGIC || salsa64_rd32(f + 8) != BB_HEADER)
		return false;
	memset(info, 0, sizeof *info);
	info->version = salsa64_rd32(f + 4);
	info->flags = salsa64_rd32(f + 12);
	info->field = salsa64_rd32(f + 16);
	info->tasks = salsa64_rd32(f + 20);
	info->fields = salsa64_rd32(f + 24);
	info->notes = salsa64_rd32(f + 28);
	info->crc1 = salsa64_rd32(f + 32);
	info->crc2 = salsa64_rd32(f + 36);
	memcpy(info->title, f + 40, 20);
	info->dropped = salsa64_rd32(f + 60);
	return info->version >= 1 && info->version <= BB_VERSION;
}

bool bb_file_next(const uint8_t *f, uint32_t len, uint32_t *pos, struct bb_file_rec *rec)
{
	uint32_t at = *pos ? *pos : BB_HEADER;
	if (len < BB_REC_HEADER || at > len - BB_REC_HEADER)
		return false;
	uint32_t kind = salsa64_rd32(f + at);
	if (kind == BB_END)
		return false;
	rec->kind = kind;
	rec->len = salsa64_rd32(f + at + 4);
	rec->flags = salsa64_rd32(f + at + 8);
	rec->id = salsa64_rd32(f + at + 12);
	at += BB_REC_HEADER;
	rec->payload = f + at;
	if (!(rec->flags & BB_REC_LZ)) {
		rec->payload_bytes = rec->len;
	} else {
		uint32_t nblocks = (rec->len + SPUKIT_LZ_BLOCK - 1) / SPUKIT_LZ_BLOCK, used = 0;
		for (uint32_t k = 0; k < nblocks; k++) {
			if (len - at < used || len - at - used < 4)
				return false;
			uint32_t n = spukit_lz_frame_len(f + at + used);
			if (n > SPUKIT_LZ_BLOCK || len - at - used - 4 < n)
				return false;
			used += 4 + n;
		}
		rec->payload_bytes = used;
	}
	if (rec->payload_bytes > len - at)
		return false;
	*pos = at + rec->payload_bytes;
	return true;
}

bool bb_file_content(const struct bb_file_rec *rec, uint8_t *out)
{
	if (!(rec->flags & BB_REC_LZ)) {
		memcpy(out, rec->payload, rec->len);
		return true;
	}
	uint32_t used = 0;
	for (uint32_t off = 0; off < rec->len; off += SPUKIT_LZ_BLOCK) {
		uint32_t n = rec->len - off < SPUKIT_LZ_BLOCK ? rec->len - off : SPUKIT_LZ_BLOCK;
		const uint8_t *hdr = rec->payload + used;
		uint32_t clen = spukit_lz_frame_len(hdr);
		if (spukit_lz_frame_stored(hdr)) {
			if (clen != n)
				return false;
			memcpy(out + off, hdr + 4, n);
		} else if (!spukit_lz_decompress(hdr + 4, clen, out + off, n)) {
			return false;
		}
		used += 4 + clen;
	}
	return true;
}
