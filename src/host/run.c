/*
 * Salsa64 — ejecutor de línea de comandos para el host, para pruebas
 * automatizadas (tools/test-roms.sh). No depende de PSL1GHT.
 *
 *   salsa64-run [opciones] rom.z64|rom.v64|rom.n64
 *
 * Carga la ROM, la arranca por HLE (core/boot.c) y corre el intérprete en
 * bloques de ciclos hasta que se cumple una condición. Lo que la ROM imprime
 * por el ISViewer sale por stdout; el resumen, por stderr.
 *
 * Toda la memoria se reserva antes de empezar a emular: la RDRAM y el búfer
 * del ISViewer son estáticos y la ROM se lee una vez con malloc.
 *
 * Con --frame, al terminar se escribe en PPM la imagen que muestra el VI.
 *
 * Las tareas gráficas del RSP pasan por la HLE (src/rsp/gfx) en el mismo
 * hilo; al final se resume lo que produjo. Con --gfx-frame, la salida de la
 * HLE se dibuja con el rasterizador de referencia (raster.c) y se guarda la
 * imagen de la color image que muestra el VI; --gfxtest-check compara esa
 * imagen con los puntos de la ROM gfxtest (tests/roms/src/gfxtest/expect.h).
 *
 * Códigos de salida:
 *   0  apareció el texto de --until-text, o se agotaron los ciclos sin pedirlo
 *   1  apareció alguno de los textos de --fail-text
 *   2  se agotaron los ciclos esperando --until-text, o el CPU quedó en un
 *      bucle infinito sin interrupciones posibles
 *   3  error de argumentos o de carga
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../tests/roms/src/gfxtest/expect.h"
#include "core/boot.h"
#include "core/endian.h"
#include "raster.h"
#include "rsp/gfx/backend.h"
#include "rsp/gfx/hle.h"

#define DEFAULT_CYCLES (1ull << 32)  /* ~46 s de la consola */
#define DEFAULT_SLICE  (1ull << 20)
#define MAX_TEXT       255
#define MAX_FAIL_TEXTS 4

enum stop_reason {
	STOP_NONE,
	STOP_UNTIL,
	STOP_FAIL,
	STOP_CYCLES,
	STOP_STUCK,
};

static struct salsa64_n64 n64;
static uint8_t rdram[SALSA64_RDRAM_MAX] __attribute__((aligned(128)));
static uint8_t isv_buf[SALSA64_ISV_SIZE] __attribute__((aligned(16)));

/* HLE gráfica: estado, búferes de salida y totales. */
static struct gfx_state gfx_state;
static struct salsa64_gfx_sync gfx_sync;
static struct gfx_vertex gfx_vtx[1u << 16] __attribute__((aligned(128)));
static uint8_t gfx_cmd[512u << 10] __attribute__((aligned(128)));
static uint8_t gfx_tex[4u << 20] __attribute__((aligned(128)));
static struct {
	struct gfx_result sum;  /* contadores sumados; flags acumuladas */
	uint32_t tasks;
	bool raster;
} gfx;

static unsigned trace_gfx_left;

static bool trace_gfx_verbose;

static void gfx_acquire(void *ctx, struct gfx_job *job)
{
	(void)ctx;
	job->vtx_ea = (uint64_t)(uintptr_t)gfx_vtx;
	job->vtx_cap = sizeof gfx_vtx;
	job->cmd_ea = (uint64_t)(uintptr_t)gfx_cmd;
	job->cmd_cap = sizeof gfx_cmd;
	job->tex_ea = (uint64_t)(uintptr_t)gfx_tex;
	job->tex_cap = sizeof gfx_tex;
}

