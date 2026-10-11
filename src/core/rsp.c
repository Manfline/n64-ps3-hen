/*
 * Salsa64 — tareas del RSP: lectura del OSTask, detección del microcódigo
 * gráfico y fin de la tarea (interrupciones SP y DP). Ver rcp.h.
 *
 * Tiempos: la tarea "dura" gfx_cycles/audio_cycles ciclos del CPU; en ese
 * lapso el backend gráfico puede trabajar en paralelo (un SPE). Al llegar
 * el fin, si el backend sabe decir si terminó (`done`) y todavía no, el
 * evento se vuelve a programar poll_cycles más tarde y el CPU sigue
 * emulando (o, si está en un bucle de espera, el backend duerme un poco:
 * así el reloj emulado no se adelanta al real); si no sabe, se le espera
 * (`finish`). La interrupción del DP llega dp_cycles después de la del
 * SP, si la lista terminó con G_RDPFULLSYNC (y el RDP no está congelado:
 * salsa64_rsp_dp_done). mupen64plus-core usa retardos
 * del mismo orden (unos miles de ciclos); aquí son más largos para que el
 * SPE alcance a terminar sin frenar al CPU, y se pueden cambiar.
 *
 * Mientras la tarea sigue en curso el RSP no está HALT y una escritura
 * que quite HALT no arranca otra (como en el hardware: ya corre); una
 * petición de yield se ignora y la tarea termina normal (TASKDONE), que
 * libultra acepta como fin de la tarea.
 */
#include <string.h>

#include "core/bus.h"
#include "core/endian.h"
#include "core/rom.h"
#include "core/sched.h"
#include "cpu/vr4300.h"

#define GFX_CYCLES_DEFAULT   30000u  /* ~0,32 ms de la N64 */
#define AUDIO_CYCLES_DEFAULT 8000u
#define DP_CYCLES_DEFAULT    10000u
#define POLL_CYCLES_DEFAULT  10000u  /* ~0,11 ms de la N64 entre sondeos */

/* Hasta dónde se buscan los textos del microcódigo en su segmento de datos. */
#define UCODE_SCAN_MAX 0x1000u

/* Lo que hace el BREAK final de un microcódigo; los de libultra ponen antes
 * la señal 2 (tarea terminada). */
static void rsp_break(struct salsa64_bus *bus, uint32_t signals)
{
	uint32_t s = __atomic_or_fetch(&bus->sp.regs[SALSA64_SP_STATUS],
				       SALSA64_SP_STATUS_HALT | SALSA64_SP_STATUS_BROKE | signals, __ATOMIC_SEQ_CST);
	if (s & SALSA64_SP_STATUS_INTR_BREAK)
		salsa64_mi_raise(bus, SALSA64_IRQ_SP);
}

/* ¿Terminó ya la tarea en curso? Los backends que lo saben decir sin
 * bloquear (`done`) se sondean; los demás se esperan en `finish`. */
static bool task_done(struct salsa64_bus *bus, bool idle)
{
	const struct salsa64_rsp *rsp = &bus->rsp;
	if (rsp->busy == SALSA64_TASK_GFX && rsp->gfx && rsp->gfx->done)
		return rsp->gfx->done(rsp->gfx->ctx, idle);
	if (rsp->busy == SALSA64_TASK_AUDIO && rsp->audio && rsp->audio->done)
		return rsp->audio->done(rsp->audio->ctx, idle);
	if (rsp->busy == SALSA64_TASK_LLE && rsp->lle && rsp->lle->done)
		return rsp->lle->done(rsp->lle->ctx, idle);
	return true;
}

static void dp_sync_at(struct salsa64_bus *bus, uint64_t now);

