/*
 * Salsa64 — frontend mínimo para PS3.
 *
 * Carga una ROM, la arranca por HLE y la emula en dos hilos del PPE
 * (docs/hilos-ppe.md):
 *
 *   emulación     (emu_thread, prioridad 1000): corre el CPU (recompilador,
 *                 o el intérprete en RPCS3 o con el archivo SALSA64_NOREC_FLAG;
 *                 ver platform/ps3/rec_cache.h) hasta el fin de cada campo del
 *                 VI, aplica los controles al PIF, copia
 *                 la imagen del VI y encola el campo; las tareas gráficas del
 *                 RSP van por HLE al SPE 0 (src/platform/ps3/gfx_spu.c) y su
 *                 salida queda en la cola de src/rdp.
 *   presentación  (el hilo principal, prioridad 1001): traduce la salida de
 *                 la HLE a comandos del RSX (salsa64_rdp_flush) y presenta
 *                 cada campo con lib/rsxkit: si lo dibujó el RSX, ese destino
 *                 de render; si no, la copia del framebuffer de la RDRAM como
 *                 textura, escalada a 4:3. Lee los DualShock 3, vuelca el
 *                 guardado, escribe el log de estadísticas, capturas y
 *                 volcados, y decide la salida.
 *
 * Si la emulación va más rápida que la tele se presenta el campo más nuevo y
 * los demás se saltan: la emulación nunca espera al vsync. Lo que la frena
 * es el ritmo de la tele emulada (pacer.c: cada campo a su hora, sin
 * acumular deuda); con SALSA64_DATA_DIR/turbo presente corre al máximo
 * (para medir). La medición A/B de vitest corre siempre al máximo.
 *
 * ROM: si existe SALSA64_DATA_DIR/embedded con "vitest" o "gfxtest", esa
 * ROM de prueba embebida (o con "file:<nombre>", ese archivo de
 * SALSA64_DATA_DIR, para probar una ROM en RPCS3 sin pisar el rom.z64 que
 * leen otras compilaciones; las variantes de gfxtest, salsa64-gfxtest-*.z64,
 * se verifican como la embebida; o con "dump:<nombre>", la repetición de un
 * volcado de la HLE en el RSX: replay.c); si no, SALSA64_DATA_DIR/rom.z64
 * (cualquiera de los tres órdenes de bytes) si existe; si no, gfxtest. Con las ROMs de
 * prueba el frontend se verifica a sí mismo (selftest.c, selftest_gfx.c) e
 * imprime "RESULTADO frontend: OK/FALLO"; en modo de prueba automática
 * (platform/ps3/autotest.h) sale después.
 * Con vitest y fuera de la prueba automática, después mide 10 s sin
 * detección de bucles de espera y 10 s con ella ("MEDICIÓN …" en el log)
 * y sigue.
 *
 * Controles: los DualShock 3 de los puertos 1–4 son los controles de la N64
 * (padmap.c; se cambian con SALSA64_DATA_DIR/controles.cfg). El puerto 1
 * siempre tiene un control conectado (neutro sin mando): el DS3 tarda unos
 * cuadros en aparecer y los juegos buscan controles al arrancar. Los cambios
 * de botones van al log.
 *
 * Guardado del cartucho (EEPROM, SRAM; savefile.c): tipo por
 * SALSA64_DATA_DIR/guardado.cfg, la cabecera de homebrew o la tabla por
 * código de juego; un archivo por juego en SALSA64_DATA_DIR/saves, escrito
 * unos segundos después de la última escritura del juego y al salir.
 *
 * HLE gráfica: en el SPE 0 (lib/spukit); con SALSA64_DATA_DIR/gfx_ppe
 * presente, en el PPE (para comparar tiempos).
 *
 * Audio (audio_out.c): HLE de las tareas de audio en el SPE 1 y lo que sale
 * por el AI al puerto de audio, desde un hilo propio; se configura con
 * SALSA64_DATA_DIR/audio.cfg. El ritmo lo marca pacer.c (la tele
 * emulada); el audio lo sigue con su corrección fina (audio_sync.h). Con
 * "audiotest" en embedded, la ROM de prueba del audio se verifica sola
 * (selftest_audio.c) y sigue sonando.
 *
 * Capturas: con una ROM del disco y SALSA64_DATA_DIR/capturas presente,
 * cada SHOT_US se guarda la pantalla en SALSA64_DATA_DIR/screen-N.ppm (N de
 * 1 a SHOT_MAX, en ronda), para verla desde la Mac por FTP. Cada captura
 * frena la presentación ~400 ms (leer el búfer del RSX desde el PPE es
 * lento): se nota como un tirón, por eso es opcional. En modo de prueba
 * automática se captura siempre, cada AUTO_SHOT_FIELDS campos, y se sale
 * tras AUTO_FIELDS (para correr una ROM en RPCS3 sin manos). Si capturas
 * dice un número N, se captura en cambio cada N campos exactos
 * (campo-<campo>.ppm; esos campos no se saltan): la emulación es
 * determinista, así que se comparan entre compilaciones; un segundo número
 * M hace salir en el campo M (mediciones sin manos).
 *
 * Medición de la HLE gráfica: además de stats() cada STATS_US, el hilo de
 * emulación resume cada GFX_WIN_FIELDS campos ("gfx campos A–B": campos/s,
 * el campo que más tardó en emularse y el trabajo del SPE). Mismas
 * ventanas, mismas escenas en todas las corridas.
 *
 * Si SALSA64_DATA_DIR/gfxdump dice un número de campo,
 * en ese campo se vuelca la salida de la última tarea gráfica a
 * SALSA64_DATA_DIR/gfxdump.bin (salsa64-run --raster-dump la dibuja).
 *
 * Salir: START + SELECT, o "Salir del juego" desde el menú del sistema.
 * Lo que la ROM imprime por el ISViewer va al log.
 */
#include <io/pad.h>
#include <malloc.h>
#include <ppu-types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/sem.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sysutil/sysutil.h>

#include <rsxkit/rsxkit.h>
#include <spukit/spukit.h>

#include "core/boot.h"
#include "cpu/rec.h"
#include "audio_out.h"
#include "audiotest_rom_bin.h"
#include "gfxtest_rom_bin.h"
#include "pacer.h"
#include "padmap.h"
#include "platform/ps3/autotest.h"
#include "platform/ps3/gfx_spu.h"
#include "platform/ps3/rsp_spu.h"
#include "platform/ps3/log.h"
#include "platform/ps3/rec_cache.h"
#include "rdp/rdp.h"
#include "rsp/gfx/hle.h"
#include "replay.h"
#include "savefile.h"
#include "screenshot.h"
#include "selftest.h"
#include "selftest_audio.h"
#include "selftest_gfx.h"
#include "vitest_rom_bin.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define ROM_PATH     SALSA64_DATA_DIR "/rom.z64"
#define PADS_PATH    SALSA64_DATA_DIR "/controles.cfg"
#define SAVES_DIR    SALSA64_DATA_DIR "/saves"
#define SAVE_CFG     SALSA64_DATA_DIR "/guardado.cfg"
#define PADS_CFG_MAX 4096
#define EMBED_PATH   SALSA64_DATA_DIR "/embedded"
#define GFX_PPE_PATH SALSA64_DATA_DIR "/gfx_ppe"
#define TURBO_PATH   SALSA64_DATA_DIR "/turbo"
#define LLE_PATH     SALSA64_DATA_DIR "/lle"
#define SHOTS_PATH   SALSA64_DATA_DIR "/capturas"
#define ROM_MAX      (64u << 20)
#define FB_MAX_W     640
#define FB_MAX_H     576
#define STATS_US     5000000u
#define BENCH_US     10000000u
#define SHOT_US      10000000u
#define SHOT_MAX     9
#define AUTO_SHOT_FIELDS 300
#define AUTO_FIELDS      1800