static void gfx_done(void *ctx, const struct gfx_job *job, const struct gfx_result *res)
{
	(void)ctx;
	struct gfx_result *s = &gfx.sum;
	gfx.tasks++;
	s->flags |= res->flags;
	s->commands += res->commands;
	s->triangles += res->triangles;
	s->rects += res->rects;
	s->draws += res->draws;
	s->textures += res->textures;
	s->tex_reused += res->tex_reused;
	s->unknown_cmds += res->unknown_cmds;
	if (res->unknown_cmds)
		s->last_unknown = res->last_unknown;
	s->approx_cc += res->approx_cc;
	s->dl_errors += res->dl_errors;
	if (gfx.raster)
		raster_task(job, res);
	if (trace_gfx_left && n64.bus.vi.fields > 500) {
		trace_gfx_left--;
		fprintf(stderr, "tarea gfx (campo %u):", n64.bus.vi.fields);
		uint32_t off = 0;
		while (off + sizeof(struct gfx_cmd) <= res->cmd_bytes) {
			const struct gfx_cmd *c = (const struct gfx_cmd *)(gfx_cmd + off);
			if (c->op == GFX_OP_END || !c->size)
				break;
			if (c->op == GFX_OP_TARGET)
				fprintf(stderr, " TARGET %06x/%u", c->a, c->b);
			else if (c->op == GFX_OP_ZCLEAR)
				fprintf(stderr, " ZCLEAR %06x", c->a);
			else if (c->op == GFX_OP_DRAW) {
				const struct gfx_draw *d = (const struct gfx_draw *)c;
				if (trace_gfx_verbose) {
					float zmin = 1e30f, zmax = -1e30f, wmin = 1e30f, wmax = -1e30f;
					for (uint32_t i = 0; i < d->h.b; i++) {
						const struct gfx_vertex *v = &gfx_vtx[d->h.a + i];
						float z = v->w != 0.0f ? v->z / v->w : 0.0f;
						zmin = z < zmin ? z : zmin, zmax = z > zmax ? z : zmax;
						wmin = v->w < wmin ? v->w : wmin, wmax = v->w > wmax ? v->w : wmax;
					}
					fprintf(stderr, "\n  D%-4u kind %u cyc %u ntex %u depth %u blend %u cull %u atest %u/%u fog %u "
						"z/w %.3f..%.3f w %.1f..%.1f", d->h.b / 3, d->prim_kind, d->cycle, d->ntex,
						d->depth, d->blend, d->cull, d->atest, d->aref, d->fog, zmin, zmax, wmin, wmax);
					if (d->blend) {
						static const char *const tn[] = { "T0", "T0a", "T1", "T1a", "1" };
						fprintf(stderr, "\n        alfa:");
						for (int t = 0; t < GFX_K_TERMS; t++)
							for (int k = 0; k < 3; k++)
								if (d->k[t][k][3] != 0.0f)
									fprintf(stderr, " %+.3f·%s·%s", d->k[t][k][3], tn[t],
										k == 0 ? "1" : k == 1 ? "C" : "Ca");
						for (int t = 0; t < d->ntex; t++)
							fprintf(stderr, " tex%d %ux%u pitch %u flags %x xf %.4f %.4f %.4f %.4f", t,
								d->tex[t].width, d->tex[t].height, d->tex[t].pitch, d->tex[t].flags,
								d->tex[t].xf[0], d->tex[t].xf[1], d->tex[t].xf[2], d->tex[t].xf[3]);
						const uint8_t *a = &gfx_vtx[d->h.a].r;
						fprintf(stderr, " v0 rgba %u %u %u %u", a[0], a[1], a[2], a[3]);
					}
				} else {
					fprintf(stderr, " D%u", d->h.b / 3);
				}
			}
			off += c->size;
		}
		fprintf(stderr, "\n");
	}
}

#ifdef SALSA64_DEBUG_SPMEM
void salsa64_debug_spmem(uint32_t reg, uint32_t v)
{
	static unsigned n;
	if (n++ < 200)
		fprintf(stderr, "lee SP mem %04x = %08x (pc %08llx, ciclo %llu)\n", reg, v,
			(unsigned long long)n64.cpu.cur_pc, (unsigned long long)n64.cpu.cycles);
}
#endif

static unsigned trace_rsp_left, trace_vi_left;

static void trace_rsp(void *ctx, const struct salsa64_task *t, enum salsa64_gbi gbi)
{
	(void)ctx;
	if (!trace_rsp_left)
		return;
	trace_rsp_left--;
	fprintf(stderr, "RSP en ciclo %llu: type=%u flags=%x ucode=%08x/%x data=%08x/%x data_ptr=%08x/%x "
		"boot=%08x/%x gbi=%d\n", (unsigned long long)n64.cpu.cycles, t->type, t->flags, t->ucode,
		t->ucode_size, t->ucode_data, t->ucode_data_size, t->data_ptr, t->data_size, t->ucode_boot,
		t->ucode_boot_size, gbi);
	/* Microcódigos pequeños: volcar texto y datos. */
	if (t->ucode_size && t->ucode_size <= 0x100) {
		fprintf(stderr, "  ucode:");
		for (uint32_t i = 0; i < t->ucode_size; i += 4)
			fprintf(stderr, " %08x", salsa64_rd32(n64.bus.rdram + ((t->ucode + i) & 0x7FFFFF)));
		fprintf(stderr, "\n  data:");
		for (uint32_t i = 0; i < t->ucode_data_size && i < 0x40; i += 4)
			fprintf(stderr, " %08x", salsa64_rd32(n64.bus.rdram + ((t->ucode_data + i) & 0x7FFFFF)));
		fprintf(stderr, "\n");
	}
}