static void task_event(void *ctx, uint64_t when)
{
	struct salsa64_bus *bus = ctx;
	struct salsa64_rsp *rsp = &bus->rsp;
	uint32_t flags = 0;

	/* ¿El CPU saltó ciclos (bucle de espera) desde el sondeo anterior? */
	uint64_t idle_now = bus->cpu ? bus->cpu->idle_cycles : 0;
	bool idle = idle_now != rsp->poll_idle;
	rsp->poll_idle = idle_now;
	if (!task_done(bus, idle)) {
		rsp->stats.polls++;
		salsa64_sched_set(rsp->sched, SALSA64_EV_SP_TASK, when + rsp->poll_cycles);
		return;
	}
	if (rsp->busy == SALSA64_TASK_LLE) {
		/* El microcódigo real (en el SPE) ya dejó SP_STATUS como lo dejó:
		 * HALT y BROKE si hizo BREAK; finish recoge su estado. */
		if (rsp->lle)
			rsp->lle->finish(rsp->lle->ctx);
		rsp->busy = 0;
		return;
	}
	if (rsp->busy == SALSA64_TASK_GFX && rsp->gfx)
		flags = rsp->gfx->finish(rsp->gfx->ctx);
	else if (rsp->busy == SALSA64_TASK_AUDIO && rsp->audio)
		rsp->audio->finish(rsp->audio->ctx, bus);
	rsp->busy = 0;
	rsp_break(bus, SALSA64_SP_STATUS_TASKDONE);
	/* La interrupción del DP llega si la lista pidió G_RDPFULLSYNC y también
	 * si la HLE la cortó antes de llegar a él (búfer lleno, lista
	 * interminable, fin dentro de un tramo de otro microcódigo): el juego
	 * espera al RDP por cada tarea gráfica y, sin ella, se queda esperando
	 * (OoT avisa «RCP is HUNG UP» a los 3 s). Mismo principio que en
	 * salsa64_rsp_start para las tareas con un microcódigo sin HLE. */
	if (flags & SALSA64_GFX_CUT)
		rsp->stats.cut++;
	if (flags & (SALSA64_GFX_FULLSYNC | SALSA64_GFX_CUT))
		dp_sync_at(bus, when);
}

#ifdef SALSA64_DEBUG_DP
void salsa64_debug_dp(int kind, uint32_t reg, uint32_t v);
#endif

static void dp_event(void *ctx, uint64_t when)
{
	(void)when;
#ifdef SALSA64_DEBUG_DP
	salsa64_debug_dp(3, 0, 0);
#endif
	salsa64_mi_raise(ctx, SALSA64_IRQ_DP);
}

/* Congelado (FREEZE), el RDP no ejecuta: la interrupción espera a que el CPU
 * lo descongele. Banjo-Kazooie lo congela en el manejador de la interrupción
 * del DP, arranca la tarea del cuadro siguiente así y la descongela
 * después; si la interrupción de esa tarea llegara en el acto, el juego la
 * perdería y se quedaría esperando al RDP (2 cuadros y pantalla fija). */
static void dp_sync_at(struct salsa64_bus *bus, uint64_t now)
{
	struct salsa64_rsp *rsp = &bus->rsp;
	if (bus->dp[SALSA64_DP_STATUS] & SALSA64_DP_STATUS_FREEZE) {
		rsp->dp_held = true;
		return;
	}
	if (rsp->sched)
		salsa64_sched_set(rsp->sched, SALSA64_EV_DP_SYNC, now + rsp->dp_cycles);
}

void salsa64_rsp_dp_done(struct salsa64_bus *bus)
{
	dp_sync_at(bus, bus->cpu ? bus->cpu->cycles : 0);
}

void salsa64_rsp_dp_unfreeze(struct salsa64_bus *bus)
{
	struct salsa64_rsp *rsp = &bus->rsp;
	if (!rsp->dp_held)
		return;
	rsp->dp_held = false;
	salsa64_rsp_dp_done(bus);
}

void salsa64_rsp_sync(struct salsa64_bus *bus)
{
	const struct salsa64_lle_backend *lle = bus->rsp.lle;
	if (lle && lle->sync)
		lle->sync(lle->ctx);
}

void salsa64_rsp_set_lle(struct salsa64_bus *bus, const struct salsa64_lle_backend *lle,
			 enum salsa64_lle_mode mode)
{
	salsa64_rsp_reset(bus);
	bus->rsp.lle = lle;
	bus->rsp.lle_mode = lle ? mode : SALSA64_LLE_OFF;
}

void salsa64_rsp_set_dpc(struct salsa64_bus *bus, const struct salsa64_dpc_backend *dpc)
{
	bus->rsp.dpc = dpc;
}