/* Hilo de emulación: más prioridad que el principal (1001), para que el
 * sistema le quite ciclos antes a la presentación. */
#define EMU_PRIO     1000
#define EMU_STACK    (256u << 10)
#define WAKE_US      4000  /* el hilo de presentación revisa controles y colas al menos cada tanto */

static struct salsa64_n64 n64;
static struct salsa64_rec rec;
static bool use_rec;  /* el CPU corre con el recompilador */
static uint8_t isv_buf[SALSA64_ISV_SIZE] __attribute__((aligned(16)));
static struct salsa64_save save;
static struct savefile save_file;
static volatile int running = 1;
static volatile int emu_done;   /* el hilo de emulación salió de su bucle */
static int selftest_on;   /* vitest */
static int gfxtest_on;    /* gfxtest */
static int audiotest_on;  /* audiotest */
static struct salsa64_gfx_spu gfx_spu;
static struct salsa64_gfx_sync gfx_ppe;
static struct gfx_state gfx_ppe_state;
/* RSP a bajo nivel (Fase 6): con SALSA64_DATA_DIR/lle presente, el
 * microcódigo real corre en un SPE para lo que no tiene HLE ("auto") o
 * para todo ("all"); el RDP lo consume gfx_ppe en el PPE. */
static struct salsa64_rsp_spu rsp_spu;
static enum salsa64_lle_mode lle_mode;
static sys_sem_t wake_sem;  /* la emulación despierta a la presentación */
static struct pacer pacer;  /* ritmo de la tele emulada (hilo de emulación) */

static void pacer_sleep(void *ctx, uint64_t us)
{
	(void)ctx;
	sysUsleep((u32)us);
}

static inline void lwsync(void) { __asm__ volatile("lwsync" ::: "memory"); }

static void wake(void)
{
	sysSemPost(wake_sem, 1);  /* si ya está en 1, da error: no importa */
}

static void sysutil_cb(u64 status, u64 param, void *user)
{
	(void)param, (void)user;
	if (status == SYSUTIL_EXIT_GAME)
		running = 0;
}

static struct salsa64_padmap padmap;

/* ---- Lo que cruza entre los hilos (docs/hilos-ppe.md) ----------------------- */

/* Copia de la imagen del VI al terminar el campo: la escribe emulación, la
 * sube al RSX presentación. Tres ranuras bastan: una escribiéndose, una
 * en cola (la más nueva; las viejas se liberan) y una en uso. */
#define SNAP_SLOTS 3
#define SNAP_BYTES (FB_MAX_W * FB_MAX_H * 4)
enum { SNAP_FREE, SNAP_WRITING, SNAP_QUEUED, SNAP_IN_USE };
static uint8_t *snap_buf[SNAP_SLOTS];
static volatile uint32_t snap_state[SNAP_SLOTS];

static int snap_cas(int slot, uint32_t from, uint32_t to)
{
	return __sync_bool_compare_and_swap(&snap_state[slot], from, to);
}

/* Un campo terminado, de emulación a presentación. */
#define FIELD_QUEUE 8
#define FIELD_CHECK 0x1u  /* verificar la pantalla (selftest): no se salta */
#define FIELD_SHOT  0x2u  /* captura de un campo fijo (capturas = N): no se salta */
#define FIELD_KEEP  (FIELD_CHECK | FIELD_SHOT)
#define GFX_WIN_FIELDS 100
static uint32_t cap_every, exit_field;  /* capturas = "N M" */
struct field_msg {
	uint32_t field;             /* n64.bus.vi.fields al terminar */
	uint32_t tasks;             /* tareas gráficas terminadas hasta ahí */
	struct salsa64_vi_frame f;  /* f.width == 0: sin imagen */
	int32_t snap;               /* ranura con la copia del VI, o -1 */
	uint32_t flags;
	volatile uint32_t skip;     /* emulación lo pone si encoló uno más nuevo */
};
static struct field_msg field_q[FIELD_QUEUE];
static volatile uint32_t field_head, field_tail;  /* head la mueve emulación; tail, presentación */

/* Controles, de presentación a emulación (seqlock: seq impar = escribiendo). */
static struct {
	volatile uint32_t seq;
	struct salsa64_pad pads[SALSA64_PIF_PORTS];
} pad_box;

static void pads_publish(const struct salsa64_pad pads[SALSA64_PIF_PORTS])
{
	pad_box.seq++;
	lwsync();
	memcpy(pad_box.pads, pads, sizeof pad_box.pads);
	lwsync();
	pad_box.seq++;
}

static void pads_apply(void)
{
	struct salsa64_pad pads[SALSA64_PIF_PORTS];
	for (;;) {
		uint32_t s1 = pad_box.seq;
		if (s1 & 1)
			continue;  /* presentación está escribiendo: son unos nanosegundos */
		lwsync();
		memcpy(pads, pad_box.pads, sizeof pads);
		lwsync();
		if (pad_box.seq == s1)
			break;
	}
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++)
		salsa64_pif_set_pad(&n64.bus, i, &pads[i]);
}

/* Contadores del hilo de emulación; presentación los lee cada 5 s. */
static struct {
	volatile uint64_t us_emu, us_copy, us_pace;
	volatile uint32_t fields, dropped, no_slot;
} emu_stats;

/* Petición de presentación a emulación: empezar la medición A/B de vitest. */
static volatile int bench_request;

/* Sink gráfico del frontend: el del RDP más el timbre a presentación. */
static const struct salsa64_gfx_sink *rdp_sink;

static void front_acquire(void *ctx, struct gfx_job *job)
{
	(void)ctx;
	rdp_sink->acquire(rdp_sink->ctx, job);
}

static void front_done(void *ctx, const struct gfx_job *job, const struct gfx_result *res)
{
	(void)ctx;
	rdp_sink->done(rdp_sink->ctx, job, res);
	wake();
}

static const struct salsa64_gfx_sink front_sink = { NULL, front_acquire, front_done };

/* ---- Medición A/B con vitest (hilo de emulación) ----------------------------- */

static struct {
	int stage;  /* 0 nada, 1 sin detección, 2 con detección, 3 terminada */
	uint64_t t0, us_emu, cycles0, idle0;
	uint32_t fields;
	double rate[2], ms[2];
} bench;

static void bench_begin(int stage, uint64_t now)
{
	bench.stage = stage;
	bench.t0 = now;
	bench.us_emu = 0;
	bench.fields = 0;
	bench.cycles0 = n64.cpu.cycles;
	bench.idle0 = n64.cpu.idle_cycles;
	salsa64_cpu_set_idle(&n64.cpu, stage == 2);
}

static void bench_field(uint64_t now, uint64_t us_emu)
{
	if (bench.stage != 1 && bench.stage != 2)
		return;
	bench.us_emu += us_emu;
	bench.fields++;
	uint64_t dt = now - bench.t0;
	if (dt < BENCH_US)
		return;
	int k = bench.stage - 1;
	uint64_t cyc = n64.cpu.cycles - bench.cycles0, idle = n64.cpu.idle_cycles - bench.idle0;
	bench.rate[k] = bench.fields * 1e6 / dt;
	bench.ms[k] = bench.us_emu / 1000.0 / bench.fields;
	salsa64_log("MEDICIÓN vitest %s detección de bucles de espera: %.2f campos/s, emulación %.2f ms "
		    "por campo, %.1f MIPS, %.1f %% del tiempo saltado (%u campos en %.1f s)",
		    k ? "con" : "sin", bench.rate[k], bench.ms[k], (double)(cyc - idle) / dt,
		    cyc ? 100.0 * idle / cyc : 0.0, bench.fields, dt / 1e6);
	if (k == 0) {
		bench_begin(2, now);
	} else {
		bench.stage = 3;
		salsa64_log("MEDICIÓN vitest: emulación %.2f → %.2f ms por campo (×%.1f), %.2f → %.2f campos/s",
			    bench.ms[0], bench.ms[1], bench.ms[1] > 0 ? bench.ms[0] / bench.ms[1] : 0.0,
			    bench.rate[0], bench.rate[1]);
	}
}

