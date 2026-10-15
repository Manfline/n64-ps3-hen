/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Salsa64 — perfilado en la PS3 (perfilado_ps3.h). */
#include "perfilado_ps3.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/systime.h>
#include <sys/thread.h>

#include <spukit/spukit.h>

#include "core/save.h"
#include "platform/ps3/log.h"
#include "rdp/rdp.h"
#include "rsp/gfx/gfx.h"

#define PERF_IO_PRIO    1500            /* el hilo que escribe: por debajo de todos */
#define PERF_IO_STACK   (64u << 10)
#define PERF_DRAIN_US   500000u
#define PERF_EMU_RING   (2u << 20)      /* compilaciones y ventanas de la traza */
#define PERF_PRES_RING  (512u << 10)
#define PERF_RESERVE    (16ull << 20)   /* lugar libre al abrir: lo que puede llegar a escribir una sesión larga */

struct perf_app perf;
static uint8_t *emu_mem, *pres_mem;      /* los anillos: la primera vez, para siempre */

uint32_t perf_ms(void)
{
	return (uint32_t)((sysGetSystemTime() - perf.t0) / 1000u);
}

static uint32_t field_now(void)
{
	return perf.field ? *perf.field : 0;
}

/* ---- Recompilador (hilo que emula) ------------------------------------------------ */

static void on_block(void *ctx, const struct salsa64_rec_trace_block *b)
{
	(void)ctx;
	if (perf.cfg.blocks && perf_room(&perf.ses))
		perf_put_block(&perf.emu, perf_ms(), field_now(), b);
}

static void on_evict(void *ctx, unsigned kind, unsigned seg, unsigned killed)
{
	(void)ctx;
	perf_put_evict(&perf.emu, perf_ms(), field_now(), kind, seg, killed);
}

void perf_app_field(struct salsa64_rec *rec, uint32_t field_cycles)
{
	struct salsa64_rec_trace *t = perf.trace;
	if (!perf.on || !t || rec->trace != t || !t->seq)
		return;
	uint32_t field = field_now();
	/* Llena, o abierta hace más de 4 campos (poco código generado): se recoge. */
	unsigned len = salsa64_rec_trace_take(rec, t->armed && field - perf.trace_field0 > 4);
	if (len) {
		if (perf_room(&perf.ses))
			perf_put_trace(&perf.emu, perf_ms(), perf.trace_field0, t->win_cycles, t, len);
		perf.traces++;
	}
	uint64_t now = sysGetSystemTime();
	if (!t->armed && !t->arm_at && now >= perf.trace_next_us) {
		/* En un punto al azar del campo que viene. */
		perf.rng = perf.rng * 1103515245u + 12345u;
		t->arm_at = rec->cpu->cycles + 1 + (field_cycles ? (perf.rng >> 8) % field_cycles : 0);
		perf.trace_field0 = field;
		perf.trace_next_us = now + (uint64_t)perf.cfg.trace_s * 1000000u;
	}
}

/* ---- Tareas gráficas (hilo de presentación) --------------------------------------- */

static bool sig_new(uint32_t sig)
{
	for (uint32_t k = 0; k < perf.nsigs; k++)
		if (perf.sigs[k] == sig)
			return false;
	if (perf.nsigs < sizeof perf.sigs / sizeof perf.sigs[0])
		perf.sigs[perf.nsigs++] = sig;
	return true;
}

static void dump_task(struct perf_task *pt, unsigned why, uint32_t what)
{
	static const char *const names[] = { "", "desconocido", "aproximado", "cortada" };
	uint64_t now = sysGetSystemTime();
	if (perf.dumps >= perf.cfg.dumps || (perf.dumps && now - perf.dump_last_us < perf.cfg.dump_gap_s * 1000000ull) ||
	    perf.ses.over)
		return;
	char dir[PERF_PATH_MAX + 16], path[PERF_PATH_MAX + 64];
	snprintf(dir, sizeof dir, "%s/volcados", perf.ses.dir);
	mkdir(dir, 0777);
	snprintf(path, sizeof path, "%s/%u-%u-%s.bin", dir, perf.ses.n, pt->field, names[why]);
	perf.dump_last_us = now;
	perf.dumps++;
	bool ok = salsa64_rdp_dump(path);
	if (!ok) {
		perf.dump_fails++;
		return;
	}
	pt->news |= PERF_TASK_DUMPED;
	const char *name = strrchr(path, '/') + 1;
	struct perf_rw w;
	uint32_t n = (uint32_t)strlen(name);
	if (perf_begin(&w, &perf.pres, PERF_R_DUMP, 20 + n)) {
		perf_w32(&w, perf_ms());
		perf_w32(&w, pt->field);
		perf_w32(&w, pt->task);
		perf_w32(&w, why);
		perf_w32(&w, what);
		perf_wbytes(&w, name, n);
		perf_end(&w);
	}
	salsa64_log("perfilado: volcado de la HLE en %s (%s, tarea %u)", path, names[why], pt->task);
}