void salsa64_rsp_init(struct salsa64_bus *bus, struct salsa64_sched *sched)
{
	struct salsa64_rsp *rsp = &bus->rsp;
	rsp->sched = sched;
	rsp->gfx_cycles = GFX_CYCLES_DEFAULT;
	rsp->audio_cycles = AUDIO_CYCLES_DEFAULT;
	rsp->dp_cycles = DP_CYCLES_DEFAULT;
	rsp->poll_cycles = POLL_CYCLES_DEFAULT;
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
	else if (rsp->busy == SALSA64_TASK_LLE && rsp->lle)
		rsp->lle->finish(rsp->lle->ctx);
	rsp->busy = 0;
	rsp->dp_held = false;
	if (rsp->lle && rsp->lle->reset)
		rsp->lle->reset(rsp->lle->ctx);
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

/* Microcódigos sin texto de identificación, por el CRC-32 de su código
 * (ucode, ucode_size bytes; solo el valor, ver docs/compatibilidad.md). */
static const struct {
	uint32_t crc, size;
	enum salsa64_gbi gbi;
} ucode_crcs[] = {
	{ 0xC543D0A8u, 0x1000, SALSA64_GBI_F3DPD },  /* Perfect Dark (NTSC-U) */
};

static enum salsa64_gbi gbi_by_crc(const struct salsa64_bus *bus, const struct salsa64_task *task)
{
	uint32_t addr = task->ucode & 0x1FFFFFFF;
	for (unsigned i = 0; i < sizeof ucode_crcs / sizeof ucode_crcs[0]; i++)
		if (task->ucode_size == ucode_crcs[i].size && in_rdram(bus, addr, task->ucode_size) &&
		    salsa64_crc32(bus->rdram + addr, task->ucode_size) == ucode_crcs[i].crc)
			return ucode_crcs[i].gbi;
	return SALSA64_GBI_UNKNOWN;
}

/* ¿El nombre del microcódigo ("RSP Gfx ucode F3DEX.NoN fifo 1.22"…) dice
 * que no recorta contra el plano cercano? */
static bool gfx_ucode_nonear(const uint8_t *q, const uint8_t *end)
{
	for (; q + 4 <= end && *q && *q != ' '; q++)
		if (starts_with(q, end, ".NoN"))
			return true;
	return false;
}

static enum salsa64_gbi detect_gbi(const struct salsa64_bus *bus, const struct salsa64_task *task, bool *nonear);

enum salsa64_gbi salsa64_rsp_detect_gbi(const struct salsa64_bus *bus, const struct salsa64_task *task)
{
	bool nonear;
	return detect_gbi(bus, task, &nonear);
}

static enum salsa64_gbi detect_gbi(const struct salsa64_bus *bus, const struct salsa64_task *task, bool *nonear)
{
	*nonear = false;
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
	if (find(p, end, "Salsa64 GBI F3DEXBG"))
		return SALSA64_GBI_F3DEXBG;
	if (find(p, end, "Salsa64 GBI F3DPD"))
		return SALSA64_GBI_F3DPD;
	if (find(p, end, "Salsa64 GBI F3DEX2"))
		return SALSA64_GBI_F3DEX2;
	if (find(p, end, "Salsa64 GBI F3DEX1"))
		return SALSA64_GBI_F3DEX;
	if (find(p, end, "Salsa64 GBI Fast3D"))
		return SALSA64_GBI_F3D;

	/* "RSP Gfx ucode <nombre> fifo|xbus <versión> ...": la versión 2.x es
	 * la segunda generación (F3DEX2 y familia). */
	const uint8_t *q = find(p, end, "RSP Gfx ucode ");
	if (q) {
		q += 14;
		*nonear = gfx_ucode_nonear(q, end);
		if (starts_with(q, end, "S2DEX"))
			return SALSA64_GBI_S2DEX;
		/* El de Conker ("F3DEXBG.NoN fifo 2.08"): F3DEX2 con lo suyo. */
		if (starts_with(q, end, "F3DEXBG"))
			return SALSA64_GBI_F3DEXBG;
		bool zex = starts_with(q, end, "F3DZEX");
		const uint8_t *v = q;
		while (v < end && *v && !(v[0] >= '0' && v[0] <= '9' && v + 1 < end && v[1] == '.'))
			v++;
		if (v < end && *v == '2')
			return zex ? SALSA64_GBI_F3DZEX2 : SALSA64_GBI_F3DEX2;
		/* 1.x y la 0.9x anterior (Mario Kart 64): primera generación. */
		if (v < end && (*v == '1' || *v == '0'))
			return SALSA64_GBI_F3DEX;
		return SALSA64_GBI_UNKNOWN;
	}
	/* Fast3D (Super Mario 64 y los primeros juegos) no trae ese texto. */
	if (find(p, end, "RSP SW Version: 2.0"))
		return SALSA64_GBI_F3D;
	return gbi_by_crc(bus, task);
}

void salsa64_rsp_ucode_text(const struct salsa64_bus *bus, uint32_t ucode_data, char *out, size_t n)
{
	uint32_t addr = ucode_data & 0x1FFFFFFF;
	if (n == 0)
		return;
	out[0] = '\0';
	if (addr >= bus->rdram_size)
		return;
	uint32_t size = bus->rdram_size - addr < UCODE_SCAN_MAX ? bus->rdram_size - addr : UCODE_SCAN_MAX;
	const uint8_t *p = bus->rdram + addr, *end = p + size;
	const uint8_t *q = find(p, end, "RSP ");
	if (!q)
		return;
	size_t i = 0;
	while (i + 1 < n && q + i < end && q[i] >= 0x20 && q[i] < 0x7F) {
		out[i] = (char)q[i];
		i++;
	}
	out[i] = '\0';
}

void salsa64_rsp_start(struct salsa64_bus *bus)
{
	struct salsa64_rsp *rsp = &bus->rsp;
	struct salsa64_task task;
	if (rsp->busy && rsp->busy != SALSA64_TASK_LLE) {
		/* Ya corre una tarea (su fin espera al backend): como en el
		 * hardware, quitar HALT a un RSP que corre no hace nada. */
		rsp->stats.starts_busy++;
		return;
	}
	if (rsp->busy == SALSA64_TASK_LLE && rsp->lle) {
		/* El CPU detuvo al microcódigo (HALT) y lo reanuda: el backend
		 * recoge lo que quedó en vuelo y sigue. */
		rsp->lle->finish(rsp->lle->ctx);
		rsp->busy = 0;
	}
	salsa64_rsp_read_task(bus, &task);

	uint64_t now = bus->cpu ? bus->cpu->cycles : 0;
	rsp->poll_idle = bus->cpu ? bus->cpu->idle_cycles : 0;
	bool valid = in_rdram(bus, task.data_ptr, 8) && in_rdram(bus, task.ucode_data, 16);

	bool nonear = false;
	enum salsa64_gbi gbi = task.type == SALSA64_TASK_GFX && valid ? detect_gbi(bus, &task, &nonear)
								       : SALSA64_GBI_UNKNOWN;
	enum salsa64_abi abi = task.type == SALSA64_TASK_AUDIO && valid ? salsa64_rsp_detect_abi(bus, &task)
								       : SALSA64_ABI_UNKNOWN;
	if (rsp->trace)
		rsp->trace(rsp->trace_ctx, &task, gbi);

	/* RSP a bajo nivel: lo que no tiene HLE (modo auto) o todo (all). Una
	 * tarea con HLE "disponible" es la que sus backends saben hacer. */
	if (rsp->lle && rsp->lle_mode != SALSA64_LLE_OFF && rsp->sched) {
		bool hle_gfx = task.type == SALSA64_TASK_GFX && valid && rsp->gfx &&
			       (gbi == SALSA64_GBI_F3DEX2 || gbi == SALSA64_GBI_F3DZEX2 || gbi == SALSA64_GBI_F3D ||
				gbi == SALSA64_GBI_F3DEX);
		bool hle_audio = task.type == SALSA64_TASK_AUDIO && valid && rsp->audio && abi != SALSA64_ABI_UNKNOWN;
		if (rsp->lle_mode == SALSA64_LLE_ALL || !(hle_gfx || hle_audio)) {
			rsp->stats.lle++;
			rsp->lle->start(rsp->lle->ctx);
			if (rsp->lle->done) {
				rsp->busy = SALSA64_TASK_LLE;
				salsa64_sched_set(rsp->sched, SALSA64_EV_SP_TASK, now + rsp->poll_cycles);
			}
			return;
		}
	}
	if (task.type == SALSA64_TASK_GFX && valid && rsp->sched) {
		if (gbi != SALSA64_GBI_UNKNOWN && gbi != SALSA64_GBI_S2DEX) {
			rsp->stats.gfx++;
			rsp->busy = SALSA64_TASK_GFX;
			rsp->gbi = gbi;
			rsp->gfx_nonear = nonear;
			if (rsp->gfx)
				rsp->gfx->start(rsp->gfx->ctx, &task, gbi);
			salsa64_sched_set(rsp->sched, SALSA64_EV_SP_TASK, now + rsp->gfx_cycles);
			return;
		}
		/* Microcódigo sin HLE: la tarea termina abajo y, como casi toda
		 * display list acaba en G_RDPFULLSYNC, también llega la
		 * interrupción del DP; si no, un juego que la espere se cuelga
		 * (GoldenEye antes de reconocer su Fast3D 2.0G). */
		rsp->stats.unknown_gbi++;
		rsp->stats.unknown_data = task.ucode_data;
		dp_sync_at(bus, now);
	} else if (task.type == SALSA64_TASK_AUDIO && valid && rsp->sched) {
		/* La HLE de audio (rsp_audio.c y src/rsp/audio; en la PS3, el SPE
		 * 1). Con un ABI desconocido la tarea solo termina. */
		rsp->stats.audio++;
		rsp->busy = SALSA64_TASK_AUDIO;
		rsp->abi = abi;
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
