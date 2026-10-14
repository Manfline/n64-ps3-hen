/*
 * Salsa64 — pruebas del armado de volcados de la HLE gráfica (rdp/gfxdump.c):
 * las texturas de la caché pasan al arena una vez cada una, las de un
 * destino se decodifican de la RDRAM y lo mal formado se rechaza.
 */
#include "test.h"

#include <string.h>

#include "rdp/gfxdump.h"

#define CMD_MAX 4096u
static uint8_t cmd[CMD_MAX] __attribute__((aligned(16)));
static uint8_t vtx[256] __attribute__((aligned(16)));
static uint8_t arena[512] __attribute__((aligned(16)));
static uint8_t cache[64 * 1024] __attribute__((aligned(16)));
static uint8_t rdram[64 * 1024];
static uint8_t out[32 * 1024] __attribute__((aligned(16)));

static uint32_t work_src[GFX_DUMP_MAX_TEX(CMD_MAX)], work_len[GFX_DUMP_MAX_TEX(CMD_MAX)];
static const struct gfx_tex *work_tgt[GFX_DUMP_MAX_TEX(CMD_MAX)];
static const struct gfx_dump_work work = { work_src, work_len, work_tgt, GFX_DUMP_MAX_TEX(CMD_MAX) };

static uint32_t add_draw(uint32_t off, const struct gfx_tex *t0, const struct gfx_tex *t1)
{
	struct gfx_draw d;
	memset(&d, 0, sizeof d);
	d.h.op = GFX_OP_DRAW;
	d.h.size = sizeof d;
	d.ntex = (t0 != NULL) + (t1 != NULL);
	if (t0)
		d.tex[0] = *t0;
	if (t1)
		d.tex[1] = *t1;
	memcpy(cmd + off, &d, sizeof d);
	return off + (uint32_t)sizeof d;
}

static struct gfx_tex tex(uint32_t where, uint32_t offset, uint16_t w, uint16_t h)
{
	struct gfx_tex t;
	memset(&t, 0, sizeof t);
	t.where = (uint8_t)where;
	t.offset = offset;
	t.width = w;
	t.height = h;
	t.pitch = (uint16_t)(w * 4);
	return t;
}

static struct gfx_dump_src src_for(uint32_t cmd_bytes)
{
	struct gfx_dump_src s = { cmd, cmd_bytes, vtx, sizeof vtx, arena, 300, cache, sizeof cache, rdram, sizeof rdram };
	return s;
}

