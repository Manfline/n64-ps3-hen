/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — E/S de la HLE gráfica en el SPE: la RDRAM se lee por DMA y la
 * salida se escribe por DMA desde búferes dobles del local store.
 *
 *   display list  bloques de 1 KB alineados; el siguiente se pide antes de
 *                 necesitarlo (doble búfer)
 *   lecturas      vértices, matrices, luces y cargas de la TMEM: DMA
 *                 síncrono del tramo alineado a 16 que las contiene
 *   vértices      dos búferes de 8 KB (256 vértices): mientras uno sale por
 *   y comandos    DMA se llena el otro; ídem dos de 4 KB para los comandos
 *   texturas      dos búferes de 32 KB; cada textura sale por DMA a la
 *                 región de la caché de texturas o al arena de la tarea
 *
 * Etiquetas DMA: 0 lecturas, 1–2 display list, 3–4 vértices, 5–6 comandos,
 * 7–8 texturas. La 31 es de spukit.
 */
#include <spukit/spu.h>
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"

#define TAG_READ 0
#define TAG_DL   1
#define TAG_VTX  3
#define TAG_CMD  5
#define TAG_TEX  7
#define TAG_WR   9

#define DL_BLOCK  1024u
#define VTX_BUF   8192u
#define CMD_BUF   4096u

static uint8_t rd_vtx[GFX_MAX_VTX * 16 + 32] __attribute__((aligned(128)));
static uint8_t rd_mtx[64 + 32] __attribute__((aligned(128)));
static uint8_t rd_small[512 + 32] __attribute__((aligned(128)));
static uint8_t rd_tex[GFX_IO_MAX + 32] __attribute__((aligned(128)));
static uint8_t wr_buf[512 + 32] __attribute__((aligned(128)));

static uint8_t dl_buf[2][DL_BLOCK] __attribute__((aligned(128)));
static uint8_t vtx_buf[2][VTX_BUF] __attribute__((aligned(128)));
static uint8_t cmd_buf[2][CMD_BUF] __attribute__((aligned(128)));
static uint8_t tex_buf[2][GFX_TEX_MAX] __attribute__((aligned(128)));

static struct {
	uint32_t dl_addr[2];   /* bloque cargado en cada búfer (~0 = ninguno) */
	int dl_cur;
	int dl_pending;        /* el otro búfer tiene un pedido en vuelo */
	int vtx_cur, cmd_cur, tex_cur;
	uint32_t vtx_fill, cmd_fill;      /* bytes en el búfer en curso */
	uint32_t vtx_flushed, cmd_flushed; /* bytes ya enviados a la memoria */
	uint32_t tex_used;
} io;

uint32_t gfx_io_clock(void)
{
	return ~spu_read_decrementer();  /* el decrementador cuenta hacia abajo */
}

void gfx_io_begin(struct gfx_state *g)
{
	(void)g;
	memset(&io, 0, sizeof io);
	io.dl_addr[0] = io.dl_addr[1] = ~0u;
}

/* Dirección efectiva de 32 bits: la memoria principal de la PS3 (y la del RSX)
 * está bajo 4 GB, así que la parte alta (EAH) es un cero constante. Con la
 * suma de 64 bits el compilador deja un EAH que no es constante (RPCS3 avisa
 * «MFC_EAH is not a zero constant») y, según el diseño del código, el
 * recompilador de SPU de RPCS3 hacía que la imagen leyera ceros en vez de la
 * display list: sus tareas de prueba salían distintas a las del PPE y el juego
 * no dibujaba. Con esto no vuelve a pasar en los binarios probados
 * (docs/spe0-familias.md). */
static inline uint64_t ea32(uint64_t ea)
{
	return (uint32_t)ea;
}

static uint64_t rdram_ea(const struct gfx_state *g, uint32_t addr)
{
	return ea32(g->job->rdram_ea + addr);
}

