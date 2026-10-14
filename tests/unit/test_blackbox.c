/*
 * Salsa64 — pruebas de la caja negra (rdp/blackbox.c): el anillo (orden,
 * relleno al dar la vuelta, descartes, deshacer), el archivo comprimido y su
 * lectura de vuelta.
 */
#include "test.h"

#include <string.h>

#include "core/endian.h"
#include "core/state.h"
#include "rdp/blackbox.h"

#define RING (64u << 10)
static uint8_t ring[RING + BB_SLACK] __attribute__((aligned(16)));
static struct bb bb;

static void ring_order_and_wrap(void)
{
	bb_init(&bb, ring, RING);
	uint32_t next = 1;
	/* Notas de largos distintos hasta dar varias vueltas; en el anillo
	 * quedan las últimas, seguidas y en orden. */
	for (unsigned i = 0; i < 400; i++) {
		uint32_t len = 100 + (i * 977) % 3000;
		uint8_t *p = bb_alloc(&bb, BB_NOTE, next, len);
		CHECK(p != NULL);
		if (!p)
			return;
		CHECK_EQ((uintptr_t)p & 15, 0);
		memset(p, (int)next, len);
		next++;
	}
	CHECK(bb.nrec > 5 && bb.nrec < 400);
	CHECK_EQ(bb.nrec, bb.nnote);
	struct bb_iter it;
	const struct bb_rec *r;
	const uint8_t *c;
	uint32_t seen = 0, last = 0;
	bb_iter_begin(&bb, &it);
	while ((r = bb_iter_next(&bb, &it, &c)) != NULL) {
		CHECK_EQ(r->kind, BB_NOTE);
		if (last)
			CHECK_EQ(r->id, last + 1);
		last = r->id;
		for (uint32_t i = 0; i < r->len; i += 97)
			CHECK_EQ(c[i], (uint8_t)r->id);
		seen++;
	}
	CHECK_EQ(seen, bb.nrec);
	CHECK_EQ(last, next - 1);  /* la más nueva sigue */
	/* No cabe ni con el anillo vacío. */
	CHECK(bb_alloc(&bb, BB_NOTE, 1, RING) == NULL);
	CHECK(bb_alloc(&bb, BB_NOTE, 1, RING - BB_REC_HEADER) != NULL);
	CHECK_EQ(bb.nrec, 1);
}

static void ring_cancel(void)
{
	bb_init(&bb, ring, RING);
	CHECK(bb_alloc(&bb, BB_NOTE, 1, 1000) != NULL);
	CHECK(bb_alloc(&bb, BB_TASK, 2, 2000) != NULL);
	CHECK_EQ(bb.ntask, 1);
	bb_cancel(&bb);
	CHECK_EQ(bb.ntask, 0);
	CHECK_EQ(bb.nrec, 1);
	struct bb_iter it;
	bb_iter_begin(&bb, &it);
	const struct bb_rec *r = bb_iter_next(&bb, &it, NULL);
	CHECK(r && r->id == 1);
	CHECK(bb_iter_next(&bb, &it, NULL) == NULL);
	/* Dos deshacer seguidos no hacen nada la segunda vez. */
	bb_cancel(&bb);
	CHECK_EQ(bb.nrec, 1);
}

/* Una tarea falsa: la de test_gfxdump con dos dibujos y una textura de la caché. */
static uint8_t cmd[2048] __attribute__((aligned(16)));
static uint8_t vtx[320] __attribute__((aligned(16)));
static uint8_t arena[256] __attribute__((aligned(16)));
static uint8_t cache[8192] __attribute__((aligned(16)));
static uint32_t wsrc[64], wlen[64];
static const struct gfx_tex *wtgt[64];
static const struct gfx_dump_work work = { wsrc, wlen, wtgt, 64 };