/* ---- Controles (hilo de presentación) --------------------------------------- */

/* Lee controles.cfg si existe; si no, el mapeo por omisión. */
static void load_padmap(void)
{
	static char text[PADS_CFG_MAX];
	salsa64_padmap_default(&padmap);
	FILE *f = fopen(PADS_PATH, "rb");
	if (!f) {
		salsa64_log("controles: mapeo por omisión (no hay %s)", PADS_PATH);
		return;
	}
	size_t len = fread(text, 1, sizeof text, f);
	fclose(f);
	char err[96];
	int bad = salsa64_padmap_parse(&padmap, text, len, err, sizeof err);
	if (bad)
		salsa64_log("controles: %s con %d líneas que no entendí; la primera, %s", PADS_PATH, bad, err);
	else
		salsa64_log("controles: %s", PADS_PATH);
}

static struct salsa64_ds3 ds3_from(const padData *d)
{
	struct salsa64_ds3 s;
	s.buttons = (uint32_t)d->BTN_CROSS << SALSA64_DS3_CROSS | (uint32_t)d->BTN_CIRCLE << SALSA64_DS3_CIRCLE |
		    (uint32_t)d->BTN_SQUARE << SALSA64_DS3_SQUARE | (uint32_t)d->BTN_TRIANGLE << SALSA64_DS3_TRIANGLE |
		    (uint32_t)d->BTN_L1 << SALSA64_DS3_L1 | (uint32_t)d->BTN_R1 << SALSA64_DS3_R1 |
		    (uint32_t)d->BTN_L2 << SALSA64_DS3_L2 | (uint32_t)d->BTN_R2 << SALSA64_DS3_R2 |
		    (uint32_t)d->BTN_L3 << SALSA64_DS3_L3 | (uint32_t)d->BTN_R3 << SALSA64_DS3_R3 |
		    (uint32_t)d->BTN_START << SALSA64_DS3_START | (uint32_t)d->BTN_SELECT << SALSA64_DS3_SELECT |
		    (uint32_t)d->BTN_UP << SALSA64_DS3_UP | (uint32_t)d->BTN_DOWN << SALSA64_DS3_DOWN |
		    (uint32_t)d->BTN_LEFT << SALSA64_DS3_LEFT | (uint32_t)d->BTN_RIGHT << SALSA64_DS3_RIGHT;
	s.lx = (uint8_t)d->ANA_L_H;
	s.ly = (uint8_t)d->ANA_L_V;
	s.rx = (uint8_t)d->ANA_R_H;
	s.ry = (uint8_t)d->ANA_R_V;
	return s;
}

/* DualShock 3 → controles de la N64, publicados para el hilo de emulación.
 * ioPadGetData da len = 0 cuando nada cambió: se conserva el último
 * estado. Devuelve 1 si algún mando tiene START + SELECT. */
static int update_pads(void)
{
	static struct salsa64_ds3 last[SALSA64_PIF_PORTS] = {
		[0 ... SALSA64_PIF_PORTS - 1] = { 0, 128, 128, 128, 128 },
	};
	static struct salsa64_pad logged[SALSA64_PIF_PORTS];
	static struct salsa64_ds3 raw_logged[SALSA64_PIF_PORTS] = {
		[0 ... SALSA64_PIF_PORTS - 1] = { 0, 128, 128, 128, 128 },
	};
	static uint8_t present[SALSA64_PIF_PORTS];
	const uint32_t quit = 1u << SALSA64_DS3_START | 1u << SALSA64_DS3_SELECT;
	struct salsa64_pad pads[SALSA64_PIF_PORTS];
	padInfo info;
	padData data;
	int exit = 0;

	if (ioPadGetInfo(&info) != 0)
		return 0;
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++) {
		struct salsa64_pad p = { 0, 0, 0, i == 0 };
		if (present[i] != !!info.status[i]) {
			present[i] = !!info.status[i];
			salsa64_log("mando %u %s", i + 1, present[i] ? "conectado" : "desconectado");
			last[i] = (struct salsa64_ds3){ 0, 128, 128, 128, 128 };  /* nada pegado */
		}
		if (info.status[i]) {
			if (ioPadGetData(i, &data) == 0 && data.len > 0)
				last[i] = ds3_from(&data);
			/* Los sticks del DS3 tal como llegan, para revisar el mapeo. */
			const struct salsa64_ds3 *a = &last[i], *b = &raw_logged[i];
			if (abs(a->lx - b->lx) >= 40 || abs(a->ly - b->ly) >= 40 ||
			    abs(a->rx - b->rx) >= 40 || abs(a->ry - b->ry) >= 40) {
				salsa64_log("mando %u: stick izquierdo (%u,%u), derecho (%u,%u)", i + 1,
					    a->lx, a->ly, a->rx, a->ry);
				raw_logged[i] = *a;
			}
			salsa64_padmap_apply(&padmap, &last[i], &p);
			if ((last[i].buttons & quit) == quit)
				exit = 1;
		}
		pads[i] = p;

		/* Al log: conexión y botones; el stick, si se movió bastante. */
		struct salsa64_pad *o = &logged[i];
		int dx = p.x - o->x, dy = p.y - o->y;
		if (p.connected != o->connected || p.buttons != o->buttons ||
		    dx * dx + dy * dy >= 40 * 40) {
			char desc[128];
			if (p.connected) {
				salsa64_pad_describe(&p, desc, sizeof desc);
				salsa64_log("control %u: %s", i + 1, desc);
			} else {
				salsa64_log("control %u: desconectado", i + 1);
			}
			*o = p;
		}
	}
	pads_publish(pads);
	return exit;
}

/* ---- ROM ------------------------------------------------------------------ */

/* Texto del ISViewer: al log línea por línea (hilo de emulación). */
static void isv_out(void *ctx, const uint8_t *text, uint32_t len)
{
	static char line[256];
	static uint32_t n;
	(void)ctx;
	if (selftest_on)
		selftest_isv(text, len);
	if (gfxtest_on)
		gfxtest_isv(text, len);
	if (audiotest_on)
		audiotest_isv(text, len);
	for (uint32_t i = 0; i < len; i++) {
		if (text[i] == '\n' || n == sizeof line - 1) {
			line[n] = '\0';
			salsa64_log("rom: %s", line);
			n = 0;
		}
		if (text[i] != '\n')
			line[n++] = (char)text[i];
	}
}

static uint8_t *embedded(const uint8_t *bin, uint32_t bin_size, uint32_t *size)
{
	uint8_t *buf = memalign(128, bin_size);
	if (buf) {
		memcpy(buf, bin, bin_size);
		*size = bin_size;
	}
	return buf;
}