static void gfx_summary(void)
{
	const struct gfx_result *s = &gfx.sum;
	double n = gfx.tasks;
	fprintf(stderr, "salsa64-run: gfx: %u tareas (%u otras, %u con microcódigo desconocido); por tarea "
		"%.0f comandos, %.0f triángulos, %.0f rectángulos, %.0f dibujos, %.1f texturas (+%.1f reusadas); "
		"%u comandos sin implementar (último 0x%02X), %u combiners aproximados, %u errores, banderas 0x%02X\n",
		gfx.tasks, n64.bus.rsp.stats.other + n64.bus.rsp.stats.audio, n64.bus.rsp.stats.unknown_gbi,
		s->commands / n, s->triangles / n, s->rects / n, s->draws / n, s->textures / n, s->tex_reused / n,
		s->unknown_cmds, s->last_unknown, s->approx_cc, s->dl_errors, s->flags);
}

/* Imagen del rasterizador para la color image que muestra el VI. */
static const uint32_t *gfx_image(uint32_t *width)
{
	struct salsa64_vi_frame f;
	if (!salsa64_vi_frame(&n64.bus, &f))
		return NULL;
	return raster_image(f.origin, width);
}

static bool gfx_dump(const char *path)
{
	uint32_t w;
	const uint32_t *img = gfx_image(&w);
	if (!img) {
		fprintf(stderr, "salsa64-run: ninguna tarea gráfica dibujó lo que muestra el VI\n");
		return false;
	}
	FILE *out = fopen(path, "wb");
	if (!out)
		return false;
	fprintf(out, "P6\n%u %u\n255\n", RASTER_W, RASTER_H);
	for (int i = 0; i < RASTER_W * RASTER_H; i++) {
		uint8_t rgb[3] = { (uint8_t)(img[i] >> 16), (uint8_t)(img[i] >> 8), (uint8_t)img[i] };
		fwrite(rgb, 1, 3, out);
	}
	fprintf(stderr, "salsa64-run: imagen de la HLE gráfica en %s\n", path);
	return fclose(out) == 0;
}

static uint8_t *read_file(const char *path, size_t *size);

/* Volcado de la PS3 (big-endian) → orden del host y rasterizador. */
static uint32_t be32(uint32_t v) { return salsa64_rd32((const uint8_t *)&v); }
static void swap32s(void *p, uint32_t bytes)
{
	uint32_t *w = p;
	for (uint32_t i = 0; i < bytes / 4; i++)
		w[i] = be32(w[i]);
}

