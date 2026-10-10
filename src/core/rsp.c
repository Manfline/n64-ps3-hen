/*
 * Salsa64 — tareas del RSP: lectura del OSTask, detección del microcódigo
 * gráfico y fin de la tarea (interrupciones SP y DP). Ver rcp.h.
 *
 * Tiempos: la tarea "dura" gfx_cycles/audio_cycles ciclos del CPU; en ese
 * lapso el backend gráfico puede trabajar en paralelo (un SPE) y al final
 * se le espera. La interrupción del DP llega dp_cycles después de la del
 * SP, si la lista terminó con G_RDPFULLSYNC. mupen64plus-core usa retardos
 * del mismo orden (unos miles de ciclos); aquí son más largos para que el
 * SPE alcance a terminar sin frenar al CPU, y se pueden cambiar.
 */
#include <string.h>

#include "core/bus.h"
#include "core/endian.h"
#include "core/sched.h"
#include "cpu/vr4300.h"

#define GFX_CYCLES_DEFAULT   30000u  /* ~0,32 ms de la N64 */
#define AUDIO_CYCLES_DEFAULT 8000u
#define DP_CYCLES_DEFAULT    10000u

/* Hasta dónde se buscan los textos del microcódigo en su segmento de datos. */
#define UCODE_SCAN_MAX 0x1000u

/* Lo que hace el BREAK final de un microcódigo; los de libultra ponen antes
 * la señal 2 (tarea terminada). */
static void rsp_break(struct salsa64_bus *bus, uint32_t signals)
{
	uint32_t s = bus->sp.regs[SALSA64_SP_STATUS];
	s |= SALSA64_SP_STATUS_HALT | SALSA64_SP_STATUS_BROKE | signals;
	bus->sp.regs[SALSA64_SP_STATUS] = s;
	if (s & SALSA64_SP_STATUS_INTR_BREAK)
		salsa64_mi_raise(bus, SALSA64_IRQ_SP);
}

static void task_event(void *ctx, uint64_t when)
{
	struct salsa64_bus *bus = ctx;
	struct salsa64_rsp *rsp = &bus->rsp;
	uint32_t flags = 0;

	if (rsp->busy == SALSA64_TASK_GFX && rsp->gfx)
		flags = rsp->gfx->finish(rsp->gfx->ctx);
	else if (rsp->busy == SALSA64_TASK_AUDIO && rsp->audio)
		rsp->audio->finish(rsp->audio->ctx, bus);
	rsp->busy = 0;
	rsp_break(bus, SALSA64_SP_STATUS_TASKDONE);
	if (flags & SALSA64_GFX_FULLSYNC)
		salsa64_sched_set(rsp->sched, SALSA64_EV_DP_SYNC, when + rsp->dp_cycles);
}

static void dp_event(void *ctx, uint64_t when)
{
	(void)when;
	salsa64_mi_raise(ctx, SALSA64_IRQ_DP);
}

void salsa64_rsp_init(struct salsa64_bus *bus, struct salsa64_sched *sched)
{
	struct salsa64_rsp *rsp = &bus->rsp;
	rsp->sched = sched;
	rsp->gfx_cycles = GFX_CYCLES_DEFAULT;
	rsp->audio_cycles = AUDIO_CYCLES_DEFAULT;
	rsp->dp_cycles = DP_CYCLES_DEFAULT;
	salsa64_sched_handler(sched, SALSA64_EV_SP_TASK, task_event, bus);
	salsa64_sched_handler(sched, SALSA64_EV_DP_SYNC, dp_event, bus);
}

void salsa64_rsp_reset(struct salsa64_bus *bus)
{
	struct salsa64_rsp *rsp = &bus->rsp;
	/* Una tarea gráfica en vuelo se espera: el backend no se puede abandonar
	 * a medias. */
	if (rsp->busy == SALSA64_TASK_GFX && rsp->gfx)
		rsp->gfx->finish(rsp->gfx->ctx);
	else if (rsp->busy == SALSA64_TASK_AUDIO && rsp->audio)
		rsp->audio->finish(rsp->audio->ctx, bus);
	rsp->busy = 0;
	if (rsp->sched) {
		salsa64_sched_cancel(rsp->sched, SALSA64_EV_SP_TASK);
		salsa64_sched_cancel(rsp->sched, SALSA64_EV_DP_SYNC);
	}
}

void salsa64_rsp_set_gfx(struct salsa64_bus *bus, const struct salsa64_gfx_backend *gfx)
{
	salsa64_rsp_reset(bus);
	bus->rsp.gfx = gfx;
}

void salsa64_rsp_read_task(const struct salsa64_bus *bus, struct salsa64_task *task)
{
	uint32_t *w = &task->type;
	for (unsigned i = 0; i < sizeof *task / 4; i++)
		w[i] = salsa64_rd32(bus->sp_mem + SALSA64_TASK_DMEM + 4 * i);
}

static bool in_rdram(const struct salsa64_bus *bus, uint32_t addr, uint32_t size)
{
	addr &= 0x1FFFFFFF;
	return addr < bus->rdram_size && size <= bus->rdram_size - addr;
}

static bool starts_with(const uint8_t *p, const uint8_t *end, const char *s)
{
	size_t n = strlen(s);
	return (size_t)(end - p) >= n && memcmp(p, s, n) == 0;
}