/* ROM embebida elegida en EMBED_PATH, o NULL. */
static uint8_t *load_embedded(uint32_t *size, const char **source)
{
	char name[80] = "";
	FILE *f = fopen(EMBED_PATH, "r");
	if (!f)
		return NULL;
	if (!fgets(name, sizeof name, f))
		name[0] = '\0';
	fclose(f);
	name[strcspn(name, " \r\n")] = '\0';
	if (!strcmp(name, "vitest")) {
		*source = "vitest (embebida)";
		selftest_on = 1;
		return embedded(vitest_rom_bin, vitest_rom_bin_size, size);
	}
	if (!strcmp(name, "gfxtest")) {
		*source = "gfxtest (embebida)";
		gfxtest_on = 1;
		return embedded(gfxtest_rom_bin, gfxtest_rom_bin_size, size);
	}
	if (!strcmp(name, "audiotest")) {
		*source = "audiotest (embebida)";
		audiotest_on = 1;
		return embedded(audiotest_rom_bin, audiotest_rom_bin_size, size);
	}
	if (!strncmp(name, "file:", 5)) {
		static char path[96];
		snprintf(path, sizeof path, SALSA64_DATA_DIR "/%s", name + 5);
		FILE *rf = fopen(path, "rb");
		long len = -1;
		if (rf && fseek(rf, 0, SEEK_END) == 0)
			len = ftell(rf);
		uint8_t *buf = NULL;
		if (rf && len >= 0x1000 && (uint32_t)len <= ROM_MAX && fseek(rf, 0, SEEK_SET) == 0 &&
		    (buf = memalign(128, (size_t)len)) && fread(buf, 1, (size_t)len, rf) == (size_t)len) {
			fclose(rf);
			*size = (uint32_t)len;
			*source = path;
			if (!strncmp(name + 5, "salsa64-gfxtest", 15))
				gfxtest_on = 1;
			return buf;
		}
		free(buf);
		if (rf)
			fclose(rf);
		salsa64_log("no se pudo leer %s", path);
		return NULL;
	}
	salsa64_log("%s dice \"%s\": no es vitest, gfxtest, audiotest ni file:<nombre>", EMBED_PATH, name);
	return NULL;
}

/* ROM del disco (preasignada una sola vez) o la embebida. */
static uint8_t *load_rom(uint32_t *size, const char **source)
{
	uint8_t *rom = load_embedded(size, source);
	if (rom)
		return rom;
	FILE *f = fopen(ROM_PATH, "rb");
	if (f) {
		long len = -1;
		if (fseek(f, 0, SEEK_END) == 0)
			len = ftell(f);
		if (len >= 0x1000 && (uint32_t)len <= ROM_MAX && fseek(f, 0, SEEK_SET) == 0) {
			uint8_t *buf = memalign(128, (size_t)len);
			if (buf && fread(buf, 1, (size_t)len, f) == (size_t)len) {
				fclose(f);
				*size = (uint32_t)len;
				*source = ROM_PATH;
				return buf;
			}
			free(buf);
		}
		fclose(f);
		salsa64_log("no se pudo leer %s: uso la ROM de prueba", ROM_PATH);
	}
	*source = "gfxtest (embebida)";
	gfxtest_on = 1;
	return embedded(gfxtest_rom_bin, gfxtest_rom_bin_size, size);
}

/* SALSA64_DATA_DIR/lle: ausente = sin LLE; "all" = todo el microcódigo;
 * cualquier otra cosa = solo lo que no tiene HLE. */
static enum salsa64_lle_mode lle_config(void)
{
	FILE *f = fopen(LLE_PATH, "r");
	if (!f)
		return SALSA64_LLE_OFF;
	char word[16] = "";
	if (fscanf(f, "%15s", word) != 1)
		word[0] = '\0';
	fclose(f);
	return strcmp(word, "all") == 0 ? SALSA64_LLE_ALL : SALSA64_LLE_AUTO;
}

static int file_exists(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f)
		fclose(f);
	return f != NULL;
}

/* ---- Hilo de emulación ------------------------------------------------------ */

/* Al terminar un campo: la imagen del VI (copiada a una ranura) y el
 * mensaje para presentación. Si hay un campo anterior sin consumir y no
 * lleva verificación, se marca para saltarlo y se le suelta la ranura. */
static void post_field(void)
{
	static uint32_t last_idx = ~0u;
	struct field_msg m;
	memset(&m, 0, sizeof m);
	m.field = n64.bus.vi.fields;
	m.tasks = salsa64_rdp_tasks_done();
	m.snap = -1;

	if (salsa64_vi_frame(&n64.bus, &m.f) && m.f.width <= FB_MAX_W && m.f.height <= FB_MAX_H) {
		if (selftest_on && selftest_frame(&n64.bus, &m.f))
			m.flags |= FIELD_CHECK;
		if (cap_every && m.field % cap_every == 0)
			m.flags |= FIELD_SHOT;
		for (int i = 0; i < SNAP_SLOTS; i++) {
			if (snap_cas(i, SNAP_FREE, SNAP_WRITING)) {
				m.snap = i;
				break;
			}
		}
		if (m.snap >= 0) {
			uint64_t t0 = sysGetSystemTime();
			uint8_t *dst = snap_buf[m.snap];
			const uint8_t *src = n64.bus.rdram + m.f.origin;
			uint32_t line = m.f.width * m.f.bpp;
			if (m.f.stride == line) {
				memcpy(dst, src, line * m.f.height);
			} else {
				for (uint32_t y = 0; y < m.f.height; y++)
					memcpy(dst + y * line, src + y * m.f.stride, line);
			}
			emu_stats.us_copy += sysGetSystemTime() - t0;
		} else {
			emu_stats.no_slot++;
		}
	} else {
		m.f.width = 0;
	}

	uint32_t h = field_head;
	/* Con verificación pendiente esperamos lugar (solo en las pruebas);
	 * si no, un campo que no cabe se pierde: la emulación no espera. */
	while (h - field_tail >= FIELD_QUEUE && (m.flags & FIELD_KEEP) && running)
		sysUsleep(200);
	if (h - field_tail >= FIELD_QUEUE) {
		if (m.snap >= 0)
			snap_state[m.snap] = SNAP_FREE;
		emu_stats.dropped++;
		return;
	}
	if (last_idx != ~0u && (int32_t)(last_idx - field_tail) >= 0) {
		struct field_msg *o = &field_q[last_idx % FIELD_QUEUE];
		if (!(o->flags & FIELD_KEEP)) {
			o->skip = 1;
			if (o->snap >= 0)
				snap_cas(o->snap, SNAP_QUEUED, SNAP_FREE);  /* si presentación no la tomó ya */
		}
	}
	if (m.snap >= 0)
		snap_state[m.snap] = SNAP_QUEUED;
	field_q[h % FIELD_QUEUE] = m;
	lwsync();  /* el mensaje y la copia, antes de publicarlos */
	field_head = h + 1;
	last_idx = h;
	wake();
}

/* Estadística de la HLE gráfica en el SPE por ventanas fijas de campos
 * (hilo de emulación, donde se suman los contadores del SPE). */
static struct {
	int on;
	uint32_t field0;
	uint64_t t0, worst;  /* inicio y el campo que más tardó en emularse (µs) */
	struct salsa64_gfx_spu spu;
	struct salsa64_rdp_stats rdp;
} gwin;

