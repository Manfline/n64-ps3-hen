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
 * Con --no-idle no se saltan los bucles de espera (cpu/idle.c): el
 * resultado debe ser idéntico, solo más lento.
 *
 * Guardado del cartucho (core/save.c): el tipo sale de la cabecera o de la
 * tabla por código de juego, o de --save. Con --save-dir se lee al empezar
 * y se escribe al terminar en ese directorio, con el mismo nombre que en la
 * PS3 (frontend/savefile.c); sin la opción queda solo en memoria.
 *
 * Con --rec corre el recompilador (solo en un host ppc64 big-endian: qemu,
 * tools/test-rec.sh). --lockstep además corre un segundo N64 con el
 * intérprete y, después de cada bloque recompilado, lo lleva al mismo ciclo
 * y compara el CPU (y cada tanto la RDRAM): la primera diferencia termina
 * con código 4 y el detalle.
 * Las tareas gráficas del RSP pasan por la HLE (src/rsp/gfx) en el mismo
 * hilo; al final se resume lo que produjo. Con --gfx-frame, la salida de la
 * HLE se dibuja con el rasterizador de referencia (raster.c) y se guarda la
 * imagen de la color image que muestra el VI; --gfxtest-check compara esa
 * imagen con los puntos de la ROM gfxtest (tests/roms/src/gfxtest/expect.h).
 *
 * Audio: las tareas de audio pasan por la HLE (src/rsp/audio) en el mismo
 * hilo y lo que sale por el AI se guarda; con --audio-wav se escribe en un
 * WAV y --audiotest-check lo compara muestra por muestra con el tono de la
 * ROM audiotest (tests/roms/src/audiotest/tone.h) y exige que no haya
 * huecos entre los DMA del AI.
 *
 * Controles programados (--pad): "CAMPO:BOTONES[@X,Y];…" (o separados por
 * ',') pone el control 1 al empezar ese campo del VI (A, B, Z, START,
 * DU/DD/DL/DR, L, R, CU/CD/CL/CR unidos con +; el stick como @X,Y o
 * x<n>/y<n>; "-" suelta todo) y el estado vale hasta la entrada siguiente.
 * Sirve para llegar a una escena sin manos (menús de un juego) y es
 * determinista: con --lockstep la referencia recibe lo mismo en el mismo
 * ciclo. --trace-from CAMPO retrasa las trazas (--trace-exc,
 * --trace-faults, --trace-rsp, --trace-gfx) hasta ese campo.
 *
 * Códigos de salida:
 *   0  apareció el texto de --until-text, o se agotaron los ciclos sin pedirlo
 *   1  apareció alguno de los textos de --fail-text
 *   2  se agotaron los ciclos esperando --until-text, o el CPU quedó en un
 *      bucle infinito sin interrupciones posibles
 *   3  error de argumentos o de carga
 *   4  --lockstep encontró una diferencia con el intérprete
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../tests/roms/src/audiotest/tone.h"
#include "../../tests/roms/src/gfxtest/expect.h"
#include "core/boot.h"
#include "core/endian.h"
#include "cpu/rec.h"
#include "host/rec_host.h"
#include "frontend/savefile.h"
#include "raster.h"
#include "rsp/gfx/backend.h"
#include "rsp/audio/backend.h"
#include "rsp/gfx/hle.h"
#include "rsp/lle/core.h"

#define DEFAULT_CYCLES (1ull << 32)  /* ~46 s de la consola */
#define DEFAULT_SLICE  (1ull << 20)
#define MAX_TEXT       255
#define MAX_FAIL_TEXTS 4
#define MAX_PADS       512

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
static struct salsa64_rec rec;

/* --lockstep: la máquina de referencia, solo con el intérprete. */
static struct salsa64_n64 ref;
static uint8_t ref_rdram[SALSA64_RDRAM_MAX] __attribute__((aligned(128)));
static uint8_t ref_isv_buf[SALSA64_ISV_SIZE] __attribute__((aligned(16)));
static struct {
	uint64_t exits;
	uint64_t ram_every;
	uint64_t history[16];   /* últimos bloques, para el informe */
	uint32_t fields;        /* campo del VI de la última comparación de la RDRAM */
} lock;

static struct salsa64_save save, ref_save;
static struct savefile save_file;

/* --pad: el control 1 por campo del VI. `applied` guarda en qué ciclo se
 * aplicó cada cambio para que la referencia de --lockstep reciba lo mismo
 * en el mismo ciclo. */
static struct {
	struct { uint32_t field; struct salsa64_pad pad; } at[MAX_PADS];
	unsigned n, next;
	struct { uint64_t cycle; struct salsa64_pad pad; } applied[MAX_PADS * 2];
	unsigned napplied, ref_next;
} pads;

static uint32_t trace_from;        /* --trace-from: campo desde el que se traza */
static bool trace_exc_faults_only; /* --trace-faults: sin interrupciones */

static bool tracing(void)
{
	return n64.bus.vi.fields >= trace_from;
}

static bool pad_button(const char *s, size_t n, uint16_t *bit)
{
	static const struct { const char *name; uint16_t bit; } names[] = {
		{ "A", SALSA64_PAD_A }, { "B", SALSA64_PAD_B }, { "Z", SALSA64_PAD_Z },
		{ "START", SALSA64_PAD_START }, { "DU", SALSA64_PAD_D_UP }, { "DD", SALSA64_PAD_D_DOWN },
		{ "DL", SALSA64_PAD_D_LEFT }, { "DR", SALSA64_PAD_D_RIGHT }, { "L", SALSA64_PAD_L },
		{ "R", SALSA64_PAD_R }, { "CU", SALSA64_PAD_C_UP }, { "CD", SALSA64_PAD_C_DOWN },
		{ "CL", SALSA64_PAD_C_LEFT }, { "CR", SALSA64_PAD_C_RIGHT },
	};
	for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
		if (strlen(names[i].name) == n && !strncmp(s, names[i].name, n)) {
			*bit = names[i].bit;
			return true;
		}
	return false;
}

/* "CAMPO:BOTONES[@X,Y];…" (o separados por ','): botones unidos con +,
 * '-' suelta todo, el stick como @X,Y o x<n>/y<n>. Cada estado vale hasta
 * el siguiente. */