static int raster_dump(const char *path, const char *out)
{
	size_t size;
	uint8_t *buf = read_file(path, &size);
	if (!buf || size < sizeof(struct gfx_dump_header)) {
		fprintf(stderr, "salsa64-run: no se pudo leer %s\n", path);
		return 3;
	}
	struct gfx_dump_header h;
	memcpy(&h, buf, sizeof h);
	swap32s(&h, sizeof h);
	if (h.magic != GFX_DUMP_MAGIC || sizeof h + (size_t)h.cmd_bytes + h.vtx_bytes + h.tex_bytes > size) {
		fprintf(stderr, "salsa64-run: %s no es un volcado de la HLE\n", path);
		return 3;
	}
	uint8_t *cmd = buf + sizeof h, *vtx = cmd + h.cmd_bytes, *tex = vtx + h.vtx_bytes;
	/* Comandos: cabeceras (op y size de 16 bits) y el resto palabras de 32. */
	for (uint32_t off = 0; off + sizeof(struct gfx_cmd) <= h.cmd_bytes;) {
		struct gfx_cmd *c = (struct gfx_cmd *)(cmd + off);
		uint16_t op = salsa64_rd16((uint8_t *)&c->op), sz = salsa64_rd16((uint8_t *)&c->size);
		c->op = op, c->size = sz;
		swap32s(&c->a, sz - 4);
		if (op == GFX_OP_ZCLEAR) {
			struct gfx_zclear *z = (struct gfx_zclear *)c;
			swap32s(z->rect, sizeof z->rect);  /* deshace: son de 16 bits */
			for (int i = 0; i < 4; i++)
				z->rect[i] = salsa64_rd16((uint8_t *)&z->rect[i]);
		} else if (op == GFX_OP_DRAW) {
			struct gfx_draw *d = (struct gfx_draw *)c;
			swap32s(d->scissor, 8 + 8);  /* deshace scissor y los bytes de estado */
			for (int i = 0; i < 4; i++)
				d->scissor[i] = salsa64_rd16((uint8_t *)&d->scissor[i]);
			for (int t = 0; t < 2; t++) {
				struct gfx_tex *x = &d->tex[t];
				swap32s(&x->width, 8);  /* deshace los de 16 y 8 bits */
				x->width = salsa64_rd16((uint8_t *)&x->width);
				x->height = salsa64_rd16((uint8_t *)&x->height);
				x->pitch = salsa64_rd16((uint8_t *)&x->pitch);
			}
		}
		if (op == GFX_OP_END || !sz)
			break;
		off += sz;
	}
	/* Vértices: 6 floats y un uint32 de relleno; el color son bytes. */
	for (uint32_t i = 0; i + sizeof(struct gfx_vertex) <= h.vtx_bytes; i += sizeof(struct gfx_vertex)) {
		struct gfx_vertex *v = (struct gfx_vertex *)(vtx + i);
		swap32s(v, 24);
	}
	struct gfx_job job;
	struct gfx_result res;
	memset(&job, 0, sizeof job);
	memset(&res, 0, sizeof res);
	job.cmd_ea = (uint64_t)(uintptr_t)cmd;
	job.vtx_ea = (uint64_t)(uintptr_t)vtx;
	job.tex_ea = (uint64_t)(uintptr_t)tex;
	res.cmd_bytes = h.cmd_bytes;
	res.vtx_bytes = h.vtx_bytes;
	res.tex_bytes = h.tex_bytes;
	raster_task(&job, &res);
	/* La primera color image del volcado. */
	const struct gfx_cmd *c = (const struct gfx_cmd *)cmd;
	uint32_t addr = 0, w;
	for (uint32_t off = 0; off < h.cmd_bytes && c->size; off += c->size, c = (const struct gfx_cmd *)(cmd + off))
		if (c->op == GFX_OP_TARGET) {
			addr = c->a;
			break;
		}
	const uint32_t *img = raster_image(addr, &w);
	if (!img || !out) {
		fprintf(stderr, "salsa64-run: el volcado no dibuja ninguna color image (o falta --gfx-frame)\n");
		return 3;
	}
	FILE *f = fopen(out, "wb");
	fprintf(f, "P6\n%u %u\n255\n", RASTER_W, RASTER_H);
	for (int i = 0; i < RASTER_W * RASTER_H; i++) {
		uint8_t rgb[3] = { (uint8_t)(img[i] >> 16), (uint8_t)(img[i] >> 8), (uint8_t)img[i] };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	fprintf(stderr, "salsa64-run: volcado de %s dibujado en %s (color image %06x)\n", path, out, addr);
	free(buf);
	return 0;
}

/* Puntos de gfxtest (expect.h) en la imagen del rasterizador. */
static bool gfxtest_check(void)
{
	static const struct gfxtest_point pts[] = { GFXTEST_POINTS };
	uint32_t w;
	const uint32_t *img = gfx_image(&w);
	if (!img) {
		fprintf(stderr, "salsa64-run: gfxtest: no hay imagen de la HLE\n");
		return false;
	}
	unsigned bad = 0;
	for (unsigned i = 0; i < sizeof pts / sizeof pts[0]; i++) {
		uint32_t got = img[pts[i].y * RASTER_W + pts[i].x], want = pts[i].rgb;
		int ok = 1;
		for (int k = 0; k < 3; k++) {
			int d = (int)((got >> (16 - 8 * k)) & 0xFF) - (int)((want >> (16 - 8 * k)) & 0xFF);
			if (d < -GFXTEST_TOL || d > GFXTEST_TOL)
				ok = 0;
		}
		if (!ok) {
			bad++;
			fprintf(stderr, "salsa64-run: gfxtest: (%u, %u) %s: 0x%06X, se esperaba 0x%06X\n", pts[i].x,
				pts[i].y, pts[i].what, got, want);
		}
	}
	fprintf(stderr, "salsa64-run: gfxtest: imagen %s (%u de %zu puntos bien)\n", bad ? "FALLO" : "OK",
		(unsigned)(sizeof pts / sizeof pts[0]) - bad, sizeof pts / sizeof pts[0]);
	return bad == 0;
}

/* Búsqueda de texto en la salida, también entre dos impresiones. */
static struct {
	const char *until, *fail[MAX_FAIL_TEXTS];
	unsigned nfail;
	size_t keep;
	size_t tail_len;
	char window[MAX_TEXT + SALSA64_ISV_SIZE];
	bool quiet;
	bool midline;  /* la última impresión no terminó en salto de línea */
	enum stop_reason hit;
} watch;

static void isv_out(void *ctx, const uint8_t *text, uint32_t len)
{
	(void)ctx;
	if (!watch.quiet && len) {
		fwrite(text, 1, len, stdout);
		watch.midline = text[len - 1] != '\n';
	}

	size_t n = watch.tail_len + len;
	memcpy(watch.window + watch.tail_len, text, len);
	if (watch.hit == STOP_NONE) {
		for (unsigned k = 0; k < watch.nfail; k++)
			if (memmem(watch.window, n, watch.fail[k], strlen(watch.fail[k])))
				watch.hit = STOP_FAIL;
		if (watch.hit == STOP_NONE && watch.until &&
		    memmem(watch.window, n, watch.until, strlen(watch.until)))
			watch.hit = STOP_UNTIL;
		if (watch.hit != STOP_NONE)
			salsa64_n64_stop(&n64);
	}
	watch.tail_len = n < watch.keep ? n : watch.keep;
	memmove(watch.window, watch.window + n - watch.tail_len, watch.tail_len);
}

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

/* Instrucción en una dirección de KSEG0/KSEG1, o 0 si no cae en la RDRAM. */
static uint32_t peek(uint64_t va)
{
	uint32_t pa = (uint32_t)va & 0x1FFFFFFF;
	if ((va >> 30) != 0x3FFFFFFFEull || pa + 4 > n64.bus.rdram_size)
		return 0;
	return salsa64_rd32(n64.bus.rdram + pa);
}

static uint64_t trace_exc_left;

static void trace_exc(struct salsa64_cpu *cpu)
{
	if (!trace_exc_left)
		return;
	trace_exc_left--;
	fprintf(stderr, "excepción %2u en ciclo %llu: pc=%016llx [%08x] epc=%016llx bd=%u badvaddr=%016llx status=%08x\n",
		SALSA64_CAUSE_EXC(cpu->cause), (unsigned long long)cpu->cycles,
		(unsigned long long)cpu->cur_pc, peek(cpu->cur_pc), (unsigned long long)cpu->epc,
		cpu->cause >> 31, (unsigned long long)cpu->bad_vaddr, cpu->status);
}

/* `b .` (o `j .`) con NOP en el delay slot y sin interrupción que lo saque.
 * El bloque puede terminar sobre el salto o sobre su delay slot. */
static bool cpu_stuck(void)
{
	const struct salsa64_cpu *cpu = &n64.cpu;
	uint64_t at = cpu->branch ? cpu->pc - 4 : cpu->pc;
	uint32_t i = peek(at);
	bool self = i == 0x1000FFFFu ||
		    (i >> 26 == 2 && (i & 0x03FFFFFF) == (((uint32_t)at >> 2) & 0x03FFFFFF));
	if (!self || peek(at + 4) != 0 || (cpu->branch && cpu->next_pc != at))
		return false;
	uint32_t st = cpu->status;
	bool irq_possible = (st & SALSA64_ST_IE) && !(st & (SALSA64_ST_EXL | SALSA64_ST_ERL)) &&
			    (st & SALSA64_ST_IM);
	return !irq_possible;
}

static uint8_t *read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	uint8_t *buf = NULL;
	long len;
	if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
	    (buf = malloc((size_t)len)) && fread(buf, 1, (size_t)len, f) == (size_t)len) {
		*size = (size_t)len;
	} else {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	return buf;
}