static void gfx_window(uint64_t now, uint64_t us_field)
{
	uint32_t field = n64.bus.vi.fields;
	if (us_field > gwin.worst)
		gwin.worst = us_field;
	if (!gwin.t0)
		gwin.t0 = now;
	if (field - gwin.field0 < GFX_WIN_FIELDS)
		return;
	struct salsa64_rdp_stats rs;
	salsa64_rdp_get_stats(&rs);
	const struct salsa64_gfx_spu *s = &gfx_spu, *s0 = &gwin.spu;
	uint32_t tasks = s->tasks - s0->tasks;
	if (tasks && now > gwin.t0) {
		double k = 1e3 / spukit_tick_hz() / tasks;
		salsa64_log("gfx campos %u–%u: %.1f campos/s (peor campo %.1f ms de emulación), %u tareas; por tarea: "
			    "%.0f dibujos, %.0f triángulos, %.1f texturas decodificadas (+%.1f reusadas, %.1f de tareas "
			    "anteriores; %.1f al arena, %.1f desalojadas), %.1f cargas a la TMEM (%.1f lecturas); SPE %.2f ms "
			    "(vértices %.2f, estado %.2f, identificar texturas %.2f, decodificar %.2f, cargas %.2f), PPE "
			    "esperando %.2f ms, traducción al RSX %.2f ms",
			    gwin.field0, field - 1, (field - gwin.field0) * 1e6 / (now - gwin.t0), gwin.worst / 1e3, tasks,
			    (double)(rs.draws - gwin.rdp.draws) / tasks, (double)(rs.triangles - gwin.rdp.triangles) / tasks,
			    (double)(s->textures - s0->textures) / tasks, (double)(s->tex_reused - s0->tex_reused) / tasks,
			    (double)(s->tex_old - s0->tex_old) / tasks, (double)(s->tex_arena - s0->tex_arena) / tasks,
			    (double)(s->tex_evicted - s0->tex_evicted) / tasks,
			    (double)(s->loads - s0->loads) / tasks, (double)(s->load_reads - s0->load_reads) / tasks,
			    (s->spu_ticks - s0->spu_ticks) * k, (s->t_vtx - s0->t_vtx) * k, (s->t_state - s0->t_state) * k,
			    (s->t_hash - s0->t_hash) * k, (s->t_tex - s0->t_tex) * k, (s->t_load - s0->t_load) * k,
			    (s->wait_us - s0->wait_us) / 1e3 / tasks, (rs.us_translate - gwin.rdp.us_translate) / 1e3 / tasks);
	}
	gwin.field0 = field;
	gwin.t0 = now;
	gwin.worst = 0;
	gwin.spu = *s;
	gwin.rdp = rs;
}

/* Tareas gráficas con un microcódigo sin HLE (terminan sin dibujar): al log
 * la primera y luego cada vez que se duplican, para no llenarlo. */
static void log_unknown_gbi(void)
{
	static uint32_t next = 1;
	const struct salsa64_rsp_stats *st = &n64.bus.rsp.stats;
	if (st->unknown_gbi < next)
		return;
	char text[96];
	salsa64_rsp_ucode_text(&n64.bus, st->unknown_data, text, sizeof text);
	salsa64_log("RSP: %u tareas gráficas con microcódigo sin HLE (datos en 0x%08x: \"%s\"); terminan sin dibujar",
		    st->unknown_gbi, (unsigned)st->unknown_data, text);
	next = st->unknown_gbi * 2;
}

static void emu_thread(void *arg)
{
	(void)arg;
	while (running) {
		pads_apply();
		uint64_t t0 = sysGetSystemTime();
		/* Al ritmo de la tele emulada; la medición A/B va al máximo y
		 * queda fuera del ritmo (ni espera ni avanza la referencia). */
		int paced = bench.stage != 1 && bench.stage != 2;
		if (paced) {
			pacer_wait(&pacer, t0);
			uint64_t tp = sysGetSystemTime();
			emu_stats.us_pace += tp - t0;
			t0 = tp;
		}
		uint64_t cycles = salsa64_n64_run_field(&n64);
		uint64_t t1 = sysGetSystemTime();
		log_unknown_gbi();
		if (paced)
			pacer_advance(&pacer, cycles);
		savefile_stage(&save_file, t1);
		post_field();
		/* audio_out: si el audio necesita este hilo (fin de cada tarea
		 * de audio), aquí. */
		if (bench_request && !bench.stage)
			bench_begin(1, t1);
		else
			bench_field(t1, t1 - t0);
		emu_stats.us_emu += t1 - t0;
		if (gwin.on)
			gfx_window(t1, t1 - t0);
		lwsync();
		emu_stats.fields++;
	}
	lwsync();
	emu_done = 1;
	wake();
	sysThreadExit(0);
}

/* ---- Hilo de presentación --------------------------------------------------- */

static struct {
	rsxkit_view view;
	rsxkit_fb *fb;
	int autotest;
	int shots_on;  /* capturas periódicas (SHOTS_PATH o prueba automática) */
	enum selftest_state result;
	uint32_t dump_field;
	uint64_t t_shot;
	uint32_t shots;
	/* Estadísticas de la ventana en curso. */
	uint64_t us_present, us_flip;
	uint32_t presented, blank, rsx_frames, skipped;
} pres = { .view = { RSXKIT_SCALE_FIT, RSXKIT_FILTER_LINEAR, 4.0f / 3.0f } };

/* Presenta (o salta) un campo encolado. */
static void present_field(const struct field_msg *m, uint64_t now)
{
	salsa64_rdp_flush(m->tasks);  /* las tareas de este campo, no las del siguiente */
	if (m->skip && !(m->flags & FIELD_KEEP)) {
		if (m->snap >= 0)
			snap_cas(m->snap, SNAP_QUEUED, SNAP_FREE);
		pres.skipped++;
		return;
	}
	int slot = m->snap;
	bool have = false;
	if (slot >= 0) {
		have = snap_cas(slot, SNAP_QUEUED, SNAP_IN_USE);
		if (!have) {
			/* Emulación la soltó al encolar un campo más nuevo. */
			pres.skipped++;
			return;
		}
	}
	rsxkit_rect r = { 0, 0, 0, 0 };
	bool check = false, check_gfx = false;
	uint64_t t0 = sysGetSystemTime();
	rsxkit_frame_begin(0x000000);
	if (m->f.width) {
		if (salsa64_rdp_present(&m->f, m->field, &pres.view, &r)) {
			pres.rsx_frames++;
			if (gfxtest_on && pres.result == SELFTEST_RUNNING)
				check_gfx = gfxtest_want_screen();
		} else if (have) {
			check = (m->flags & FIELD_CHECK) != 0;
			rsxkit_fb_upload(pres.fb, snap_buf[slot], m->f.width, m->f.height, m->f.width * m->f.bpp,
					 m->f.bpp == 4 ? RSXKIT_RGBA8888 : RSXKIT_RGBA5551);
			rsxkit_fb_draw(pres.fb, &pres.view, &r);
		} else {
			pres.blank++;
		}
	} else {
		pres.blank++;
	}
	if (have)
		snap_state[slot] = SNAP_FREE;  /* ya está en la memoria del RSX */
	if (check || check_gfx) {
		rsxkit_finish();
		if (check)
			selftest_screen(&m->f, &r);
		else
			gfxtest_screen(&m->f, &r);
	} else if ((m->flags & FIELD_SHOT) && m->f.width) {
		char path[64];
		snprintf(path, sizeof path, SALSA64_DATA_DIR "/campo-%u.ppm", m->field);
		rsxkit_finish();
		salsa64_screenshot(path, &m->f, &r);
		salsa64_log("captura %s", path);
	} else if (pres.shots_on && !cap_every && !selftest_on && !gfxtest_on && !audiotest_on && m->f.width &&
		   (pres.autotest ? m->field / AUTO_SHOT_FIELDS != pres.shots : now - pres.t_shot >= SHOT_US)) {
		char path[64];
		snprintf(path, sizeof path, SALSA64_DATA_DIR "/screen-%u.ppm", pres.shots % SHOT_MAX + 1);
		rsxkit_finish();
		salsa64_screenshot(path, &m->f, &r);
		salsa64_log("captura %s (campo %u)", path, m->field);
		pres.shots = pres.autotest ? m->field / AUTO_SHOT_FIELDS : pres.shots + 1;
		pres.t_shot = now;
	}
	if (pres.dump_field && m->field >= pres.dump_field) {
		rsxkit_finish();
		salsa64_log("volcado de la HLE en el campo %u: %s", m->field,
			    salsa64_rdp_dump(SALSA64_DATA_DIR "/gfxdump.bin") ? "bien" : "FALLO");
		pres.dump_field = 0;
	}
	/* audio_out: una vez por campo presentado (estadísticas y volcado). */
	uint64_t t1 = sysGetSystemTime();
	audio_out_field(t1);
	rsxkit_frame_end();
	uint64_t t2 = sysGetSystemTime();
	pres.us_present += t1 - t0;
	pres.us_flip += t2 - t1;
	pres.presented++;

	if (exit_field && m->field >= exit_field) {
		salsa64_log("salida en el campo %u (capturas)", m->field);
		running = 0;
	}
	if (pres.autotest && !selftest_on && !gfxtest_on && !audiotest_on && m->field >= AUTO_FIELDS) {
		salsa64_log("RESULTADO frontend: OK (ROM del disco, %u campos)", m->field);
		running = 0;
	}
	if ((selftest_on || gfxtest_on || audiotest_on) && pres.result == SELFTEST_RUNNING) {
		pres.result = selftest_on ? selftest_state(m->field)
			    : gfxtest_on  ? gfxtest_state(m->field)
					  : audiotest_state(m->field);
		if (pres.result != SELFTEST_RUNNING) {
			salsa64_log("RESULTADO frontend: %s (campo %u)", pres.result == SELFTEST_OK ? "OK" : "FALLO",
				    m->field);
			if (pres.autotest)
				running = 0;
			else if (pres.result == SELFTEST_OK && selftest_on)
				bench_request = 1;
		}
	}
}