static bool pad_parse(const char *spec)
{
	char sep = strchr(spec, ';') ? ';' : ',';
	while (*spec) {
		if (pads.n == MAX_PADS)
			return false;
		char *end;
		unsigned long field = strtoul(spec, &end, 0);
		if (end == spec || *end != ':' || (pads.n && field <= pads.at[pads.n - 1].field))
			return false;
		struct salsa64_pad pad = { 0, 0, 0, 1 };
		const char *p = end + 1;
		while (*p && *p != sep) {
			if (*p == '@') {
				long x = strtol(p + 1, &end, 10);
				if (*end != ',')
					return false;
				long y = strtol(end + 1, &end, 10);
				if (x < -128 || x > 127 || y < -128 || y > 127)
					return false;
				pad.x = (int8_t)x, pad.y = (int8_t)y;
				p = end;
				continue;
			}
			const char *q = p;
			while (*q && *q != sep && *q != '+' && *q != '@')
				q++;
			size_t n = (size_t)(q - p);
			uint16_t bit;
			if (n == 1 && *p == '-') {
				/* suelta todo */
			} else if (n > 1 && (*p == 'x' || *p == 'y')) {
				long v = strtol(p + 1, &end, 10);
				if (end != q || v < -128 || v > 127)
					return false;
				*(*p == 'x' ? &pad.x : &pad.y) = (int8_t)v;
			} else if (n && pad_button(p, n, &bit)) {
				pad.buttons |= bit;
			} else if (n) {
				return false;
			}
			p = *q == '+' ? q + 1 : q;
		}
		pads.at[pads.n].field = (uint32_t)field;
		pads.at[pads.n].pad = pad;
		pads.n++;
		spec = *p == sep ? p + 1 : p;
	}
	return pads.n > 0;
}

static void pad_apply(struct salsa64_n64 *m, const struct salsa64_pad *pad)
{
	salsa64_pif_set_pad(&m->bus, 0, pad);
}

/* Al empezar cada campo (la máquina parada justo en el fin del anterior). */
static void pad_field(void)
{
	uint32_t field = n64.bus.vi.fields;
	const struct salsa64_pad *pad = NULL;
	while (pads.next < pads.n && field >= pads.at[pads.next].field)
		pad = &pads.at[pads.next++].pad;
	if (!pad)
		return;
	pad_apply(&n64, pad);
	if (pads.napplied < sizeof pads.applied / sizeof pads.applied[0]) {
		pads.applied[pads.napplied].cycle = n64.cpu.cycles;
		pads.applied[pads.napplied].pad = *pad;
		pads.napplied++;
	}
	if (tracing())
		fprintf(stderr, "campo %u (ciclo %llu): control 1 botones 0x%04x x %d y %d\n", field,
			(unsigned long long)n64.cpu.cycles, pad->buttons, pad->x, pad->y);
}

/* Al terminar cada campo (modo campo por campo): controles programados y,
 * con --gfx-frame-every, la imagen de la HLE en ARCHIVO-<campo>.ppm (sin la
 * extensión de ARCHIVO) desde --trace-from. */
static bool gfx_dump(const char *path);

static void field_done(const char *gfx_path, uint32_t frame_every)
{
	pad_field();
	if (frame_every && gfx_path && tracing() && n64.bus.vi.fields % frame_every == 0) {
		char name[512];
		size_t len = strlen(gfx_path);
		if (len > 4 && !strcmp(gfx_path + len - 4, ".ppm"))
			len -= 4;
		snprintf(name, sizeof name, "%.*s-%u.ppm", (int)len, gfx_path, n64.bus.vi.fields);
		gfx_dump(name);
	}
}

/* La referencia de --lockstep hasta `to`, aplicando los controles en el
 * ciclo en que los recibió la máquina principal. */
static void ref_run_to(uint64_t to)
{
	while (pads.ref_next < pads.napplied && pads.applied[pads.ref_next].cycle <= to) {
		uint64_t at = pads.applied[pads.ref_next].cycle;
		if (ref.cpu.cycles < at)
			salsa64_n64_run(&ref, at - ref.cpu.cycles);
		pad_apply(&ref, &pads.applied[pads.ref_next].pad);
		pads.ref_next++;
	}
	if (ref.cpu.cycles < to)
		salsa64_n64_run(&ref, to - ref.cpu.cycles);
}

/* Audio: HLE y lo que sale por el AI (16 bits estéreo, orden de la N64). */
#define AUDIO_MAX (16u << 20)
static struct aud_state aud_state;
static struct salsa64_audio_sync aud_sync;
static uint8_t audio_buf[AUDIO_MAX];
static struct {
	uint32_t bytes, rate, dmas, gaps, rates;
	uint64_t next;      /* ciclo en que debía empezar el DMA siguiente */
	bool lost;          /* no cupo todo en audio_buf */
} audio;

/* Sumidero del AI: guarda las muestras y cuenta los huecos (un DMA que no
 * empieza justo cuando terminó el anterior). */
static void audio_push(void *ctx, const uint8_t *p, uint32_t bytes, uint32_t rate, uint64_t cycle)
{
	(void)ctx;
	if (audio.dmas && cycle != audio.next)
		audio.gaps++;
	if (audio.rate && rate != audio.rate)
		audio.rates++;
	audio.rate = rate;
	audio.dmas++;
	uint64_t div = (n64.bus.ai.regs[SALSA64_AI_DACRATE] & 0x3FFF) + 1;
	uint64_t num = (uint64_t)(bytes / 4) * 93750000ull * div, den = n64.bus.vi.clock_hz;
	audio.next = cycle + (num + den - 1) / den;
	uint32_t n = bytes < AUDIO_MAX - audio.bytes ? bytes : AUDIO_MAX - audio.bytes;
	memcpy(audio_buf + audio.bytes, p, n);
	audio.bytes += n;
	audio.lost |= n < bytes;
}

/* HLE gráfica: estado, búferes de salida y totales. */
static struct gfx_state gfx_state;
static struct salsa64_gfx_sync gfx_sync;