static void gfxdump_layout(void)
{
	memset(cmd, 0, sizeof cmd);
	for (unsigned i = 0; i < sizeof cache; i++)
		cache[i] = (uint8_t)(i * 7 + 1);
	for (unsigned i = 0; i < sizeof arena; i++)
		arena[i] = (uint8_t)(0xA0 ^ i);
	memset(vtx, 0x55, sizeof vtx);
	/* Dibujo 1: dos texturas de la caché (la segunda, repetida en el 2);
	 * dibujo 2: la repetida y una del arena; dibujo 3: una de un destino. */
	struct gfx_tex a = tex(GFX_TEX_CACHE, 0x1000, 4, 4), b = tex(GFX_TEX_CACHE, 0x2000, 8, 2);
	struct gfx_tex ar = tex(GFX_TEX_ARENA, 0x40, 2, 2);
	struct gfx_tex tg = tex(GFX_TEX_TARGET, 0, 2, 2);
	tg.src_addr = 0x100;
	tg.src_pitch = 2;
	uint32_t off = 0;
	off = add_draw(off, &a, &b);
	off = add_draw(off, &b, &ar);
	off = add_draw(off, &tg, NULL);
	struct gfx_cmd end = { GFX_OP_END, sizeof end, 0, 0, 0 };
	memcpy(cmd + off, &end, sizeof end);
	off += sizeof end;
	/* RGBA16 de la RDRAM para la textura del destino: 4 texels. */
	rdram[0x100] = 0xF8, rdram[0x101] = 0x01;  /* rojo opaco */
	rdram[0x102] = 0x07, rdram[0x103] = 0xC1;  /* verde opaco */

	struct gfx_dump_src s = src_for(off);
	uint32_t need = gfx_dump_measure(&s, &work);
	/* arena 300 → 384; a 64 → 128; b 64 → 128; el destino 16 → 128 */
	uint32_t tex_bytes = 384 + 128 + 128 + 128;
	CHECK_EQ(need, sizeof(struct gfx_dump_header) + off + sizeof vtx + tex_bytes);
	CHECK_EQ(gfx_dump_build(&s, &work, out, sizeof out), need);
	CHECK_EQ(gfx_dump_build(&s, &work, out, need - 1), 0);  /* no cabe */

	struct gfx_dump_header h;
	memcpy(&h, out, sizeof h);
	CHECK_EQ(h.magic, GFX_DUMP_MAGIC);
	CHECK_EQ(h.cmd_bytes, off);
	CHECK_EQ(h.vtx_bytes, sizeof vtx);
	CHECK_EQ(h.tex_bytes, tex_bytes);
	const uint8_t *oc = out + sizeof h, *ov = oc + h.cmd_bytes, *ot = ov + h.vtx_bytes;
	CHECK(memcmp(ov, vtx, sizeof vtx) == 0);
	CHECK(memcmp(ot, arena, 300) == 0);
	CHECK_EQ(ot[300], 0);  /* relleno */
	/* Dibujos reescritos: todo en el arena; la caché una sola vez. */
	struct gfx_draw d1, d2, d3;
	memcpy(&d1, oc, sizeof d1);
	memcpy(&d2, oc + sizeof d1, sizeof d2);
	memcpy(&d3, oc + 2 * sizeof d1, sizeof d3);
	CHECK_EQ(d1.tex[0].where, GFX_TEX_ARENA);
	CHECK_EQ(d1.tex[0].offset, 384);
	CHECK_EQ(d1.tex[1].offset, 384 + 128);
	CHECK_EQ(d2.tex[0].offset, 384 + 128);   /* repetida: la misma copia */
	CHECK_EQ(d2.tex[1].where, GFX_TEX_ARENA);
	CHECK_EQ(d2.tex[1].offset, 0x40);        /* la del arena no se toca */
	CHECK_EQ(d3.tex[0].where, GFX_TEX_ARENA);
	CHECK_EQ(d3.tex[0].offset, 384 + 256);
	CHECK(memcmp(ot + 384, cache + 0x1000, 64) == 0);
	CHECK(memcmp(ot + 384 + 128, cache + 0x2000, 64) == 0);
	CHECK_EQ(ot[384 + 64], 0);
	/* La del destino: RGBA16 → RGBA8888 (rojo y verde). */
	CHECK_EQ(ot[384 + 256 + 0], 0xFF);
	CHECK_EQ(ot[384 + 256 + 1], 0x00);
	CHECK_EQ(ot[384 + 256 + 3], 0xFF);
	CHECK_EQ(ot[384 + 256 + 4 + 1], 0xFF);
}

static void gfxdump_rejects(void)
{
	memset(cmd, 0, sizeof cmd);
	struct gfx_tex bad = tex(GFX_TEX_CACHE, (uint32_t)sizeof cache - 8, 4, 4);  /* se sale de la caché */
	uint32_t off = add_draw(0, &bad, NULL);
	struct gfx_cmd end = { GFX_OP_END, sizeof end, 0, 0, 0 };
	memcpy(cmd + off, &end, sizeof end);
	struct gfx_dump_src s = src_for(off + (uint32_t)sizeof end);
	CHECK_EQ(gfx_dump_measure(&s, &work), 0);
	s.cache = NULL;  /* sin caché: una textura de la caché no se puede armar */
	struct gfx_tex ok = tex(GFX_TEX_CACHE, 0, 2, 2);
	add_draw(0, &ok, NULL);
	CHECK_EQ(gfx_dump_measure(&s, &work), 0);
	/* Un registro más largo que lo que queda. */
	s = src_for(off + (uint32_t)sizeof end);
	((struct gfx_cmd *)cmd)->size = 4000;
	CHECK_EQ(gfx_dump_measure(&s, &work), 0);
}

void t_suite_gfxdump(void)
{
	RUN(gfxdump_layout);
	RUN(gfxdump_rejects);
}
