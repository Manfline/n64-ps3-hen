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
#if GFX_HAS(F5INDY)
static uint8_t wr_buf[512 + 32] __attribute__((aligned(128)));
#endif

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

#if GFX_HAS(F5INDY)
/* Diagnóstico (solo la imagen de Indiana Jones: las otras no tienen holgura de local store para
 * esto): la primera lectura de display list de la tarea. */
static struct {
	uint32_t addr, n;
	uint32_t w[4];
} dbg;

void gfx_io_dbg_first(uint32_t *addr, uint32_t w[4])
{
	*addr = dbg.n ? dbg.addr : ~0u;
	for (unsigned i = 0; i < 4; i++)
		w[i] = dbg.w[i];
}
#endif

uint32_t gfx_io_clock(void)
{
	return ~spu_read_decrementer();  /* el decrementador cuenta hacia abajo */
}

#if GFX_SPE0_IMG == 1
void gfx_io_detail_begin(struct gfx_state *g)
{
	if (g->detail) {
		memset(g->detail, 0, sizeof *g->detail);
		g->detail->version = GFX_DETAIL_VERSION;
		g->detail->task = g->job->task;
		g->detail->bytes = sizeof *g->detail;
	}
}

void gfx_io_detail_end(struct gfx_state *g)
{
	if (g->detail) {
		/* Etiqueta propia; el resultado de spukit se publica después. */
		spukit_dma_put(g->detail, (uint32_t)g->job->detail_ea, sizeof *g->detail, TAG_WR);
		spukit_dma_wait(1u << TAG_WR);
	}
}
#endif

#define DETAIL_WAIT(g, mask, index) do { \
	uint32_t tw = 0; \
	GFX_DETAIL(g, tw = gfx_io_clock()); \
	spukit_dma_wait(mask); \
	GFX_DETAIL(g, (g)->detail->wait_ticks[index] += gfx_io_clock() - tw); \
} while (0)
#define DETAIL_PUT(g, index, size) GFX_DETAIL(g, { \
	(g)->detail->dma_puts[index] += ((size) + 16383u) / 16384u; \
	(g)->detail->dma_bytes[index] += (size); \
})

void gfx_io_begin(struct gfx_state *g)
{
	(void)g;
	memset(&io, 0, sizeof io);
#if GFX_HAS(F5INDY)
	memset(&dbg, 0, sizeof dbg);
#endif
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
	DETAIL_PUT(g, 0, (io.vtx_fill + 15) & ~15u);
	io.vtx_flushed += io.vtx_fill;
	io.vtx_fill = 0;
	io.vtx_cur ^= 1;
	DETAIL_WAIT(g, 1u << (TAG_VTX + io.vtx_cur), 0);  /* el otro búfer, libre */
}

static void flush_cmd(struct gfx_state *g)
{
	if (!io.cmd_fill)
		return;
	int b = io.cmd_cur;
	spukit_dma_put(cmd_buf[b], ea32(g->job->cmd_ea + io.cmd_flushed), io.cmd_fill, TAG_CMD + b);
	DETAIL_PUT(g, 1, io.cmd_fill);
	io.cmd_flushed += io.cmd_fill;
	io.cmd_fill = 0;
	io.cmd_cur ^= 1;
	DETAIL_WAIT(g, 1u << (TAG_CMD + io.cmd_cur), 1);
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
	GFX_DETAIL(g, {
		uint64_t ea = rdram_ea(g, addr), start = ea & ~15ull;
		uint32_t len = (uint32_t)(((ea + size + 15) & ~15ull) - start);
		g->detail->reads[slot]++;
		g->detail->requested[slot] += size;
		g->detail->transferred[slot] += len;
		uint32_t t = gfx_io_clock();
		spukit_dma_get(buf, start, len, TAG_READ);
		g->detail->send_ticks[slot] += gfx_io_clock() - t;
		t = gfx_io_clock();
		spukit_dma_wait(1u << TAG_READ);
		g->detail->read_ticks[slot] += gfx_io_clock() - t;
		return buf + (ea - start);
	});
	return spukit_get_unaligned(buf, rdram_ea(g, addr), size, TAG_READ);
}