static void dump_regs(void)
{
	static const char *const names[32] = {
		"zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
		"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra",
	};
	const struct salsa64_cpu *cpu = &n64.cpu;
	for (int r = 0; r < 32; r++)
		fprintf(stderr, "%s=%016llx%s", names[r], (unsigned long long)cpu->gpr[r], r % 4 == 3 ? "\n" : " ");
	fprintf(stderr, "pc=%016llx status=%08x cause=%08x epc=%016llx badvaddr=%016llx\n",
		(unsigned long long)cpu->pc, cpu->status, cpu->cause,
		(unsigned long long)cpu->epc, (unsigned long long)cpu->bad_vaddr);
	const struct salsa64_bus *b = &n64.bus;
	fprintf(stderr, "MI intr=%02x mask=%02x; VI control=%08x origin=%08x v_intr=%u campos=%u; SP status=%04x; "
		"PI status=%x; tareas RSP: gfx %u, audio %u, otras %u, microcódigo desconocido %u\n",
		b->mi.intr, b->mi.mask, b->vi.regs[SALSA64_VI_CONTROL], b->vi.regs[SALSA64_VI_ORIGIN],
		b->vi.regs[SALSA64_VI_V_INTR], b->vi.fields, b->sp.regs[SALSA64_SP_STATUS], b->pi[SALSA64_PI_STATUS],
		b->rsp.stats.gfx, b->rsp.stats.audio, b->rsp.stats.other, b->rsp.stats.unknown_gbi);
	fprintf(stderr, "PIF RAM:");
	for (int i = 0; i < 64; i += 4)
		fprintf(stderr, " %08x", salsa64_rd32(b->pif_ram + i));
	fprintf(stderr, "\nSI status=%x dram=%08x\n", b->si[SALSA64_SI_STATUS], b->si[SALSA64_SI_DRAM_ADDR]);
#ifdef SALSA64_DEBUG_SPMEM
	extern unsigned salsa64_debug_irq[6];
	fprintf(stderr, "interrupciones: SP %u SI %u AI %u VI %u PI %u DP %u\n", salsa64_debug_irq[0],
		salsa64_debug_irq[1], salsa64_debug_irq[2], salsa64_debug_irq[3], salsa64_debug_irq[4],
		salsa64_debug_irq[5]);
#endif
}