static struct gfx_dump_src fake_task(uint8_t seed, struct gfx_result *res)
{
	memset(cmd, 0, sizeof cmd);
	uint32_t off = 0;
	for (int d = 0; d < 2; d++) {
		struct gfx_draw dr;
		memset(&dr, 0, sizeof dr);
		dr.h.op = GFX_OP_DRAW;
		dr.h.size = sizeof dr;
		dr.ntex = 1;
		dr.tex[0].where = GFX_TEX_CACHE;
		dr.tex[0].offset = 0x400;
		dr.tex[0].width = 8;
		dr.tex[0].height = 4;
		dr.tex[0].pitch = 32;
		memcpy(cmd + off, &dr, sizeof dr);
		off += (uint32_t)sizeof dr;
	}
	struct gfx_cmd end = { GFX_OP_END, sizeof end, 0, 0, 0 };
	memcpy(cmd + off, &end, sizeof end);
	off += sizeof end;
	for (unsigned i = 0; i < sizeof vtx; i++)
		vtx[i] = (uint8_t)(seed + i / 8);
	memset(arena, seed, sizeof arena);
	for (unsigned i = 0; i < sizeof cache; i++)
		cache[i] = (uint8_t)((i % 16) + seed);
	memset(res, 0, sizeof *res);
	res->draws = 2;
	res->triangles = 5;
	res->flags = 3;
	struct gfx_dump_src s = { cmd, off, vtx, sizeof vtx, arena, sizeof arena, cache, sizeof cache, NULL, 0 };
	return s;
}

struct mem {
	uint8_t data[512 << 10];
	uint32_t len;
};
static struct mem file;

static bool mem_write(void *ctx, const void *d, uint32_t n)
{
	struct mem *m = ctx;
	if (n > sizeof m->data - m->len)
		return false;
	memcpy(m->data + m->len, d, n);
	m->len += n;
	return true;
}

static struct salsa64_state_lz zlz;