#if GFX_HAS(F5INDY)
/* Escribe en la RDRAM por DMA exactamente los bytes pedidos, sin leer antes ni
 * reescribir los vecinos: el CPU emulado corre en el PPE mientras el SPE dibuja y
 * puede cambiar bytes del mismo bloque de 16 (una lectura-modificación-escritura
 * por bloques le pisaba lo escrito entre la lectura y la escritura, y el juego
 * perdía datos de libultra o de sus propias estructuras). El MFC admite DMA de 1,
 * 2, 4 y 8 bytes naturalmente alineados (con los 4 bits bajos del local store
 * iguales a los de la dirección) y de múltiplos de 16 alineados a 16: el tramo se
 * parte en esas piezas. Solo la HLE de Indiana Jones lo usa. */
void gfx_io_write(struct gfx_state *g, uint32_t addr, const uint8_t *src, uint32_t size)
{
	if (size == 0 || addr >= g->job->rdram_size || size > g->job->rdram_size - addr || size > 512)
		return;
	const uint32_t base = addr & ~15u;
	memcpy(wr_buf + (addr - base), src, size);
	uint32_t a = addr, n = size;
	while (n) {
		uint32_t chunk;
		if ((a & 15) == 0 && n >= 16) {
			chunk = n & ~15u;
		} else {
			chunk = 8;
			while (chunk > n || (a & (chunk - 1)))
				chunk >>= 1;
		}
		mfc_put((volatile void *)(wr_buf + (a - base)), rdram_ea(g, a), chunk, TAG_WR, 0, 0);
		a += chunk;
		n -= chunk;
	}
	spukit_dma_wait(1u << TAG_WR);
}
#endif

/* Pide el bloque de display list que contiene `block` al búfer b. */
static void dl_fetch(struct gfx_state *g, int b, uint32_t block)
{
	uint32_t len = DL_BLOCK;
	if (block + len > g->job->rdram_size)
		len = g->job->rdram_size - block;
	spukit_dma_get(dl_buf[b], rdram_ea(g, block), (len + 15) & ~15u, TAG_DL + b);
	GFX_DETAIL(g, { g->detail->dl_fetches++; g->detail->dl_bytes += (len + 15) & ~15u; });
	io.dl_addr[b] = block;
}

static inline __attribute__((always_inline)) bool dl_read(struct gfx_state *g, uint32_t addr, uint32_t *w0, uint32_t *w1)
{
	addr &= ~7u;
	if (addr + 8 > g->job->rdram_size)
		return false;
	uint32_t block = addr & ~(DL_BLOCK - 1);
	int b = io.dl_cur;
	if (io.dl_addr[b] != block) {
		GFX_DETAIL(g, g->detail->dl_misses++);
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
	} else {
		GFX_DETAIL(g, g->detail->dl_hits++);
	}
	const uint8_t *p = dl_buf[b] + (addr - block);
	*w0 = salsa64_rd32(p);
	*w1 = salsa64_rd32(p + 4);
#if GFX_HAS(F5INDY)
	if (!dbg.n) {
		dbg.n = 1;
		dbg.addr = addr;
		for (unsigned i = 0; i < 4; i++)  /* addr es múltiplo de 8: 16 bytes caben en el bloque salvo en su cola */
			dbg.w[i] = salsa64_rd32(p + 4 * i);
	}
#endif
	return true;
}

bool gfx_io_dl(struct gfx_state *g, uint32_t addr, uint32_t *w0, uint32_t *w1)
{
	uint32_t t = 0;
	GFX_DETAIL(g, t = gfx_io_clock());
	bool ok = dl_read(g, addr, w0, w1);
	GFX_DETAIL(g, g->detail->t_dl += gfx_io_clock() - t);
	return ok;
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
	DETAIL_WAIT(g, 1u << (TAG_TEX + io.tex_cur), 2);
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
	DETAIL_PUT(g, 2, (size + 15) & ~15u);
	io.tex_cur ^= 1;
	io.tex_used = off + size;
	return (int32_t)off;
}

void gfx_io_tex_cache(struct gfx_state *g, uint32_t off, uint32_t size)
{
	spukit_dma_put(tex_buf[io.tex_cur], ea32(g->job->cache_ea + off), (size + 15) & ~15u, TAG_TEX + io.tex_cur);
	DETAIL_PUT(g, 2, (size + 15) & ~15u);
	io.tex_cur ^= 1;
}