/* Imagen del VI en PPM (P6), RGBA5551 o RGBA8888 → RGB de 8 bits. */
static bool dump_frame(const char *path)
{
	struct salsa64_vi_frame f;
	if (!salsa64_vi_frame(&n64.bus, &f)) {
		fprintf(stderr, "salsa64-run: el VI no muestra imagen (VI_CONTROL = 0x%08x)\n",
			n64.bus.vi.regs[SALSA64_VI_CONTROL]);
		return false;
	}
	FILE *out = fopen(path, "wb");
	if (!out)
		return false;
	fprintf(out, "P6\n%u %u\n255\n", f.width, f.height);
	for (uint32_t y = 0; y < f.height; y++) {
		const uint8_t *line = n64.bus.rdram + f.origin + y * f.stride;
		for (uint32_t x = 0; x < f.width; x++) {
			uint8_t rgb[3];
			if (f.bpp == 2) {
				uint32_t v = salsa64_rd16(line + 2 * x);
				uint32_t c[3] = { v >> 11, (v >> 6) & 31, (v >> 1) & 31 };
				for (int k = 0; k < 3; k++)
					rgb[k] = (uint8_t)(c[k] << 3 | c[k] >> 2);
			} else {
				memcpy(rgb, line + 4 * x, 3);
			}
			fwrite(rgb, 1, 3, out);
		}
	}
	fprintf(stderr, "salsa64-run: imagen del VI %ux%u (%s, origen 0x%06x) en %s; %u campos\n",
		f.width, f.height, f.bpp == 2 ? "RGBA5551" : "RGBA8888", f.origin, path,
		n64.bus.vi.fields);
	return fclose(out) == 0;
}