/* --lle: el microcódigo real del RSP, en rebanadas del hilo del CPU. */
static struct salsa64_lle_host lle_host;
static enum salsa64_lle_mode lle_mode;
static double lle_speed;
static struct gfx_vertex gfx_vtx[1u << 16] __attribute__((aligned(128)));
static uint8_t gfx_cmd[512u << 10] __attribute__((aligned(128)));
static uint8_t gfx_tex[4u << 20] __attribute__((aligned(128)));
static uint8_t gfx_cache[8u << 20] __attribute__((aligned(128)));
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
	/* Caché de texturas: el rasterizador termina cada tarea en gfx_done, así
	 * que la anterior ya no lee nada. */
	if (!getenv("SALSA64_NO_TEXCACHE")) {
		job->cache_ea = (uint64_t)(uintptr_t)gfx_cache;
		job->cache_cap = sizeof gfx_cache;
	}
	job->task = gfx.tasks + 1;
	job->done_task = gfx.tasks;
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
	s->tex_old += res->tex_old;
	s->tex_arena += res->tex_arena;
	s->tex_evicted += res->tex_evicted;
	s->loads += res->loads;
	s->unknown_cmds += res->unknown_cmds;
	if (res->unknown_cmds)
		s->last_unknown = res->last_unknown;
	s->approx_cc += res->approx_cc;
	s->dl_errors += res->dl_errors;
	s->skipped += res->skipped;
	if (gfx.raster)
		raster_task(job, res);
	if (trace_gfx_left && n64.bus.vi.fields > (trace_from ? trace_from : 500)) {
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

static unsigned trace_rsp_left, trace_vi_left, trace_audio_left;

/* --trace-audio: los opcodes de la lista de audio de una tarea (8 bytes por
 * comando), para comparar lo que el juego pide en escenas distintas. */
static void trace_alist(const struct salsa64_task *t)
{
	uint32_t a = t->data_ptr & 0x7FFFFF, n = t->data_size / 8;
	fprintf(stderr, "alist (campo %u, %u comandos):", n64.bus.vi.fields, n);
	for (uint32_t i = 0; i < n && a + 8 * i + 8 <= n64.bus.rdram_size; i++) {
		uint32_t w0 = salsa64_rd32(n64.bus.rdram + a + 8 * i), w1 = salsa64_rd32(n64.bus.rdram + a + 8 * i + 4);
		fprintf(stderr, " %02x", w0 >> 24);
		if (getenv("SALSA64_ALIST_VERBOSE"))
			fprintf(stderr, ":%06x:%08x", w0 & 0xFFFFFF, w1);
	}
	fprintf(stderr, "\n");
}

/* Muestreo del PC al final de cada bloque del bucle principal, por segmento:
 * cuánto del tiempo corre código mapeado por la TLB (kuseg, kseg2/3). */
enum { SEG_KSEG0, SEG_KSEG1, SEG_TLB, SEG_COUNT };
static uint64_t pc_samples[SEG_COUNT];

static void pc_sample(uint64_t pc)
{
	uint32_t top = (uint32_t)pc >> 29;
	pc_samples[top == 4 ? SEG_KSEG0 : top == 5 ? SEG_KSEG1 : SEG_TLB]++;
}

/* --lle-check-audio: cada tarea de audio que corre en el microcódigo real
 * se corre también en la HLE sobre una copia de la RDRAM; cuando el RSP
 * termina (en el siguiente arranque, o al final) se comparan los tramos
 * que la HLE escribió. Encuentra la primera divergencia del LLE. */
static struct {
	bool on, pending;
	uint8_t *rdram;         /* copia de la RDRAM con la salida de la HLE */
	struct aud_result res;
	uint32_t task, tasks, bad, bytes;
	int reports;
} shadow;

static void shadow_compare(void)
{
	shadow.pending = false;
	bool bad = false;
	for (uint32_t i = 0; i < shadow.res.nranges && i < AUD_MAX_RANGES; i++) {
		uint32_t a = shadow.res.range_addr[i] & 0x7FFFFF, n = shadow.res.range_len[i];
		if (n > n64.bus.rdram_size - a)
			n = n64.bus.rdram_size - a;
		shadow.bytes += n;
		const uint8_t *x = n64.bus.rdram + a, *y = shadow.rdram + a;
		uint32_t k = 0;
		while (k < n && x[k] == y[k])
			k++;
		if (k == n)
			continue;
		bad = true;
		if (shadow.reports < 8) {
			shadow.reports++;
			uint32_t d = 0;
			for (uint32_t j = 0; j < n; j++)
				d += x[j] != y[j];
			fprintf(stderr, "lle-check: tarea de audio %u, tramo %u [%06x, %u bytes]: %u bytes distintos desde "
				"+%u: lle %02x%02x%02x%02x %02x%02x%02x%02x, hle %02x%02x%02x%02x %02x%02x%02x%02x\n",
				shadow.task, i, a, n, d, k, x[k], x[k + 1], x[k + 2], x[k + 3], x[k + 4], x[k + 5], x[k + 6],
				x[k + 7], y[k], y[k + 1], y[k + 2], y[k + 3], y[k + 4], y[k + 5], y[k + 6], y[k + 7]);
		}
	}
	shadow.bad += bad;
}

static void shadow_start(const struct salsa64_task *t)
{
	enum salsa64_abi abi = salsa64_rsp_detect_abi(&n64.bus, t);
	if (abi == SALSA64_ABI_UNKNOWN)
		return;
	if (!shadow.rdram)
		shadow.rdram = malloc(n64.bus.rdram_size);
	memcpy(shadow.rdram, n64.bus.rdram, n64.bus.rdram_size);
	struct aud_job job;
	salsa64_audio_job(&job, &n64.bus, t, abi);
	job.rdram_ea = (uint64_t)(uintptr_t)shadow.rdram;
	aud_run(&aud_state, &job, &shadow.res);
	shadow.pending = true;
	shadow.task = ++shadow.tasks;
	/* SALSA64_LLE_DUMP_TASK=N: la lista de comandos de esa tarea y lo que
	 * la HLE escribió, para seguir la divergencia a mano. */
	const char *dump = getenv("SALSA64_LLE_DUMP_TASK");
	if (dump && (uint32_t)atoi(dump) == shadow.task) {
		fprintf(stderr, "lle-check: tarea de audio %u, alist %06x (%u bytes), ABI %s:\n", shadow.task,
			job.alist, job.alist_size, salsa64_abi_name(abi));
		for (uint32_t i = 0; i + 8 <= job.alist_size; i += 8)
			fprintf(stderr, "  %3u: %08x %08x\n", i / 8, salsa64_rd32(n64.bus.rdram + ((job.alist + i) & 0x7FFFFF)),
				salsa64_rd32(n64.bus.rdram + ((job.alist + i + 4) & 0x7FFFFF)));
		for (uint32_t i = 0; i < shadow.res.nranges && i < AUD_MAX_RANGES; i++)
			fprintf(stderr, "  tramo %u: %06x %u bytes\n", i, shadow.res.range_addr[i], shadow.res.range_len[i]);
	}
}

static void trace_rsp(void *ctx, const struct salsa64_task *t, enum salsa64_gbi gbi)
{
	(void)ctx;
	if (shadow.on) {
		if (shadow.pending)
			shadow_compare();
		if (t->type == SALSA64_TASK_AUDIO)
			shadow_start(t);
	}
	if (trace_audio_left && t->type == SALSA64_TASK_AUDIO && tracing()) {
		trace_audio_left--;
		trace_alist(t);
	}
	if (!trace_rsp_left || !tracing())
		return;
	trace_rsp_left--;
	fprintf(stderr, "RSP en ciclo %llu: type=%u flags=%x ucode=%08x/%x data=%08x/%x data_ptr=%08x/%x "
		"boot=%08x/%x gbi=%d\n", (unsigned long long)n64.cpu.cycles, t->type, t->flags, t->ucode,
		t->ucode_size, t->ucode_data, t->ucode_data_size, t->data_ptr, t->data_size, t->ucode_boot,
		t->ucode_boot_size, gbi);
	/* Identificación del microcódigo: su texto ("RSP Gfx ucode …"), el CRC-32
	 * del código y de ucode_data (para los que no traen texto) y, en las de
	 * audio, el ABI y las palabras de ucode_data que lo distinguen. */
	char text[96];
	salsa64_rsp_ucode_text(&n64.bus, t->ucode_data, text, sizeof text);
	if (text[0])
		fprintf(stderr, "  %s\n", text);
	uint32_t base = t->ucode_data & 0x7FFFFF, size = t->ucode_data_size;
	if (base >= n64.bus.rdram_size)
		size = 0;
	else if (!size || size > 0x1000)
		size = 0x1000;
	if (size && size > n64.bus.rdram_size - base)
		size = n64.bus.rdram_size - base;
	uint32_t code = t->ucode & 0x7FFFFF, code_size = t->ucode_size;
	if (code >= n64.bus.rdram_size || code_size > n64.bus.rdram_size - code)
		code_size = 0;
	fprintf(stderr, "  crc32: ucode %08x (0x%x bytes), ucode_data %08x (0x%x bytes)\n",
		code_size ? salsa64_crc32(n64.bus.rdram + code, code_size) : 0, code_size,
		size ? salsa64_crc32(n64.bus.rdram + base, size) : 0, size);
	if (t->type == SALSA64_TASK_AUDIO && size >= 0x34)
		fprintf(stderr, "  audio %s; ucode_data[00 10 28 30]: %08x %08x %08x %08x\n",
			salsa64_abi_name(salsa64_rsp_detect_abi(&n64.bus, t)), salsa64_rd32(n64.bus.rdram + base),
			salsa64_rd32(n64.bus.rdram + base + 0x10), salsa64_rd32(n64.bus.rdram + base + 0x28),
			salsa64_rd32(n64.bus.rdram + base + 0x30));
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
		"%.0f comandos, %.0f triángulos, %.0f rectángulos, %.0f dibujos, %.1f cargas a la TMEM, %.1f texturas "
		"decodificadas (+%.1f reusadas, %.1f de tareas anteriores; %.1f al arena, %.1f desalojadas); "
		"%u comandos sin implementar (último 0x%02X), %u de otro microcódigo saltados, %u combiners "
		"aproximados, %u errores, %u tareas cortadas, banderas 0x%02X\n",
		gfx.tasks, n64.bus.rsp.stats.other + n64.bus.rsp.stats.audio, n64.bus.rsp.stats.unknown_gbi,
		s->commands / n, s->triangles / n, s->rects / n, s->draws / n, s->loads / n, s->textures / n,
		s->tex_reused / n, s->tex_old / n, s->tex_arena / n, s->tex_evicted / n,
		s->unknown_cmds, s->last_unknown, s->skipped, s->approx_cc, s->dl_errors, n64.bus.rsp.stats.cut,
		s->flags);
}