static const uint8_t *find(const uint8_t *p, const uint8_t *end, const char *s)
{
	size_t n = strlen(s);
	for (; (size_t)(end - p) >= n; p++)
		if (memcmp(p, s, n) == 0)
			return p;
	return NULL;
}

enum salsa64_gbi salsa64_rsp_detect_gbi(const struct salsa64_bus *bus, const struct salsa64_task *task)
{
	uint32_t addr = task->ucode_data & 0x1FFFFFFF;
	uint32_t size = task->ucode_data_size;
	if (size == 0 || size > UCODE_SCAN_MAX)
		size = UCODE_SCAN_MAX;
	if (addr >= bus->rdram_size)
		return SALSA64_GBI_UNKNOWN;
	if (size > bus->rdram_size - addr)
		size = bus->rdram_size - addr;
	const uint8_t *p = bus->rdram + addr, *end = p + size;

	/* ROMs de prueba de Salsa64 (sin microcódigo de Nintendo). */
	if (find(p, end, "Salsa64 GBI F3DEX2"))
		return SALSA64_GBI_F3DEX2;

	/* "RSP Gfx ucode <nombre> fifo|xbus <versión> ...": la versión 2.x es
	 * la segunda generación (F3DEX2 y familia). */
	const uint8_t *q = find(p, end, "RSP Gfx ucode ");
	if (q) {
		q += 14;
		if (starts_with(q, end, "S2DEX"))
			return SALSA64_GBI_S2DEX;
		bool zex = starts_with(q, end, "F3DZEX");
		const uint8_t *v = q;
		while (v < end && *v && !(v[0] >= '0' && v[0] <= '9' && v + 1 < end && v[1] == '.'))
			v++;
		if (v < end && *v == '2')
			return zex ? SALSA64_GBI_F3DZEX2 : SALSA64_GBI_F3DEX2;
		if (v < end && *v == '1')
			return SALSA64_GBI_F3DEX;
		return SALSA64_GBI_UNKNOWN;
	}
	/* Fast3D (Super Mario 64 y los primeros juegos) no trae ese texto. */
	if (find(p, end, "RSP SW Version: 2.0"))
		return SALSA64_GBI_F3D;
	return SALSA64_GBI_UNKNOWN;
}

void salsa64_rsp_start(struct salsa64_bus *bus)
{
	struct salsa64_rsp *rsp = &bus->rsp;
	struct salsa64_task task;
	salsa64_rsp_read_task(bus, &task);

	uint64_t now = bus->cpu ? bus->cpu->cycles : 0;
	bool valid = in_rdram(bus, task.data_ptr, 8) && in_rdram(bus, task.ucode_data, 16);

	enum salsa64_gbi gbi = task.type == SALSA64_TASK_GFX && valid ? salsa64_rsp_detect_gbi(bus, &task)
								       : SALSA64_GBI_UNKNOWN;
	if (rsp->trace)
		rsp->trace(rsp->trace_ctx, &task, gbi);
	if (task.type == SALSA64_TASK_GFX && valid && rsp->sched) {
		if (gbi == SALSA64_GBI_F3DEX2 || gbi == SALSA64_GBI_F3DZEX2) {
			rsp->stats.gfx++;
			rsp->busy = SALSA64_TASK_GFX;
			rsp->gbi = gbi;
			if (rsp->gfx)
				rsp->gfx->start(rsp->gfx->ctx, &task, gbi);
			salsa64_sched_set(rsp->sched, SALSA64_EV_SP_TASK, now + rsp->gfx_cycles);
			return;
		}
		rsp->stats.unknown_gbi++;
	} else if (task.type == SALSA64_TASK_AUDIO && valid && rsp->sched) {
		/* La HLE de audio (rsp_audio.c y src/rsp/audio; en la PS3, el SPE
		 * 1). Con un ABI desconocido la tarea solo termina. */
		rsp->stats.audio++;
		rsp->busy = SALSA64_TASK_AUDIO;
		rsp->abi = salsa64_rsp_detect_abi(bus, &task);
		if (rsp->abi == SALSA64_ABI_UNKNOWN)
			rsp->stats.unknown_abi++;
		else if (rsp->audio)
			rsp->audio->start(rsp->audio->ctx, &task, rsp->abi);
		salsa64_sched_set(rsp->sched, SALSA64_EV_SP_TASK, now + rsp->audio_cycles);
		return;
	} else {
		rsp->stats.other++;
	}
	/* Sin HLE para esta tarea: se detiene en el acto. Si parece una tarea de
	 * libultra (como hace mupen64plus-rsp-hle: rspboot de hasta 4 KB), con
	 * TASKDONE: el manejador de interrupciones de libultra solo avisa que la
	 * tarea terminó si ve TASKDONE o YIELDED; si no, la toma por un BREAK
	 * suelto y quien la espera no despierta nunca (Zelda OoT al arrancar
	 * manda una tarea de tipo 4 así). */
	bool libultra = task.type != 0 && task.ucode_size != 0 && task.ucode_boot_size <= 0x1000;
	rsp_break(bus, libultra ? SALSA64_SP_STATUS_TASKDONE : 0);
}
