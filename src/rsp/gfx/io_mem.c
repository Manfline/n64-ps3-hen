/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — E/S de la HLE gráfica con memoria directa (host y PPE).
 *
 * La RDRAM se lee en su lugar; la salida va directo a los búferes del
 * trabajo (job->*_ea son punteros del mismo proceso). El SPE tiene su
 * propia versión con DMA (src/spu/gfx/io_spu.c).
 */
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"

#define PTR(ea) ((uint8_t *)(uintptr_t)(ea))

struct io_mem {
	uint32_t vtx_used, cmd_used, tex_used;
	uint8_t tex_stage[GFX_TEX_MAX] __attribute__((aligned(16)));
};

static struct io_mem io_default;

void gfx_io_detail_begin(struct gfx_state *g)
{
	const struct gfx_job *j = g->job;
	g->detail = NULL;
	if (j->detail_version != GFX_DETAIL_VERSION || j->detail_cap < sizeof(struct gfx_detail) ||
	    !j->detail_ea || (j->detail_ea & 127))
		return;
	g->detail = (struct gfx_detail *)(uintptr_t)j->detail_ea;
	memset(g->detail, 0, sizeof *g->detail);
	g->detail->version = GFX_DETAIL_VERSION;
	g->detail->bytes = sizeof *g->detail;
	g->detail->task = j->task;
}

void gfx_io_detail_end(struct gfx_state *g) { (void)g; }

uint32_t gfx_io_clock(void)
{
	return 0;
}

void gfx_io_begin(struct gfx_state *g)
{
	if (!g->io)
		g->io = &io_default;
	struct io_mem *io = g->io;
	io->vtx_used = io->cmd_used = io->tex_used = 0;
}

void gfx_io_end(struct gfx_state *g)
{
	struct io_mem *io = g->io;
	static const struct gfx_cmd end = { GFX_OP_END, sizeof end, 0, 0, 0 };
	gfx_io_cmd(g, &end, sizeof end);
	g->res->vtx_bytes = io->vtx_used;
	g->res->cmd_bytes = io->cmd_used;
	g->res->tex_bytes = io->tex_used;
}

const uint8_t *gfx_io_read(struct gfx_state *g, enum gfx_io_slot slot, uint32_t addr, uint32_t size)
{
	const struct gfx_job *j = g->job;
	if (addr >= j->rdram_size || size > j->rdram_size - addr)
		return NULL;
	GFX_DETAIL(g, {
		g->detail->reads[slot]++;
		g->detail->requested[slot] += size;
	});
	return PTR(j->rdram_ea) + addr;
}

void gfx_io_write(struct gfx_state *g, uint32_t addr, const uint8_t *src, uint32_t size)
{
	const struct gfx_job *j = g->job;
	if (addr >= j->rdram_size || size > j->rdram_size - addr)
		return;
	memcpy(PTR(j->rdram_ea) + addr, src, size);
}

bool gfx_io_dl(struct gfx_state *g, uint32_t addr, uint32_t *w0, uint32_t *w1)
{
	const uint8_t *p;
	if (g->job->flags & GFX_JOB_XBUS)
		p = PTR(g->job->dmem_ea) + (addr & 0xFF8u);
	else
		p = gfx_io_read(g, GFX_IO_SMALL, addr & ~7u, 8);
	if (!p)
		return false;
	*w0 = salsa64_rd32(p);
	*w1 = salsa64_rd32(p + 4);
	return true;
}

struct gfx_vertex *gfx_io_vtx(struct gfx_state *g, uint32_t n)
{
	struct io_mem *io = g->io;
	uint32_t bytes = n * (uint32_t)sizeof(struct gfx_vertex);
	if (bytes > g->job->vtx_cap - io->vtx_used) {
		g->res->flags |= GFX_RES_VTX_FULL;
		return NULL;
	}
	struct gfx_vertex *v = (struct gfx_vertex *)(PTR(g->job->vtx_ea) + io->vtx_used);
	io->vtx_used += bytes;
	return v;
}

bool gfx_io_cmd(struct gfx_state *g, const void *rec, uint32_t size)
{
	struct io_mem *io = g->io;
	/* Siempre queda lugar para el GFX_OP_END final. */
	uint32_t reserve = ((const struct gfx_cmd *)rec)->op == GFX_OP_END ? 0 : sizeof(struct gfx_cmd);
	if (size + reserve > g->job->cmd_cap - io->cmd_used) {
		g->res->flags |= ((const struct gfx_cmd *)rec)->op == GFX_OP_VGROUP ? GFX_RES_VGROUP_FULL : GFX_RES_CMD_FULL;
		return false;
	}
	memcpy(PTR(g->job->cmd_ea) + io->cmd_used, rec, size);
	io->cmd_used += size;
	return true;
}

uint8_t *gfx_io_tex_begin(struct gfx_state *g, uint32_t size)
{
	struct io_mem *io = g->io;
	return size <= GFX_TEX_MAX ? io->tex_stage : NULL;
}

int32_t gfx_io_tex_commit(struct gfx_state *g, uint32_t size)
{
	struct io_mem *io = g->io;
	uint32_t off = (io->tex_used + 127) & ~127u;
	if (off > g->job->tex_cap || size > g->job->tex_cap - off) {
		g->res->flags |= GFX_RES_TEX_FULL;
		return -1;
	}
	memcpy(PTR(g->job->tex_ea) + off, io->tex_stage, size);
	io->tex_used = off + size;
	return (int32_t)off;
}

void gfx_io_tex_cache(struct gfx_state *g, uint32_t off, uint32_t size)
{
	struct io_mem *io = g->io;
	memcpy(PTR(g->job->cache_ea) + off, io->tex_stage, size);
}