static void audio_summary(void)
{
	const struct salsa64_audio_stats *a = &aud_sync.stats;
	const struct salsa64_rsp_stats *r = &n64.bus.rsp.stats;
	fprintf(stderr, "salsa64-run: audio: %u tareas (%u con ABI desconocido; último ABI %s), %u comandos, "
		"%u sin implementar (último 0x%08X), %u errores; AI: %u DMA, %u muestras a %u Hz, %u huecos, "
		"%u cambios de frecuencia%s\n", r->audio, r->unknown_abi, salsa64_abi_name(n64.bus.rsp.abi),
		a->commands, a->unknown, a->last_unknown, a->errors, audio.dmas, audio.bytes / 4, audio.rate,
		audio.gaps ? audio.gaps - 1 : 0, audio.rates, audio.lost ? " (no cupo todo)" : "");
}

static void lle_summary(void)
{
	const struct salsa64_lle_host *h = &lle_host;
	const struct gfx_result *d = &gfx_sync.dpc_sum;
	fprintf(stderr, "salsa64-run: rsp lle (%s, %.3f instr/ciclo): %u arranques, %llu instrucciones (%u tareas, "
		"%.0f por tarea, la más larga %u), %u rebanadas, %u sincronías; rdp: %u tramos, %u comandos, "
		"%u triángulos, %u rectángulos, %u dibujos, %u sin implementar, %u errores\n",
		lle_mode == SALSA64_LLE_ALL ? "todo" : "sin HLE", (double)h->ipc_num / h->ipc_den, h->starts,
		(unsigned long long)h->instructions, h->tasks_done,
		h->tasks_done ? (double)h->instructions / h->tasks_done : 0.0, h->task_max, h->slices, h->syncs,
		gfx_sync.dpc_runs, d->commands, d->triangles, d->rects, d->draws, d->unknown_cmds, d->dl_errors);
	const uint32_t *cr = h->rsp.c0_reads, *cw = h->rsp.c0_writes;
	fprintf(stderr, "salsa64-run: rsp lle COP0: SP_STATUS %u lecturas/%u escrituras, semáforo %u/%u, DMA_BUSY/FULL %u, "
		"DMA %u lecturas/%u escrituras, DP %u lecturas/%u escrituras\n", cr[4], cw[4], cr[7], cw[7], cr[5] + cr[6],
		cw[2], cw[3], cr[8] + cr[9] + cr[10] + cr[11] + cr[12] + cr[13] + cr[14] + cr[15],
		cw[8] + cw[9] + cw[10] + cw[11]);
}