static void task_hook(const struct salsa64_rdp_task_info *info)
{
	if (!perf.on)
		return;
	const struct gfx_result *r = info->res;
	uint64_t hz = spukit_tick_hz();
	struct perf_task pt;
	memset(&pt, 0, sizeof pt);
	pt.ms = perf_ms();
	pt.field = field_now();
	pt.task = info->task;
	pt.commands = r->commands;
	pt.triangles = r->triangles;
	pt.draws = (uint16_t)perf_sat16(r->draws);
	pt.textures = (uint16_t)perf_sat16(r->textures);
	pt.unknown = (uint16_t)perf_sat16(r->unknown_cmds);
	pt.last_unknown = (uint8_t)r->last_unknown;
	pt.flags = (uint8_t)r->flags;
	pt.approx_cc = (uint16_t)perf_sat16(r->approx_cc);
	pt.approx_draws = (uint16_t)perf_sat16(info->approx_draws);
	pt.skipped = (uint16_t)perf_sat16(r->skipped);
	pt.dl_errors = (uint16_t)perf_sat16(r->dl_errors);
	pt.spe_us = (uint16_t)perf_sat16(hz ? (uint64_t)r->t_total * 1000000u / hz : 0);
	pt.translate_us = (uint16_t)perf_sat16(info->us_translate);
	pt.set_wait_us = (uint16_t)perf_sat16(info->us_set_wait);
	pt.fp_loads = (uint16_t)perf_sat16(info->fp_loads);
	pt.readbacks = (uint8_t)(info->readbacks > 255 ? 255 : info->readbacks);
	unsigned why = 0;
	uint32_t what = 0;
	if (r->unknown_cmds) {
		uint8_t op = (uint8_t)r->last_unknown;
		if (!(perf.seen_op[op >> 3] & (1u << (op & 7)))) {
			perf.seen_op[op >> 3] |= (uint8_t)(1u << (op & 7));
			pt.news |= PERF_TASK_NEW_UNKNOWN;
			why = 1;
			what = op;
			char ev[48];
			snprintf(ev, sizeof ev, "desconocido=0x%02X tarea=%u", op, pt.task);
			perf_put_text(&perf.pres, PERF_R_EVENT, pt.ms, ev);
		}
	}
	for (uint32_t k = 0; k < info->nsig; k++)
		if (sig_new(info->sig[k])) {
			char ev[48];
			snprintf(ev, sizeof ev, "aproximado=%08X tarea=%u", (unsigned)info->sig[k], pt.task);
			perf_put_text(&perf.pres, PERF_R_EVENT, pt.ms, ev);
			pt.news |= PERF_TASK_NEW_APPROX;
			if (!why) {
				why = 2;
				what = info->sig[k];
			}
		}
	if (!why && (r->flags & GFX_RES_CUT) && !perf.cut_seen) {
		perf.cut_seen = true;
		why = 3;
		what = r->flags;
	}
	if (why && perf.cfg.dumps)
		dump_task(&pt, why, what);
	perf.tasks++;
	if (perf.cfg.tasks && perf_room(&perf.ses))
		perf_put_task(&perf.pres, &pt);
}

/* ---- Sesión ------------------------------------------------------------------------- */

static void io_thread(void *arg)
{
	(void)arg;
	while (!perf.stop) {
		sysUsleep(PERF_DRAIN_US);
		perf_drain(&perf.ses);
	}
	sysThreadExit(0);
}

static void read_cfg(struct perf_cfg *c)
{
	perf_cfg_defaults(c);
	FILE *f = fopen(PERF_CFG_PATH, "r");
	if (!f)
		return;
	char text[2048];
	size_t n = fread(text, 1, sizeof text - 1, f);
	fclose(f);
	text[n] = '\0';
	perf_cfg_parse(c, text);
}