static void flush_vtx(struct gfx_state *g)
{
	if (!io.vtx_fill)
		return;
	int b = io.vtx_cur;
	spukit_dma_put(vtx_buf[b], ea32(g->job->vtx_ea + io.vtx_flushed), (io.vtx_fill + 15) & ~15u, TAG_VTX + b);
	io.vtx_flushed += io.vtx_fill;
	io.vtx_fill = 0;
	io.vtx_cur ^= 1;
	spukit_dma_wait(1u << (TAG_VTX + io.vtx_cur));  /* el otro búfer, libre */
}

static void flush_cmd(struct gfx_state *g)
{
	if (!io.cmd_fill)
		return;
	int b = io.cmd_cur;
	spukit_dma_put(cmd_buf[b], ea32(g->job->cmd_ea + io.cmd_flushed), io.cmd_fill, TAG_CMD + b);
	io.cmd_flushed += io.cmd_fill;
	io.cmd_fill = 0;
	io.cmd_cur ^= 1;
	spukit_dma_wait(1u << (TAG_CMD + io.cmd_cur));
}

void gfx_io_end(struct gfx_state *g)
{
	static const struct gfx_cmd end __attribute__((aligned(16))) = { GFX_OP_END, sizeof end, 0, 0, 0 };
	gfx_io_cmd(g, &end, sizeof end);
	flush_vtx(g);
	flush_cmd(g);
	spukit_dma_wait(0x7FFFFFFFu);
	g->res->vtx_bytes = io.vtx_flushed;
	g->res->cmd_bytes = io.cmd_flushed;
	g->res->tex_bytes = io.tex_used;
}

const uint8_t *gfx_io_read(struct gfx_state *g, enum gfx_io_slot slot, uint32_t addr, uint32_t size)
{
	const struct gfx_job *j = g->job;
	if (addr >= j->rdram_size || size > j->rdram_size - addr)
		return NULL;
	uint8_t *buf;
	uint32_t cap;
	switch (slot) {
	case GFX_IO_VTX: buf = rd_vtx; cap = sizeof rd_vtx; break;
	case GFX_IO_MTX: buf = rd_mtx; cap = sizeof rd_mtx; break;
	case GFX_IO_TEX: buf = rd_tex; cap = sizeof rd_tex; break;
	default:         buf = rd_small; cap = sizeof rd_small; break;
	}
	if (size + 32 > cap)
		return NULL;
	return spukit_get_unaligned(buf, rdram_ea(g, addr), size, TAG_READ);
}

/* Escribe en la RDRAM por DMA: el tramo alineado a 16 que contiene los bytes (se
 * lee antes lo que no se pisa). Solo la HLE de Indiana Jones lo usa. */
void gfx_io_write(struct gfx_state *g, uint32_t addr, const uint8_t *src, uint32_t size)
{
	if (size == 0 || addr >= g->job->rdram_size || size > g->job->rdram_size - addr)
		return;
	const uint32_t start = addr & ~15u, end = (addr + size + 15) & ~15u, len = end - start;
	if (len > 512)
		return;
	if (start != addr || end != addr + size) {
		spukit_dma_get(wr_buf, rdram_ea(g, start), len, TAG_WR);
		spukit_dma_wait(1u << TAG_WR);
	}
	memcpy(wr_buf + (addr - start), src, size);
	spukit_dma_put(wr_buf, rdram_ea(g, start), len, TAG_WR);
	spukit_dma_wait(1u << TAG_WR);
}

/* Pide el bloque de display list que contiene `block` al búfer b. */
static void dl_fetch(struct gfx_state *g, int b, uint32_t block)
{
	uint32_t len = DL_BLOCK;
	if (block + len > g->job->rdram_size)
		len = g->job->rdram_size - block;
	spukit_dma_get(dl_buf[b], rdram_ea(g, block), (len + 15) & ~15u, TAG_DL + b);
	io.dl_addr[b] = block;
}