static bool audio_wav(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	uint32_t data = audio.bytes, rate = audio.rate ? audio.rate : 32000;
	uint8_t h[44];
	memcpy(h, "RIFF", 4);
	for (int i = 0; i < 4; i++) h[4 + i] = (uint8_t)((36 + data) >> (8 * i));
	memcpy(h + 8, "WAVEfmt ", 8);
	static const uint8_t fmt[8] = { 16, 0, 0, 0, 1, 0, 2, 0 };  /* PCM, 2 canales */
	memcpy(h + 16, fmt, 8);
	for (int i = 0; i < 4; i++) h[24 + i] = (uint8_t)(rate >> (8 * i));
	for (int i = 0; i < 4; i++) h[28 + i] = (uint8_t)((rate * 4) >> (8 * i));
	h[32] = 4, h[33] = 0, h[34] = 16, h[35] = 0;
	memcpy(h + 36, "data", 4);
	for (int i = 0; i < 4; i++) h[40 + i] = (uint8_t)(data >> (8 * i));
	fwrite(h, 1, sizeof h, f);
	for (uint32_t i = 0; i < data; i += 2) {
		uint8_t le[2] = { audio_buf[i + 1], audio_buf[i] };  /* la N64 es big-endian */
		fwrite(le, 1, 2, f);
	}
	fprintf(stderr, "salsa64-run: audio del AI en %s (%u muestras a %u Hz)\n", path, data / 4, rate);
	return fclose(f) == 0;
}