/* Consume lo encolado: campos en orden y, al final, las tareas gráficas que
 * ya terminaron (son del campo siguiente). */
static void drain(uint64_t now, int present)
{
	while (field_tail != field_head) {
		lwsync();  /* el mensaje, después de ver head */
		struct field_msg m = field_q[field_tail % FIELD_QUEUE];
		if (present) {
			present_field(&m, now);
		} else if (m.snap >= 0) {
			snap_cas(m.snap, SNAP_QUEUED, SNAP_FREE);
		}
		lwsync();
		field_tail++;
	}
	salsa64_rdp_flush(~0u);
}

/* Estadísticas de los dos hilos, cada STATS_US. */
static void stats(uint64_t now, int gfx_on_spu, const struct salsa64_boot_info *info)
{
	static uint64_t t_stats, us_emu0, us_copy0, us_pace0, cyc0, idle0;
	static uint32_t fields0;
	static struct salsa64_rdp_stats rdp0;
	static uint32_t gfx_waits0, gfx_polls0, gfx_cut0;
	static uint64_t gfx_skipped0;
	static uint64_t gfx_ticks0, gfx_wait0, gfx_sleep0, gfx_vtx0, gfx_state0, gfx_tex0, gfx_load0;

	if (!t_stats) {
		t_stats = now;
		salsa64_rdp_get_stats(&rdp0);
		return;
	}
	uint64_t dt = now - t_stats;
	if (dt < STATS_US)
		return;
	/* Los contadores de emulación se leen sin lock: palabras de 64 bits
	 * alineadas, una línea de estadísticas. */
	uint32_t fields = emu_stats.fields - fields0;
	uint64_t us_emu = emu_stats.us_emu - us_emu0, us_copy = emu_stats.us_copy - us_copy0;
	uint64_t us_pace = emu_stats.us_pace - us_pace0;
	uint64_t cyc = n64.cpu.cycles - cyc0, idle = n64.cpu.idle_cycles - idle0;
	salsa64_log("%.1f campos/s (%.0f %% de %s), %.1f MIPS, %.0f %% en bucles de espera; "
		    "emulación %.1f ms por campo (copia del VI %.2f), ritmo %.1f ms (%u reinicios); presentados %u "
		    "(%.1f ms + flip %.1f ms cada uno), %u saltados, %u descartados, %u sin imagen, %u del RSX",
		    fields * 1e6 / dt, fields * 1e6 / dt * 100.0 * n64.bus.vi.field_cycles / 93750000.0,
		    info->tv == SALSA64_TV_PAL ? "PAL" : "NTSC", (double)(cyc - idle) / dt,
		    cyc ? 100.0 * idle / cyc : 0.0, fields ? us_emu / 1000.0 / fields : 0.0,
		    fields ? us_copy / 1000.0 / fields : 0.0, fields ? us_pace / 1000.0 / fields : 0.0,
		    pacer.resets, pres.presented,
		    pres.presented ? pres.us_present / 1000.0 / pres.presented : 0.0,
		    pres.presented ? pres.us_flip / 1000.0 / pres.presented : 0.0, pres.skipped,
		    emu_stats.dropped, pres.blank, pres.rsx_frames);
	/* Tareas gráficas: tiempo de la HLE (en el SPE), espera del PPE,
	 * traducción a comandos del RSX y esperas por un juego de búferes. */
	struct salsa64_rdp_stats rs;
	salsa64_rdp_get_stats(&rs);
	uint32_t tasks = rs.tasks - rdp0.tasks;
	if (tasks) {
		double spu_ms = 0, wait_ms = 0, sleep_ms = 0;
		uint32_t waits = 0, polls = 0;
		if (gfx_on_spu) {
			spu_ms = (gfx_spu.spu_ticks - gfx_ticks0) * 1e3 / spukit_tick_hz() / tasks;
			wait_ms = (gfx_spu.wait_us - gfx_wait0) / 1e3 / tasks;
			waits = gfx_spu.waits - gfx_waits0;
			polls = gfx_spu.polls - gfx_polls0;
			sleep_ms = (gfx_spu.sleep_us - gfx_sleep0) / 1e3 / tasks;
		}
		salsa64_log("gfx: %u tareas (%.1f/s); por tarea: %.0f dibujos, %.0f triángulos, HLE en el "
			    "SPE %.2f ms, emulación esperando al SPE %.2f ms (%u esperas), %.1f sondeos y %.2f ms "
			    "dormida en bucles de espera, traducción al RSX %.2f ms; %u destinos creados, %u esperas "
			    "por un juego de búferes (%.2f ms); %u tareas cortadas, %u comandos de otro microcódigo "
			    "saltados",
			    tasks, tasks * 1e6 / dt, (double)(rs.draws - rdp0.draws) / tasks,
			    (double)(rs.triangles - rdp0.triangles) / tasks, spu_ms, wait_ms, waits,
			    (double)polls / tasks, sleep_ms, (rs.us_translate - rdp0.us_translate) / 1e3 / tasks,
			    rs.targets_created - rdp0.targets_created, rs.set_waits - rdp0.set_waits,
			    (rs.us_set_wait - rdp0.us_set_wait) / 1e3, n64.bus.rsp.stats.cut - gfx_cut0,
			    (unsigned)(gfx_spu.skipped - gfx_skipped0));
		if (gfx_on_spu) {
			double k = 1e3 / spukit_tick_hz() / tasks;
			salsa64_log("gfx en el SPE por tarea: vértices %.2f ms, estado %.2f ms (texturas %.2f ms), "
				    "cargas a la TMEM %.2f ms", (gfx_spu.t_vtx - gfx_vtx0) * k,
				    (gfx_spu.t_state - gfx_state0) * k, (gfx_spu.t_tex - gfx_tex0) * k,
				    (gfx_spu.t_load - gfx_load0) * k);
		}
	}
	if (lle_mode != SALSA64_LLE_OFF) {
		static uint32_t lle_tasks0, lle_jobs0, lle_polls0;
		static uint64_t lle_instr0, lle_ticks0, lle_wait0, lle_sleep0;
		uint32_t t = rsp_spu.tasks - lle_tasks0;
		salsa64_rsp_spu_stuck(&rsp_spu);
		if (t) {
			uint64_t instr = rsp_spu.instructions - lle_instr0, ticks = rsp_spu.spu_ticks - lle_ticks0;
			double us = ticks * 1e6 / spukit_tick_hz();
			salsa64_log("rsp lle: %u tareas (%u trabajos), %.0f instrucciones por tarea, %.2f ms en el SPE "
				    "por tarea, %.1f ns por instrucción (%.1f MIPS; el RSP real ~62), esperando %.2f ms, "
				    "%.1f sondeos y %.2f ms dormida por tarea, %u interrupciones, %u tramos del RDP",
				    t, rsp_spu.jobs - lle_jobs0, (double)instr / t, us / 1e3 / t,
				    instr ? us * 1e3 / instr : 0.0, us ? instr / us : 0.0,
				    (rsp_spu.wait_us - lle_wait0) / 1e3 / t, (double)(rsp_spu.polls - lle_polls0) / t,
				    (rsp_spu.sleep_us - lle_sleep0) / 1e3 / t, rsp_spu.irqs, rsp_spu.dpc_entries);
			salsa64_log("rsp lle COP0 por tarea: SP_STATUS %.0f lecturas/%.0f escrituras, semáforo %.0f/%.0f, "
				    "DMA %.0f filas dentro/%.0f fuera, DP %.0f lecturas, %.0f reintentos atómicos",
				    (double)rsp_spu.status_reads / rsp_spu.tasks, (double)rsp_spu.status_writes / rsp_spu.tasks,
				    (double)rsp_spu.sem_reads / rsp_spu.tasks, (double)rsp_spu.sem_writes / rsp_spu.tasks,
				    (double)rsp_spu.dma_rows_in / rsp_spu.tasks, (double)rsp_spu.dma_rows_out / rsp_spu.tasks,
				    (double)rsp_spu.dp_reads / rsp_spu.tasks, (double)rsp_spu.atomic_retries / rsp_spu.tasks);
		}
		lle_tasks0 = rsp_spu.tasks;
		lle_jobs0 = rsp_spu.jobs;
		lle_polls0 = rsp_spu.polls;
		lle_instr0 = rsp_spu.instructions;
		lle_ticks0 = rsp_spu.spu_ticks;
		lle_wait0 = rsp_spu.wait_us;
		lle_sleep0 = rsp_spu.sleep_us;
	}
	if (use_rec) {
		const struct salsa64_rec_stats *st = &rec.stats;
		salsa64_log("recompilador: %llu bloques (%llu fuera de KSEG0), %u KB de caché, %llu entradas, "
			    "%llu enlaces, %llu invalidados, %llu vaciados, %llu traducciones, "
			    "%llu instr. interpretadas",
			    (unsigned long long)st->compiled, (unsigned long long)st->compiled_x,
			    salsa64_rec_used(&rec) / 256, (unsigned long long)st->entries,
			    (unsigned long long)st->linked, (unsigned long long)st->invalidated,
			    (unsigned long long)st->flushes, (unsigned long long)st->vt_probes,
			    (unsigned long long)st->interp_steps);
	}
	gfx_vtx0 = gfx_spu.t_vtx;
	gfx_state0 = gfx_spu.t_state;
	gfx_tex0 = gfx_spu.t_tex;
	gfx_load0 = gfx_spu.t_load;
	rdp0 = rs;
	gfx_ticks0 = gfx_spu.spu_ticks;
	gfx_wait0 = gfx_spu.wait_us;
	gfx_waits0 = gfx_spu.waits;
	gfx_polls0 = gfx_spu.polls;
	gfx_sleep0 = gfx_spu.sleep_us;
	gfx_cut0 = n64.bus.rsp.stats.cut;
	gfx_skipped0 = gfx_spu.skipped;
	t_stats = now;
	fields0 = emu_stats.fields;
	us_emu0 = emu_stats.us_emu;
	us_copy0 = emu_stats.us_copy;
	us_pace0 = emu_stats.us_pace;
	cyc0 = n64.cpu.cycles;
	idle0 = n64.cpu.idle_cycles;
	pres.us_present = pres.us_flip = 0;
	pres.presented = pres.blank = pres.rsx_frames = pres.skipped = 0;
}