static void usage(FILE *out)
{
	fputs("uso: salsa64-run [opciones] rom\n"
	      "  -c, --cycles N        límite de ciclos del VR4300 (por omisión 2^32)\n"
	      "  -u, --until-text T    terminar (código 0) cuando el ISViewer imprima T\n"
	      "  -f, --fail-text T     terminar con código 1 si el ISViewer imprime T (hasta 4)\n"
	      "      --tv ntsc|pal|mpal  tipo de TV (por omisión, según el encabezado)\n"
	      "      --ram 4|8         MB de RDRAM (por omisión 8)\n"
	      "      --slice N         ciclos por bloque entre verificaciones (2^20)\n"
	      "  -q, --quiet           no copiar la salida del ISViewer a stdout\n"
	      "  -r, --regs            volcar los registros del CPU al terminar\n"
	      "      --trace-exc N     mostrar las primeras N excepciones del CPU\n"
	      "      --frame ARCHIVO   al terminar, guardar en PPM la imagen del VI\n"
	      "      --gfx-frame ARCHIVO  al terminar, guardar en PPM lo que dibujó la HLE gráfica\n"
	      "      --gfxtest-check   verificar la imagen de la HLE con los puntos de gfxtest\n"
	      "      --trace-rsp N     mostrar las primeras N tareas del RSP\n"
	      "      --trace-vi N      mostrar la imagen del VI en los últimos N campos\n"
	      "      --raster-dump F   dibujar un volcado de la HLE de la PS3 (gfxdump.bin) en\n"
	      "                        --gfx-frame y salir (sin ROM)\n", out);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "cycles", required_argument, NULL, 'c' },
		{ "until-text", required_argument, NULL, 'u' },
		{ "fail-text", required_argument, NULL, 'f' },
		{ "tv", required_argument, NULL, 'T' },
		{ "ram", required_argument, NULL, 'R' },
		{ "slice", required_argument, NULL, 'S' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "regs", no_argument, NULL, 'r' },
		{ "trace-exc", required_argument, NULL, 'X' },
		{ "frame", required_argument, NULL, 'F' },
		{ "gfx-frame", required_argument, NULL, 'G' },
		{ "gfxtest-check", no_argument, NULL, 'K' },
		{ "trace-rsp", required_argument, NULL, 'P' },
		{ "trace-vi", required_argument, NULL, 'V' },
		{ "trace-gfx", required_argument, NULL, 'Q' },
		{ "raster-dump", required_argument, NULL, 'D' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	uint64_t max_cycles = DEFAULT_CYCLES, slice = DEFAULT_SLICE;
	uint32_t ram = SALSA64_RDRAM_MAX;
	int tv = SALSA64_BOOT_TV_AUTO, c;
	bool regs = false;
	const char *frame_path = NULL, *gfx_path = NULL;
	bool gfx_check = false;
	const char *dump_path = NULL;

	while ((c = getopt_long(argc, argv, "c:u:f:qrh", opts, NULL)) != -1) {
		switch (c) {
		case 'c': max_cycles = strtoull(optarg, NULL, 0); break;
		case 'u': watch.until = optarg; break;
		case 'f':
			if (watch.nfail == MAX_FAIL_TEXTS) {
				usage(stderr);
				return 3;
			}
			watch.fail[watch.nfail++] = optarg;
			break;
		case 'S': slice = strtoull(optarg, NULL, 0); break;
		case 'q': watch.quiet = true; break;
		case 'r': regs = true; break;
		case 'X': trace_exc_left = strtoull(optarg, NULL, 0); break;
		case 'F': frame_path = optarg; break;
		case 'G': gfx_path = optarg; gfx.raster = true; break;
		case 'K': gfx_check = true; gfx.raster = true; break;
		case 'P': trace_rsp_left = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'V': trace_vi_left = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'D': dump_path = optarg; break;
		case 'Q': trace_gfx_left = (unsigned)strtoul(optarg, NULL, 0); trace_gfx_verbose = getenv("SALSA64_GFX_VERBOSE") != NULL; break;
		case 'T':
			if (!strcmp(optarg, "ntsc")) tv = SALSA64_TV_NTSC;
			else if (!strcmp(optarg, "pal")) tv = SALSA64_TV_PAL;
			else if (!strcmp(optarg, "mpal")) tv = SALSA64_TV_MPAL;
			else { usage(stderr); return 3; }
			break;
		case 'R':
			if (!strcmp(optarg, "4")) ram = 4u << 20;
			else if (!strcmp(optarg, "8")) ram = 8u << 20;
			else { usage(stderr); return 3; }
			break;
		case 'h': usage(stdout); return 0;
		default:  usage(stderr); return 3;
		}
	}
	if (dump_path)
		return raster_dump(dump_path, gfx_path);
	size_t longest = watch.until ? strlen(watch.until) : 0;
	for (unsigned k = 0; k < watch.nfail; k++)
		if (strlen(watch.fail[k]) > longest)
			longest = strlen(watch.fail[k]);
	if (optind != argc - 1 || slice == 0 || longest > MAX_TEXT) {
		usage(stderr);
		return 3;
	}
	watch.keep = longest ? longest - 1 : 0;

	const char *path = argv[optind];
	size_t rom_size = 0;
	uint8_t *rom = read_file(path, &rom_size);
	if (!rom) {
		fprintf(stderr, "salsa64-run: no se pudo leer %s\n", path);
		return 3;
	}
	static const char *const fmt_names[] = { ".z64", ".v64 (convertida)", ".n64 (convertida)" };
	enum salsa64_rom_format fmt = salsa64_rom_normalize(rom, rom_size);

	salsa64_n64_init(&n64, rdram, ram);
	const struct salsa64_gfx_sink sink = { NULL, gfx_acquire, gfx_done };
	salsa64_gfx_sync_init(&gfx_sync, &gfx_state, &n64.bus, &sink);
	salsa64_rsp_set_gfx(&n64.bus, &gfx_sync.backend);
	n64.bus.rsp.trace = trace_rsp;
	if (trace_exc_left)
		n64.cpu.exc_hook = trace_exc;
	salsa64_bus_set_isviewer(&n64.bus, isv_buf, isv_out, NULL);
	salsa64_bus_set_rom(&n64.bus, rom, (uint32_t)(rom_size > UINT32_MAX ? UINT32_MAX : rom_size));
	struct salsa64_boot_info info;
	if (!salsa64_boot_hle(&n64, tv, &info)) {
		fprintf(stderr, "salsa64-run: no se pudo arrancar %s: %s\n", path, info.error);
		return 3;
	}
	static const char *const tv_names[] = { "PAL", "NTSC", "MPAL" };
	if (info.kind == SALSA64_BOOT_ELF)
		fprintf(stderr, "salsa64-run: %s %s, %zu bytes; ELF en 0x%X (IPL3 libre), %u bytes cargados, "
			"entrada 0x%08X, TV %s, RDRAM %u MB\n", path, fmt_names[fmt], rom_size, info.elf_offset,
			info.loaded, info.entry, tv_names[info.tv], ram >> 20);
	else
		fprintf(stderr, "salsa64-run: %s %s, %zu bytes; carga plana (CIC %s), %u bytes, "
			"entrada 0x%08X, TV %s, RDRAM %u MB\n", path, fmt_names[fmt], rom_size,
			salsa64_cic_name(info.cic), info.loaded, info.entry, tv_names[info.tv], ram >> 20);

	/* Bucle principal: bloques de `slice` ciclos y verificación entre ellos. */
	enum stop_reason reason = STOP_NONE;
	uint64_t start_cycles = n64.cpu.cycles, t0 = now_us();
	while (reason == STOP_NONE) {
		uint64_t done = n64.cpu.cycles - start_cycles;
		if (done >= max_cycles) {
			reason = STOP_CYCLES;
			break;
		}
		if (trace_vi_left && max_cycles - done < (uint64_t)trace_vi_left * 1600000u) {
			/* Últimos campos: de a uno, con la imagen que describe el VI. */
			salsa64_n64_run_field(&n64);
			struct salsa64_vi_frame f;
			if (salsa64_vi_frame(&n64.bus, &f))
				fprintf(stderr, "campo %u: origen %06x %ux%u stride %u bpp %u (V_VIDEO %08x Y_SCALE %08x)\n",
					n64.bus.vi.fields, f.origin, f.width, f.height, f.stride, f.bpp,
					n64.bus.vi.regs[SALSA64_VI_V_VIDEO], n64.bus.vi.regs[SALSA64_VI_Y_SCALE]);
		} else {
			salsa64_n64_run(&n64, max_cycles - done < slice ? max_cycles - done : slice);
		}
		if (watch.hit != STOP_NONE)
			reason = watch.hit;
		else if (cpu_stuck())
			reason = STOP_STUCK;
	}
	uint64_t us = now_us() - t0, cycles = n64.cpu.cycles - start_cycles;
	if (watch.midline)
		putchar('\n');
	fflush(stdout);

	static const char *const reason_names[] = {
		[STOP_UNTIL] = "apareció el texto esperado",
		[STOP_FAIL] = "apareció el texto de falla",
		[STOP_CYCLES] = "se agotaron los ciclos",
		[STOP_STUCK] = "bucle infinito sin interrupciones posibles",
	};
	fprintf(stderr, "salsa64-run: %s; %llu ciclos (%.2f s de consola) en %.2f s, %.1f MIPS, pc=0x%016llx\n",
		reason_names[reason], (unsigned long long)cycles, cycles / 93.75e6, us / 1e6,
		us ? cycles / (double)us : 0.0, (unsigned long long)n64.cpu.pc);
	if (regs)
		dump_regs();
	if (gfx.tasks || n64.bus.rsp.stats.unknown_gbi)
		gfx_summary();
	if ((frame_path && !dump_frame(frame_path)) || (gfx_path && !gfx_dump(gfx_path))) {
		free(rom);
		return 3;
	}
	free(rom);
	if (gfx_check && !gfxtest_check())
		return 1;

	switch (reason) {
	case STOP_FAIL:   return 1;
	case STOP_STUCK:  return 2;
	case STOP_CYCLES: return watch.until ? 2 : 0;
	default:          return 0;
	}
}