/* Lo que salió por el AI contra el tono de audiotest. */
static bool audiotest_check(void)
{
	struct tone_gen g = { 0, 0, 0 };
	uint32_t frames = audio.bytes / 4, bad = 0, first = 0;
	for (uint32_t i = 0; i < frames; i++) {
		int16_t l, r;
		tone_next(&g, &l, &r);
		int16_t gl = (int16_t)salsa64_rd16(audio_buf + 4 * i), gr = (int16_t)salsa64_rd16(audio_buf + 4 * i + 2);
		if (gl != l || gr != r) {
			if (!bad++)
				first = i;
		}
	}
	/* Huecos: el primer DMA empieza con la cola vacía; ninguno más. Las dos
	 * fases de la ROM son 180 búferes; el último puede seguir en la cola. */
	uint32_t gaps = audio.gaps ? audio.gaps - 1 : 0;
	bool ok = frames >= 179 * TONE_FRAMES && !bad && !gaps && !audio.rates && !audio.lost;
	char where[48] = "";
	if (bad)
		snprintf(where, sizeof where, ", la primera en la muestra %u", first);
	fprintf(stderr, "salsa64-run: audiotest: audio %s (%u muestras, %u distintas%s, %u huecos, %u Hz)\n",
		ok ? "OK" : "FALLO", frames, bad, where, gaps, audio.rate);
	return ok;
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

static void ref_isv_out(void *ctx, const uint8_t *text, uint32_t len)
{
	(void)ctx, (void)text, (void)len;
}

#define DIFF(name, a, b)                                                                        \
	do {                                                                                    \
		if ((uint64_t)(a) != (uint64_t)(b)) {                                           \
			fprintf(stderr, "  %-10s rec=%016llx intérprete=%016llx\n", name,      \
				(unsigned long long)(a), (unsigned long long)(b));              \
			bad = true;                                                             \
		}                                                                               \
	} while (0)

/* Después de cada salida del código generado: la referencia al mismo ciclo y
 * comparar. */
static void lockstep_check(struct salsa64_rec *r, uint64_t block_pc, void *ctx)
{
	(void)r, (void)ctx;
	const struct salsa64_cpu *a = &n64.cpu, *b = &ref.cpu;
	lock.history[lock.exits % 16] = block_pc;
	lock.exits++;
	/* La referencia despacha los eventos que vencen en el ciclo al que
	 * llega; el despachador del recompilador lo haría en su próxima vuelta. */
	if (a->cycles >= n64.sched.next)
		salsa64_sched_dispatch(&n64.sched, a->cycles);
	ref_run_to(a->cycles);

	bool bad = false;
	char name[16];
	DIFF("cycles", a->cycles, b->cycles);
	DIFF("pc", a->pc, b->pc);
	DIFF("next_pc", a->next_pc, b->next_pc);
	DIFF("branch", a->branch, b->branch);
	for (int k = 1; k < 32; k++) {
		snprintf(name, sizeof name, "r%d", k);
		DIFF(name, a->gpr[k], b->gpr[k]);
	}
	DIFF("hi", a->hi, b->hi);
	DIFF("lo", a->lo, b->lo);
	for (int k = 0; k < 32; k++) {
		snprintf(name, sizeof name, "f%d", k);
		DIFF(name, a->fpr[k], b->fpr[k]);
	}
	DIFF("fcr31", a->fcr31, b->fcr31);
	DIFF("status", a->status, b->status);
	DIFF("cause", a->cause, b->cause);
	DIFF("epc", a->epc, b->epc);
	DIFF("errorepc", a->error_epc, b->error_epc);
	DIFF("badvaddr", a->bad_vaddr, b->bad_vaddr);
	DIFF("compare", a->compare, b->compare);
	DIFF("count_off", a->count_offset, b->count_offset);
	DIFF("llbit", a->llbit, b->llbit);
	DIFF("entryhi", a->entry_hi, b->entry_hi);
	DIFF("mi.intr", n64.bus.mi.intr, ref.bus.mi.intr);
	DIFF("vi.fields", n64.bus.vi.fields, ref.bus.vi.fields);
	if (!bad && (lock.exits % lock.ram_every == 0 || n64.bus.vi.fields != lock.fields)) {
		lock.fields = n64.bus.vi.fields;
		for (uint32_t pa = 0; pa < n64.bus.rdram_size; pa += 4) {
			if (salsa64_rd32(rdram + pa) != salsa64_rd32(ref_rdram + pa)) {
				fprintf(stderr, "  RDRAM %08x rec=%08x intérprete=%08x\n", pa,
					salsa64_rd32(rdram + pa), salsa64_rd32(ref_rdram + pa));
				bad = true;
				break;
			}
		}
	}
	if (!bad)
		return;
	fprintf(stderr, "salsa64-run: --lockstep: diferencia después del bloque %016llx (salida %llu, ciclo %llu)\n"
		"  bloques anteriores:", (unsigned long long)block_pc, (unsigned long long)lock.exits,
		(unsigned long long)a->cycles);
	for (unsigned k = 1; k <= 16 && k <= lock.exits; k++)
		fprintf(stderr, " %08llx", (unsigned long long)(uint32_t)lock.history[(lock.exits - k) % 16]);
	fputc('\n', stderr);
	exit(4);
}

static void save_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("salsa64-run: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
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
	if (!trace_exc_left || !tracing() || (trace_exc_faults_only && SALSA64_CAUSE_EXC(cpu->cause) == 0))
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

/* Opciones largas sin letra (códigos fuera de ASCII). */
enum { OPT_REC = 0x100, OPT_LOCKSTEP, OPT_PAD, OPT_TRACE_FROM, OPT_TRACE_FAULTS, OPT_RAM_DUMP, OPT_TRACE_AUDIO,
       OPT_FRAME_EVERY, OPT_LLE, OPT_LLE_SPEED, OPT_LLE_CHECK };

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
	      "      --trace-faults N  igual, sin las interrupciones\n"
	      "      --trace-from C    trazar solo desde el campo C del VI\n"
	      "      --pad GUION       control 1 por campos: \"300:START;306:-;400:A+Z@0,80\"\n"
	      "                        (también CAMPO:BOTONES,… con x80/y-50); vale hasta el siguiente\n"
	      "      --frame ARCHIVO   al terminar, guardar en PPM la imagen del VI\n"
	      "      --ram-dump ARCHIVO  al terminar, volcar la RDRAM (tools/libultra-threads.py)\n"
	      "      --rec             usar el recompilador (host ppc64 big-endian)\n"
	      "      --lockstep[=N]    con --rec, comparar contra el intérprete después de cada\n"
	      "                        bloque, y la RDRAM en cada campo del VI y cada N bloques\n"
	      "                        (por omisión 4096)\n"
	      "      --no-idle         no saltar los bucles de espera\n"
	      "      --save TIPO       tipo de guardado: ninguno, eeprom4k, eeprom16k, sram256k\n"
	      "      --save-dir DIR    leer y escribir el guardado del cartucho en DIR\n"
	      "      --gfx-frame ARCHIVO  al terminar, guardar en PPM lo que dibujó la HLE gráfica\n"
	      "      --gfx-frame-every N  con --pad, además cada N campos desde --trace-from (ARCHIVO-<campo>.ppm)\n"
	      "      --gfxtest-check   verificar la imagen de la HLE con los puntos de gfxtest\n"
	      "      --trace-rsp N     mostrar las primeras N tareas del RSP\n"
	      "      --trace-audio N   opcodes de la lista de audio de las primeras N tareas de audio\n"
	      "      --trace-vi N      mostrar la imagen del VI en los últimos N campos\n"
	      "      --audio-wav ARCHIVO  al terminar, guardar en WAV lo que salió por el AI\n"
	      "      --audiotest-check comparar lo que salió por el AI con el tono de audiotest\n"
	      "      --raster-dump F   dibujar un volcado de la HLE de la PS3 (gfxdump.bin) en\n"
	      "                        --gfx-frame y salir (sin ROM)\n"
	      "      --lle[=auto|all]  RSP a bajo nivel: el microcódigo real para lo que no tiene\n"
	      "                        HLE (auto) o para todo (all); el RDP consume sus comandos\n"
	      "      --lle-speed F     instrucciones del RSP por ciclo del CPU (nominal 0,667)\n"
	      "      --lle-check-audio con --lle=all, comparar cada tarea de audio con la HLE\n", out);
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
		{ "rec", no_argument, NULL, OPT_REC },
		{ "lockstep", optional_argument, NULL, OPT_LOCKSTEP },
		{ "pad", required_argument, NULL, OPT_PAD },
		{ "trace-from", required_argument, NULL, OPT_TRACE_FROM },
		{ "trace-faults", required_argument, NULL, OPT_TRACE_FAULTS },
		{ "ram-dump", required_argument, NULL, OPT_RAM_DUMP },
		{ "trace-audio", required_argument, NULL, OPT_TRACE_AUDIO },
		{ "gfx-frame-every", required_argument, NULL, OPT_FRAME_EVERY },
		{ "no-idle", no_argument, NULL, 'I' },
		{ "save", required_argument, NULL, 'E' },
		{ "save-dir", required_argument, NULL, 'W' },
		{ "gfx-frame", required_argument, NULL, 'G' },
		{ "gfxtest-check", no_argument, NULL, 'K' },
		{ "trace-rsp", required_argument, NULL, 'P' },
		{ "trace-vi", required_argument, NULL, 'V' },
		{ "lle", optional_argument, NULL, OPT_LLE },
		{ "lle-speed", required_argument, NULL, OPT_LLE_SPEED },
		{ "lle-check-audio", no_argument, NULL, OPT_LLE_CHECK },
		{ "trace-gfx", required_argument, NULL, 'Q' },
		{ "raster-dump", required_argument, NULL, 'D' },
		{ "audio-wav", required_argument, NULL, 'A' },
		{ "audiotest-check", no_argument, NULL, 'Z' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	uint64_t max_cycles = DEFAULT_CYCLES, slice = DEFAULT_SLICE;
	uint32_t ram = SALSA64_RDRAM_MAX;
	int tv = SALSA64_BOOT_TV_AUTO, save_type = SALSA64_SAVE_AUTO, c;
	bool regs = false, idle = true, use_rec = false, lockstep = false;
	const char *frame_path = NULL, *gfx_path = NULL, *save_dir = NULL;
	bool gfx_check = false, audio_check = false;
	const char *wav_path = NULL;
	const char *dump_path = NULL, *ram_path = NULL;
	uint32_t frame_every = 0;

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
		case OPT_REC: use_rec = true; break;
		case OPT_LOCKSTEP:
			use_rec = lockstep = true;
			lock.ram_every = optarg ? strtoull(optarg, NULL, 0) : 4096;
			if (!lock.ram_every)
				lock.ram_every = 1;
			break;
		case 'I': idle = false; break;
		case OPT_PAD:
			if (!pad_parse(optarg)) {
				fprintf(stderr, "salsa64-run: --pad: no se entiende \"%s\"\n", optarg);
				return 3;
			}
			break;
		case OPT_TRACE_FROM: trace_from = (uint32_t)strtoul(optarg, NULL, 0); break;
		case OPT_TRACE_FAULTS: trace_exc_left = strtoull(optarg, NULL, 0); trace_exc_faults_only = true; break;
		case OPT_RAM_DUMP: ram_path = optarg; break;
		case OPT_TRACE_AUDIO: trace_audio_left = (unsigned)strtoul(optarg, NULL, 0); break;
		case OPT_FRAME_EVERY: frame_every = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'W': save_dir = optarg; break;
		case 'E':
			for (int t = 0; t < SALSA64_SAVE_NTYPES; t++)
				if (!strcmp(optarg, salsa64_save_name(t)))
					save_type = t;
			if (save_type == SALSA64_SAVE_AUTO) {
				usage(stderr);
				return 3;
			}
			break;
		case 'G': gfx_path = optarg; gfx.raster = true; break;
		case 'K': gfx_check = true; gfx.raster = true; break;
		case 'P': trace_rsp_left = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'V': trace_vi_left = (unsigned)strtoul(optarg, NULL, 0); break;
		case OPT_LLE:
			if (!optarg || !strcmp(optarg, "auto"))
				lle_mode = SALSA64_LLE_AUTO;
			else if (!strcmp(optarg, "all"))
				lle_mode = SALSA64_LLE_ALL;
			else {
				fprintf(stderr, "salsa64-run: --lle=%s no se entiende (auto, all)\n", optarg);
				return 3;
			}
			break;
		case OPT_LLE_SPEED: lle_speed = atof(optarg); break;
		case OPT_LLE_CHECK: shadow.on = true; break;
		case 'D': dump_path = optarg; break;
		case 'A': wav_path = optarg; break;
		case 'Z': audio_check = true; break;
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
	salsa64_cpu_set_idle(&n64.cpu, idle);
	const struct salsa64_gfx_sink sink = { NULL, gfx_acquire, gfx_done };
	salsa64_gfx_sync_init(&gfx_sync, &gfx_state, &n64.bus, &sink);
	salsa64_rsp_set_gfx(&n64.bus, &gfx_sync.backend);
	salsa64_audio_sync_init(&aud_sync, &aud_state, &n64.bus);
	salsa64_rsp_set_audio(&n64.bus, &aud_sync.backend);
	salsa64_ai_set_sink(&n64.bus, audio_push, NULL);
	if (lle_mode != SALSA64_LLE_OFF) {
		salsa64_lle_host_init(&lle_host, &n64.bus);
		if (lle_speed > 0.0)
			salsa64_lle_host_speed(&lle_host, lle_speed);
		salsa64_rsp_set_lle(&n64.bus, &lle_host.backend, lle_mode);
	}
	salsa64_rsp_set_dpc(&n64.bus, &gfx_sync.dpc);
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
	savefile_open(&save_file, &save, &n64.bus, rom, save_dir, NULL, save_type, save_log);

	if (use_rec) {
		void *code = salsa64_host_rec_buffer();
		rec.opts = salsa64_host_rec_opts();
		if (!code || !salsa64_rec_init(&rec, &n64.cpu, code, SALSA64_HOST_REC_BYTES)) {
			fprintf(stderr, "salsa64-run: el recompilador no corre en este host\n");
			return 3;
		}
		if (lockstep) {
			/* La referencia arranca igual; cada bloque vuelve a C para comparar. */
			salsa64_n64_init(&ref, ref_rdram, ram);
			salsa64_cpu_set_idle(&ref.cpu, idle);
			salsa64_bus_set_isviewer(&ref.bus, ref_isv_buf, ref_isv_out, NULL);
			salsa64_bus_set_rom(&ref.bus, rom, (uint32_t)(rom_size > UINT32_MAX ? UINT32_MAX : rom_size));
			struct salsa64_boot_info ref_info;
			if (!salsa64_boot_hle(&ref, tv, &ref_info)) {
				fprintf(stderr, "salsa64-run: la referencia no arrancó\n");
				return 3;
			}
			/* El mismo guardado, en una copia: sin él la PIF de la referencia
			 * responde «sin dispositivo» donde la principal ve la EEPROM. */
			if (n64.bus.save) {
				ref_save = save;
				salsa64_save_attach(&ref.bus, &ref_save);
			}
			rec.no_link = true;
			rec.after_exit = lockstep_check;
		}
		fprintf(stderr, "salsa64-run: recompilador activo%s%s; caché en %p, llamadas a C %s\n",
			lockstep ? " con --lockstep" : "", rec.opts & SALSA64_REC_OPT_PIN ? ", registros fijos" : "",
			code, rec.calls_near ? "con bl" : "por mtctr/bctrl");
	}

	/* Bucle principal: bloques de `slice` ciclos y verificación entre ellos. */
	enum stop_reason reason = STOP_NONE;
	uint64_t start_cycles = n64.cpu.cycles, start_idle = n64.cpu.idle_cycles, t0 = now_us();
	while (reason == STOP_NONE) {
		uint64_t done = n64.cpu.cycles - start_cycles;
		if (done >= max_cycles) {
			reason = STOP_CYCLES;
			break;
		}
		bool trace_vi = trace_vi_left && max_cycles - done < (uint64_t)trace_vi_left * 1600000u;
		if (trace_vi) {
			/* Últimos campos: de a uno, con la imagen que describe el VI (los
			 * controles programados se aplican en field_done). */
			salsa64_n64_run_field(&n64);
			field_done(gfx_path, frame_every);
			struct salsa64_vi_frame f;
			if (trace_vi && salsa64_vi_frame(&n64.bus, &f))
				fprintf(stderr, "campo %u: origen %06x %ux%u stride %u bpp %u (V_VIDEO %08x Y_SCALE %08x)\n",
					n64.bus.vi.fields, f.origin, f.width, f.height, f.stride, f.bpp,
					n64.bus.vi.regs[SALSA64_VI_V_VIDEO], n64.bus.vi.regs[SALSA64_VI_Y_SCALE]);
		} else if (pads.n) {
			/* Con controles programados, campo por campo: el control cambia
			 * justo al empezar uno. */
			salsa64_n64_run_field(&n64);
			field_done(gfx_path, frame_every);
		} else {
			salsa64_n64_run(&n64, max_cycles - done < slice ? max_cycles - done : slice);
		}
		pc_sample(n64.cpu.pc);
		if (watch.hit != STOP_NONE)
			reason = watch.hit;
		else if (cpu_stuck())
			reason = STOP_STUCK;
	}
	uint64_t us = now_us() - t0, cycles = n64.cpu.cycles - start_cycles;
	uint64_t idle_cycles = n64.cpu.idle_cycles - start_idle;
	if (watch.midline)
		putchar('\n');
	fflush(stdout);

	static const char *const reason_names[] = {
		[STOP_UNTIL] = "apareció el texto esperado",
		[STOP_FAIL] = "apareció el texto de falla",
		[STOP_CYCLES] = "se agotaron los ciclos",
		[STOP_STUCK] = "bucle infinito sin interrupciones posibles",
	};
	uint64_t samples = pc_samples[SEG_KSEG0] + pc_samples[SEG_KSEG1] + pc_samples[SEG_TLB];
	if (samples)
		fprintf(stderr, "salsa64-run: pc al final de cada bloque (%llu muestras): KSEG0 %.1f %%, KSEG1 %.1f %%, "
			"TLB %.1f %%\n", (unsigned long long)samples, 100.0 * pc_samples[SEG_KSEG0] / samples,
			100.0 * pc_samples[SEG_KSEG1] / samples, 100.0 * pc_samples[SEG_TLB] / samples);
	/* Una instrucción por ciclo, salvo los ciclos saltados en bucles de espera. */
	fprintf(stderr, "salsa64-run: %s; %llu ciclos (%.2f s de consola) en %.2f s, %.1f MIPS, "
		"%.1f %% en bucles de espera, pc=0x%016llx\n",
		reason_names[reason], (unsigned long long)cycles, cycles / 93.75e6, us / 1e6,
		us ? (cycles - idle_cycles) / (double)us : 0.0,
		cycles ? 100.0 * idle_cycles / cycles : 0.0, (unsigned long long)n64.cpu.pc);
	if (use_rec) {
		const struct salsa64_rec_stats *st = &rec.stats;
		fprintf(stderr, "salsa64-run: recompilador: %llu bloques (%llu fuera de KSEG0, %u KB de caché), "
			"%llu solo intérprete, %llu entradas, %llu enlaces, %llu invalidados, %llu vaciados, "
			"%llu traducciones, %llu instr. interpretadas (%.1f %%), %llu llamadas al intérprete%s\n",
			(unsigned long long)st->compiled, (unsigned long long)st->compiled_x, salsa64_rec_used(&rec) / 256,
			(unsigned long long)st->interp_only, (unsigned long long)st->entries,
			(unsigned long long)st->linked, (unsigned long long)st->invalidated,
			(unsigned long long)st->flushes, (unsigned long long)st->vt_probes,
			(unsigned long long)st->interp_steps, cycles ? 100.0 * (double)st->interp_steps / (double)cycles : 0.0,
			(unsigned long long)st->exec_calls, lockstep ? "; --lockstep sin diferencias" : "");
	}
	savefile_close(&save_file);
	if (regs)
		dump_regs();
	if (ram_path) {
		FILE *f = fopen(ram_path, "wb");
		if (!f || fwrite(rdram, 1, n64.bus.rdram_size, f) != n64.bus.rdram_size || fclose(f) != 0) {
			fprintf(stderr, "salsa64-run: no se pudo escribir %s\n", ram_path);
			return 3;
		}
		fprintf(stderr, "salsa64-run: RDRAM (%u MB) en %s\n", n64.bus.rdram_size >> 20, ram_path);
	}
	if (gfx.tasks || n64.bus.rsp.stats.unknown_gbi)
		gfx_summary();
	if (n64.bus.rsp.stats.audio || audio.dmas)
		audio_summary();
	if (lle_mode != SALSA64_LLE_OFF)
		lle_summary();
	if (shadow.on) {
		if (shadow.pending)
			shadow_compare();
		fprintf(stderr, "salsa64-run: lle-check: %u tareas de audio comparadas con la HLE, %u bytes, %u tareas "
			"distintas\n", shadow.tasks, shadow.bytes, shadow.bad);
	}
	if (wav_path && !audio_wav(wav_path)) {
		free(rom);
		return 3;
	}
	if ((frame_path && !dump_frame(frame_path)) || (gfx_path && !gfx_dump(gfx_path))) {
		free(rom);
		return 3;
	}
	free(rom);
	if (gfx_check && !gfxtest_check())
		return 1;
	if (audio_check && !audiotest_check())
		return 1;

	switch (reason) {
	case STOP_FAIL:   return 1;
	case STOP_STUCK:  return 2;
	case STOP_CYCLES: return watch.until ? 2 : 0;
	default:          return 0;
	}
}