int main(void)
{
	salsa64_log_init("salsa64");
	pres.autotest = salsa64_autotest();

	int rc = rsxkit_init(NULL);
	pres.fb = rc == RSXKIT_OK ? rsxkit_fb_create(FB_MAX_W, FB_MAX_H) : NULL;
	if (!pres.fb) {
		salsa64_log("RESULTADO frontend: FALLO al iniciar el RSX (%d)", rc);
		salsa64_log_shutdown();
		return 1;
	}
	const rsxkit_screen *sc = rsxkit_screen_info();
	salsa64_log("RSX listo: %ux%u, tele %s", sc->width, sc->height, sc->aspect > 1.5f ? "16:9" : "4:3");
	if ((rc = salsa64_rdp_init(0)) != RSXKIT_OK) {
		salsa64_log("RESULTADO frontend: FALLO al preparar el RDP (%d)", rc);
		rsxkit_shutdown();
		salsa64_log_shutdown();
		return 1;
	}
	rdp_sink = salsa64_rdp_sink();

	{
		char name[80] = "";
		FILE *ef = fopen(EMBED_PATH, "r");
		if (ef) {
			if (!fgets(name, sizeof name, ef))
				name[0] = '\0';
			fclose(ef);
			name[strcspn(name, " \r\n")] = '\0';
		}
		if (!strncmp(name, "dump:", 5)) {
			char path[96];
			snprintf(path, sizeof path, SALSA64_DATA_DIR "/%s", name + 5);
			int rr = salsa64_replay(path);
			salsa64_log("RESULTADO frontend: %s (repetición)", rr == 0 ? "OK" : "FALLO");
			salsa64_rdp_shutdown();
			rsxkit_shutdown();
			salsa64_log_shutdown();
			return rr == 0 ? 0 : 1;
		}
	}

	uint32_t rom_size = 0;
	const char *source = NULL;
	uint8_t *rom = load_rom(&rom_size, &source);
	uint8_t *rdram = memalign(128, SALSA64_RDRAM_MAX);
	for (int i = 0; i < SNAP_SLOTS; i++)
		snap_buf[i] = memalign(128, SNAP_BYTES);
	if (!rom || !rdram || !snap_buf[SNAP_SLOTS - 1]) {
		salsa64_log("RESULTADO frontend: FALLO sin memoria");
		rsxkit_shutdown();
		salsa64_log_shutdown();
		return 1;
	}
	salsa64_rom_normalize(rom, rom_size);

	salsa64_n64_init(&n64, rdram, SALSA64_RDRAM_MAX);
	salsa64_bus_set_isviewer(&n64.bus, isv_buf, isv_out, NULL);

	/* HLE gráfica: en un SPE, o en el PPE si se pide. */
	int gfx_on_spu = !file_exists(GFX_PPE_PATH);
	if (gfx_on_spu) {
		spukit_init(6);
		if ((rc = salsa64_gfx_spu_init(&gfx_spu, &n64.bus, &front_sink)) != 0) {
			salsa64_log("no arrancó el SPE de la HLE gráfica (%d): uso el PPE", rc);
			gfx_on_spu = 0;
		}
	}
	if (gfx_on_spu) {
		salsa64_rsp_set_gfx(&n64.bus, &gfx_spu.backend);
	} else {
		salsa64_gfx_sync_init(&gfx_ppe, &gfx_ppe_state, &n64.bus, &front_sink);
		salsa64_rsp_set_gfx(&n64.bus, &gfx_ppe.backend);
	}
	salsa64_log("HLE gráfica en %s", gfx_on_spu ? "el SPE" : "el PPE");
	/* audio_out: inicio (HLE en el SPE 1, sumidero del AI, puerto de audio). */
	audio_out_init(&n64);
	/* RSP a bajo nivel en otro SPE. */
	lle_mode = lle_config();
	if (lle_mode != SALSA64_LLE_OFF) {
		spukit_init(6);
		if ((rc = salsa64_rsp_spu_init(&rsp_spu, &n64.bus)) != 0) {
			salsa64_log("rsp lle: no arrancó el SPE del microcódigo (%d): sin LLE", rc);
			lle_mode = SALSA64_LLE_OFF;
		} else {
			if (gfx_on_spu)
				salsa64_gfx_sync_init(&gfx_ppe, &gfx_ppe_state, &n64.bus, &front_sink);
			salsa64_rsp_set_dpc(&n64.bus, &gfx_ppe.dpc);
			salsa64_rsp_set_lle(&n64.bus, &rsp_spu.backend, lle_mode);
			salsa64_log("rsp lle: microcódigo real en un SPE para %s; el RDP en el PPE",
				    lle_mode == SALSA64_LLE_ALL ? "todas las tareas" : "lo que no tiene HLE");
		}
	}

	salsa64_bus_set_rom(&n64.bus, rom, rom_size);
	struct salsa64_boot_info info;
	if (!salsa64_boot_hle(&n64, SALSA64_BOOT_TV_AUTO, &info)) {
		salsa64_log("RESULTADO frontend: FALLO al arrancar %s: %s", source, info.error);
		rsxkit_shutdown();
		salsa64_log_shutdown();
		return 1;
	}
	char title[21];
	memcpy(title, rom + 0x20, 20);
	title[20] = '\0';
	salsa64_log("ROM %s \"%s\", %u bytes, %s, entrada 0x%08X, TV %s", source, title, rom_size,
		    info.kind == SALSA64_BOOT_ELF ? "ELF (IPL3 libre)" : "carga plana",
		    info.entry, info.tv == SALSA64_TV_PAL ? "PAL" : info.tv == SALSA64_TV_MPAL ? "MPAL" : "NTSC");
	savefile_open(&save_file, &save, &n64.bus, rom, SAVES_DIR, SAVE_CFG, SALSA64_SAVE_AUTO, salsa64_log);

	rec.opts = SALSA64_REC_OPT_PIN;  /* +7 % en nemu64-test (docs/perf-log.md) */
	if (!salsa64_ps3_rec_wanted())
		salsa64_log("recompilador apagado (%s): intérprete", SALSA64_NOREC_FLAG);
	else if (!(use_rec = salsa64_rec_init(&rec, &n64.cpu, salsa64_ps3_rec_cache(), SALSA64_REC_CACHE_BYTES)))
		salsa64_log("recompilador no disponible: intérprete");
	else
		salsa64_log("recompilador activo: caché de %u MB en %p, llamadas a C %s, opciones 0x%x",
			    SALSA64_REC_CACHE_BYTES >> 20, salsa64_ps3_rec_cache(), rec.calls_near ? "con bl" : "por mtctr",
			    rec.opts);

	ioPadInit(7);
	load_padmap();
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);
	if (selftest_on)
		selftest_start();
	if (gfxtest_on)
		gfxtest_start();
	if (audiotest_on)
		audiotest_start();
	{
		FILE *df = fopen(SALSA64_DATA_DIR "/gfxdump", "r");
		if (df) {
			if (fscanf(df, "%u", &pres.dump_field) != 1)
				pres.dump_field = 0;
			fclose(df);
		}
	}
	/* El puerto 1 con control neutro desde el primer campo. */
	update_pads();
	pacer_init(&pacer, !file_exists(TURBO_PATH), pacer_sleep, NULL);
	salsa64_log("ritmo: %s", pacer.enabled ? "el de la tele emulada (cada campo a su hora)" : "sin límite (turbo)");
	pres.shots_on = pres.autotest || file_exists(SHOTS_PATH);
	{
		FILE *cf = fopen(SHOTS_PATH, "r");
		if (cf) {
			if (fscanf(cf, "%u %u", &cap_every, &exit_field) < 1)
				cap_every = exit_field = 0;
			fclose(cf);
		}
	}
	if (cap_every)
		salsa64_log("capturas: campo-N.ppm cada %u campos%s", cap_every, exit_field ? ", y salida" : "");
	else if (pres.shots_on && !pres.autotest)
		salsa64_log("capturas: screen-N.ppm cada %u s (%s)", SHOT_US / 1000000u, SHOTS_PATH);
	gwin.on = gfx_on_spu;

	/* El hilo de emulación. */
	sys_sem_attr_t sem_attr = { SYS_SEM_ATTR_PROTOCOL, SYS_SEM_ATTR_PSHARED, 0, 0, 0, "wake" };
	sys_ppu_thread_t emu_tid;
	if (sysSemCreate(&wake_sem, &sem_attr, 0, 1) != 0 ||
	    sysThreadCreate(&emu_tid, emu_thread, NULL, EMU_PRIO, EMU_STACK, THREAD_JOINABLE,
			    (char *)"salsa64_emu") != 0) {
		salsa64_log("RESULTADO frontend: FALLO al crear el hilo de emulación");
		salsa64_rsp_set_gfx(&n64.bus, NULL);
		rsxkit_shutdown();
		salsa64_log_shutdown();
		return 1;
	}
	salsa64_log("hilos: emulación con prioridad %d, presentación con 1001", EMU_PRIO);

	uint64_t now = sysGetSystemTime();
	pres.t_shot = now;
	stats(now, gfx_on_spu, &info);
	while (running) {
		sysSemWait(wake_sem, WAKE_US);
		sysUtilCheckCallback();
		if (update_pads())
			break;
		now = sysGetSystemTime();
		savefile_write_staged(&save_file, now);
		drain(now, 1);
		stats(now, gfx_on_spu, &info);
	}

	/* Parar la emulación sin dejarla esperando un juego de búferes. */
	running = 0;
	while (!emu_done) {
		drain(sysGetSystemTime(), 0);
		sysUsleep(500);
	}
	u64 ret;
	sysThreadJoin(emu_tid, &ret);
	drain(sysGetSystemTime(), 0);

	savefile_close(&save_file);
	/* audio_out: cierre (con el hilo de emulación ya terminado). */
	audio_out_shutdown();
	salsa64_rsp_set_gfx(&n64.bus, NULL);  /* espera la tarea en vuelo */
	if (lle_mode != SALSA64_LLE_OFF) {
		salsa64_rsp_set_lle(&n64.bus, NULL, SALSA64_LLE_OFF);
		salsa64_rsp_set_dpc(&n64.bus, NULL);
		salsa64_log("rsp lle: %u tareas, %u trabajos, %llu instrucciones (la tarea más larga %u), %u fallidos, "
			    "%u tramos del RDP (%u perdidos); SPE detenido (salida %d)", rsp_spu.tasks, rsp_spu.jobs,
			    (unsigned long long)rsp_spu.instructions, rsp_spu.task_max, rsp_spu.failed,
			    rsp_spu.dpc_entries, rsp_spu.dpc_lost, salsa64_rsp_spu_shutdown(&rsp_spu));
	}
	if (gfx_on_spu)
		salsa64_gfx_spu_shutdown(&gfx_spu);
	/* Después de la tarea en vuelo: su fin llama a front_done → wake(). */
	sysSemDestroy(wake_sem);
	salsa64_rdp_shutdown();
	rsxkit_shutdown();
	ioPadEnd();
	salsa64_log("salida normal tras %u campos (%u emulados, %u descartados por la cola, %u sin ranura)",
		    n64.bus.vi.fields, emu_stats.fields, emu_stats.dropped, emu_stats.no_slot);
	salsa64_log_shutdown();
	return 0;
}