bool perf_app_open(const uint8_t *rom, struct salsa64_rec *rec, const volatile uint32_t *field)
{
	struct salsa64_rec_trace *trace = perf.trace;
	memset(&perf, 0, sizeof perf);
	perf.trace = trace;
	rec->trace = NULL;
	if (!perf_app_default())
		return false;  /* la app normal: ni lee perfilado.cfg */
	read_cfg(&perf.cfg);
	if (!perf.cfg.on)
		return false;
	if (!emu_mem)
		emu_mem = memalign(128, PERF_EMU_RING);
	if (!pres_mem)
		pres_mem = memalign(128, PERF_PRES_RING);
	if (!emu_mem || !pres_mem) {
		salsa64_log("perfilado: sin memoria para los anillos: apagado");
		return false;
	}
	perf_ring_init(&perf.emu, emu_mem, PERF_EMU_RING);
	perf_ring_init(&perf.pres, pres_mem, PERF_PRES_RING);
	perf.ses.rings[0] = &perf.emu;
	perf.ses.rings[1] = &perf.pres;
	salsa64_save_basename(rom, perf.game, sizeof perf.game);
	perf.t0 = sysGetSystemTime();
	perf.field = field;
	perf.rng = (uint32_t)perf.t0;
	uint64_t cap = (uint64_t)perf.cfg.cap_mb << 20;
	if (!perf_session_open(&perf.ses, PERF_ROOT, perf.game, cap, cap / 10 > PERF_RESERVE ? PERF_RESERVE : cap / 10)) {
		salsa64_log("perfilado: no se pudo crear %s/%s: apagado", PERF_ROOT, perf.game);
		return false;
	}
	perf.on = true;
	if (!perf.trace)
		perf.trace = memalign(128, sizeof *perf.trace);
	if (perf.trace) {
		perf.trace->seq = perf.cfg.trace;
		perf.trace->jr = perf.cfg.jr;
		perf.trace->win_len = perf.cfg.trace_len;
		perf.trace->on_block = on_block;
		perf.trace->on_evict = on_evict;
		perf.trace->ctx = NULL;
		salsa64_rec_trace_init(perf.trace);
		rec->trace = perf.trace;
	} else {
		salsa64_log("perfilado: sin memoria para el registro del recompilador (%u KB)",
			    (unsigned)(sizeof *perf.trace >> 10));
	}
	perf.trace_next_us = perf.t0 + 3000000u;  /* la primera ventana, a los 3 s */
	perf.stop = 0;
	sys_ppu_thread_t tid;
	perf.thread_ok = sysThreadCreate(&tid, io_thread, NULL, PERF_IO_PRIO, PERF_IO_STACK, THREAD_JOINABLE,
					 (char *)"salsa64_perfilado") == 0;
	perf.tid = tid;
	salsa64_rdp_set_perf_hook(task_hook);
	char names[2048];
	perf_names_text(names, sizeof names);
	perf_put_text(&perf.pres, PERF_R_NAMES, 0, names);
	unsigned sessions = 0;
	uint64_t bytes = perf_dir_bytes(PERF_ROOT, &sessions);
	salsa64_log("perfilado: sesión %s (%u sesiones guardadas, %.1f de %u MB; %u viejas borradas), escritura %s",
		    perf.ses.path, sessions, bytes / 1048576.0, perf.cfg.cap_mb, perf.ses.removed,
		    perf.thread_ok ? "en un hilo propio" : "al cerrar (sin hilo)");
	return true;
}

void perf_app_context(const char *text)
{
	if (perf.on)
		perf_put_text(&perf.pres, PERF_R_CONTEXT, perf_ms(), text);
}

void perf_app_close_rec(struct salsa64_rec *rec, struct salsa64_rec_sample *sample)
{
	if (!perf.on)
		return;
	/* El hilo que emula ya terminó: su anillo (el grande) queda libre para
	 * estas tablas, que pueden pasar de 256 KB cada una. */
	uint32_t ms = perf_ms();
	if (rec && rec->trace) {
		unsigned len = salsa64_rec_trace_take(rec, true);
		if (len && perf_room(&perf.ses))
			perf_put_trace(&perf.emu, ms, perf.trace_field0, rec->trace->win_cycles, rec->trace, len);
		perf_put_fpu(&perf.emu, ms, rec->trace);
		salsa64_rec_trace_fold_all(rec);
		if (rec->trace->jr)
			perf_put_jr(&perf.emu, ms, rec->trace);
	}
	if (sample && rec) {
		salsa64_rec_sample_fold_all(rec);
		perf_put_samples(&perf.emu, ms, sample);
	}
}

void perf_app_close(const char *end_text)
{
	salsa64_rdp_set_perf_hook(NULL);
	if (!perf.on)
		return;
	char text[1024];
	snprintf(text, sizeof text,
		 "%sperdidos_emu=%u\nperdidos_pres=%u\ntareas=%u\nvolcados=%u\nvolcados_fallidos=%u\ntrazas=%u\n", end_text,
		 perf.emu.lost, perf.pres.lost, perf.tasks, perf.dumps, perf.dump_fails, perf.traces);
	perf_put_text(&perf.pres, PERF_R_END, perf_ms(), text);
	if (perf.thread_ok) {
		u64 rv;
		perf.stop = 1;
		sysThreadJoin((sys_ppu_thread_t)perf.tid, &rv);
	}
	perf_session_close(&perf.ses);
	salsa64_log("cierre: perfilado: %s, %.1f MB, %u tareas, %u volcados, %u ventanas de la traza, %u bytes perdidos, "
		    "%u errores de escritura",
		    perf.ses.path, perf.ses.written / 1048576.0, perf.tasks, perf.dumps, perf.traces,
		    perf.emu.lost + perf.pres.lost, perf.ses.write_errors);
	perf.on = false;
}