bool gfx_io_dl(struct gfx_state *g, uint32_t addr, uint32_t *w0, uint32_t *w1)
{
	addr &= ~7u;
	if (addr + 8 > g->job->rdram_size)
		return false;
	uint32_t block = addr & ~(DL_BLOCK - 1);
	int b = io.dl_cur;
	if (io.dl_addr[b] != block) {
		int o = b ^ 1;
		if (io.dl_addr[o] == block) {
			/* El bloque siguiente que se pidió antes: ya llegó o está llegando. */
			b = io.dl_cur = o;
		} else {
			spukit_dma_wait(1u << (TAG_DL + o));
			dl_fetch(g, b, block);
		}
		spukit_dma_wait(1u << (TAG_DL + b));
		/* Adelantar el bloque que sigue en el otro búfer. */
		uint32_t next = block + DL_BLOCK;
		int o2 = b ^ 1;
		if (next < g->job->rdram_size && io.dl_addr[o2] != next) {
			spukit_dma_wait(1u << (TAG_DL + o2));
			dl_fetch(g, o2, next);
		}
	}
	const uint8_t *p = dl_buf[b] + (addr - block);
	*w0 = salsa64_rd32(p);
	*w1 = salsa64_rd32(p + 4);
	return true;
}

struct gfx_vertex *gfx_io_vtx(struct gfx_state *g, uint32_t n)
{
	uint32_t bytes = n * (uint32_t)sizeof(struct gfx_vertex);
	if (bytes > g->job->vtx_cap - io.vtx_flushed - io.vtx_fill) {
		g->res->flags |= GFX_RES_VTX_FULL;
		return NULL;
	}
	if (io.vtx_fill + bytes > VTX_BUF)
		flush_vtx(g);
	struct gfx_vertex *v = (struct gfx_vertex *)(vtx_buf[io.vtx_cur] + io.vtx_fill);
	io.vtx_fill += bytes;
	return v;
}

bool gfx_io_cmd(struct gfx_state *g, const void *rec, uint32_t size)
{
	uint32_t reserve = ((const struct gfx_cmd *)rec)->op == GFX_OP_END ? 0 : sizeof(struct gfx_cmd);
	if (size + reserve > g->job->cmd_cap - io.cmd_flushed - io.cmd_fill || size > CMD_BUF) {
		g->res->flags |= ((const struct gfx_cmd *)rec)->op == GFX_OP_VGROUP ? GFX_RES_VGROUP_FULL : GFX_RES_CMD_FULL;
		return false;
	}
	if (io.cmd_fill + size > CMD_BUF)
		flush_cmd(g);
	memcpy(cmd_buf[io.cmd_cur] + io.cmd_fill, rec, size);
	io.cmd_fill += size;
	return true;
}

uint8_t *gfx_io_tex_begin(struct gfx_state *g, uint32_t size)
{
	(void)g;
	if (size > GFX_TEX_MAX)
		return NULL;
	/* El búfer se reutiliza cuando su DMA anterior terminó. */
	spukit_dma_wait(1u << (TAG_TEX + io.tex_cur));
	return tex_buf[io.tex_cur];
}

int32_t gfx_io_tex_commit(struct gfx_state *g, uint32_t size)
{
	uint32_t off = (io.tex_used + 127) & ~127u;
	if (off > g->job->tex_cap || size > g->job->tex_cap - off) {
		g->res->flags |= GFX_RES_TEX_FULL;
		return -1;
	}
	spukit_dma_put(tex_buf[io.tex_cur], ea32(g->job->tex_ea + off), (size + 15) & ~15u, TAG_TEX + io.tex_cur);
	io.tex_cur ^= 1;
	io.tex_used = off + size;
	return (int32_t)off;
}

void gfx_io_tex_cache(struct gfx_state *g, uint32_t off, uint32_t size)
{
	spukit_dma_put(tex_buf[io.tex_cur], ea32(g->job->cache_ea + off), (size + 15) & ~15u, TAG_TEX + io.tex_cur);
	io.tex_cur ^= 1;
}