static void file_round_trip(void)
{
	bb_init(&bb, ring, RING);
	struct gfx_result res;
	struct gfx_dump_src s;
	uint32_t blob_len[3];
	for (uint32_t t = 0; t < 3; t++) {
		s = fake_task((uint8_t)(10 * t), &res);
		blob_len[t] = gfx_dump_measure(&s, &work);
		CHECK(bb_add_task(&bb, 40 + t, 100 + t, &s, &res, &work));
		uint32_t f[BB_FLD_WORDS];
		memset(f, 0, sizeof f);
		f[BB_FLD_FIELD] = 100 + t;
		f[BB_FLD_FLAGS] = BB_FLD_SHOWN | BB_FLD_RSX;
		f[BB_FLD_VI_WIDTH] = 320;
		f[BB_FLD_VI_REGS + 1] = 0x200000 + t;
		f[BB_FLD_KIND] = t == 1 ? BB_KIND_INTERP : BB_KIND_TRUE;
		f[BB_FLD_T] = 0x3F000000;  /* 0,5 */
		CHECK(bb_add_field(&bb, f));
	}
	CHECK(bb_add_task(&bb, 44, 103, &s, &res, &work));  /* con un hueco: la 43 no se registró */
	const char note[] = "vmatch: prueba";
	CHECK(bb_add_note(&bb, BB_FOURCC('V', 'M', 'A', 'T'), note, sizeof note));
	CHECK_EQ(bb.ntask, 4);
	CHECK_EQ(bb.nfield, 3);
	CHECK_EQ(bb.nnote, 1);

	struct bb_meta meta = { 777, 0x11223344, 0x55667788, "SALSA64 PRUEBA     " };
	memset(&file, 0, sizeof file);
	uint64_t raw = 0;
	CHECK(bb_write(&bb, &meta, salsa64_state_compress_host, &zlz, mem_write, &file, &raw));
	CHECK(file.len < raw);  /* las tareas se comprimen */

	struct bb_file_info info;
	CHECK(bb_file_info(file.data, file.len, &info));
	CHECK_EQ(info.field, 777);
	CHECK_EQ(info.tasks, 4);
	CHECK_EQ(info.fields, 3);
	CHECK_EQ(info.notes, 1);
	CHECK_EQ(info.crc1, 0x11223344);
	CHECK(strncmp(info.title, "SALSA64 PRUEBA", 14) == 0);

	/* Los registros salen en el orden del anillo, con su contenido. */
	static uint8_t out[64 << 10];
	uint32_t pos = 0, n = 0;
	struct bb_file_rec rec;
	const uint32_t want_kind[] = { BB_TASK, BB_FIELD, BB_TASK, BB_FIELD, BB_TASK, BB_FIELD, BB_TASK, BB_NOTE };
	while (bb_file_next(file.data, file.len, &pos, &rec)) {
		CHECK(n < 8);
		if (n >= 8)
			break;
		CHECK_EQ(rec.kind, want_kind[n]);
		CHECK(rec.len <= sizeof out);
		CHECK(bb_file_content(&rec, out));
		if (rec.kind == BB_TASK) {
			uint32_t t = rec.id;
			CHECK(t >= 40 && t <= 44);
			CHECK_EQ(salsa64_rd32(out + 4 * BB_TASK_NUM), t);
			CHECK_EQ(salsa64_rd32(out + 4 * BB_TASK_DRAWS), 2);
			CHECK_EQ(salsa64_rd32(out + 4 * BB_TASK_RES_FLAGS), 3);
			if (t == 40) {
				CHECK_EQ(salsa64_rd32(out + 4 * BB_TASK_PREV), BB_TASK_NONE);
			} else if (t == 44) {
				CHECK_EQ(salsa64_rd32(out + 4 * BB_TASK_PREV), 42);
				CHECK_EQ(salsa64_rd32(out + 4 * BB_TASK_FLAGS) & BB_TASK_ADJ, 0);
			} else {
				CHECK_EQ(salsa64_rd32(out + 4 * BB_TASK_PREV), t - 1);
				CHECK(salsa64_rd32(out + 4 * BB_TASK_FLAGS) & BB_TASK_ADJ);
			}
			/* Lo que sigue es el volcado de esa tarea. */
			struct gfx_dump_header h;
			memcpy(&h, out + 4 * BB_TASK_WORDS, sizeof h);
			CHECK_EQ(h.magic, GFX_DUMP_MAGIC);
			CHECK_EQ(h.cmd_bytes, s.cmd_bytes);
			CHECK_EQ(rec.len, 4 * BB_TASK_WORDS + blob_len[t == 44 ? 2 : t - 40]);
		} else if (rec.kind == BB_FIELD) {
			CHECK_EQ(rec.len, 4 * BB_FLD_WORDS);
			CHECK_EQ(salsa64_rd32(out + 4 * BB_FLD_FIELD), rec.id);
			CHECK_EQ(salsa64_rd32(out + 4 * BB_FLD_VI_WIDTH), 320);
			CHECK_EQ(salsa64_rd32(out + 4 * (BB_FLD_VI_REGS + 1)), 0x200000 + (rec.id - 100));
			CHECK_EQ(salsa64_rd32(out + 4 * BB_FLD_T), 0x3F000000);
		} else {
			CHECK_EQ(rec.id, BB_FOURCC('V', 'M', 'A', 'T'));
			CHECK(memcmp(out, note, sizeof note) == 0);
		}
		n++;
	}
	CHECK_EQ(n, 8);

	/* Cortado: el lector se detiene sin salirse. */
	for (uint32_t cut = 64; cut < file.len; cut += 331) {
		pos = 0;
		while (bb_file_next(file.data, cut, &pos, &rec)) {
			CHECK(pos <= cut);
		}
	}
	/* Sin comprimir también sale. */
	memset(&file, 0, sizeof file);
	CHECK(bb_write(&bb, &meta, NULL, NULL, mem_write, &file, NULL));
	pos = 0;
	n = 0;
	while (bb_file_next(file.data, file.len, &pos, &rec)) {
		CHECK(!(rec.flags & BB_REC_LZ));
		n++;
	}
	CHECK_EQ(n, 8);
}

void t_suite_blackbox(void)
{
	RUN(ring_order_and_wrap);
	RUN(ring_cancel);
	RUN(file_round_trip);
}
