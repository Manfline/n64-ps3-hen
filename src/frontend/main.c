/* SPDX-License-Identifier: GPL-2.0-or-later */
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
 * ROM: si existe SALSA64_DATA_DIR/embedded, se arranca directo lo que diga
 * (las pruebas automáticas dependen de esto): "vitest", "gfxtest" o
 * "audiotest", esa ROM de prueba embebida; "file:<nombre>", ese archivo de
 * SALSA64_DATA_DIR (para probar una ROM en RPCS3 sin pisar el rom.z64 que
 * leen otras compilaciones; las variantes de gfxtest, salsa64-gfxtest-*.z64,
 * se verifican como la embebida); "dump:<nombre>", la repetición de un
 * volcado de la HLE en el RSX (replay.c); otra cosa,
 * SALSA64_DATA_DIR/rom.z64 (cualquiera de los tres órdenes de bytes) si
 * existe y si no gfxtest. START + SELECT sale al XMB.
 *
 * Sin embedded, el menú (menu.c, romlist.c), con una disposición inspirada
 * en el XMB y dibujado entero por el RSX (rsxkit/ui.h): los .z64/.v64/.n64
 * de SALSA64_DATA_DIR y de /dev_usbNNN/salsa64 en una columna, ordenados
 * por título, con un panel de información (archivo, código, región, tipo de
 * guardado) y ondas de fondo; en Opciones se apagan las ondas (se guarda
 * en SALSA64_DATA_DIR/menu.cfg) y se elige el recorte del overscan (se
 * escribe overscan.cfg, que run_game relee en cada juego). Cruceta para elegir, X para abrir, triángulo
 * para Opciones; la última ROM abierta queda en SALSA64_DATA_DIR/ultima-rom
 * y es la elegida al volver. Dentro del juego, START + SELECT vuelve al menú: se
 * cierra todo lo del juego (guardado al disco, audio, HLE gráfica y de
 * audio, SPEs, recompilador, destinos del RSX, colas entre hilos, ritmo;
 * cada paso al log, game_close) y se puede abrir otro. «Salir al XMB» está
 * al final de la lista. El menú reserva al arrancar un búfer para la ROM
 * más grande (64 MB) y los 2 MB mapeados del 2D; nada más se asigna al
 * cambiar de juego salvo los bloques de control de los SPEs. Con
 * SALSA64_DATA_DIR/capturas (o en prueba automática) deja una captura del
 * menú en SALSA64_DATA_DIR/menu.ppm.
 *
 * Con las ROMs de prueba el frontend se verifica a sí mismo (selftest.c,
 * selftest_gfx.c) e imprime "RESULTADO frontend: OK/FALLO"; en modo de
 * prueba automática (platform/ps3/autotest.h) sale después. En prueba
 * automática sin embedded el menú se maneja solo: abre en orden las ROMs
 * que nombra SALSA64_DATA_DIR/menu-prueba (una por línea; las de prueba se
 * verifican como las embebidas), vuelve al menú después de cada una y sale
 * con "RESULTADO frontend" del conjunto (tools/rpcs3-run.py --menu).
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
 * Presentación: la imagen del VI sin el overscan (SALSA64_DATA_DIR/
 * overscan.cfg: píxeles por lado a 320 × 240; por omisión 8 y 6, 0 la
 * apaga), escalada a 4:3. Qué destino del RSX se presenta lo decide
 * src/rdp/targets.c.
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
 * Salir: «Salir al XMB» en el menú, START + SELECT sin menú (con embedded),
 * o "Salir del juego" desde el menú del sistema en cualquier momento.
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
#include <rsxkit/ui.h>
#include <spukit/spukit.h>

#include "core/boot.h"
#include "cpu/rec.h"
#include "audio_out.h"
#include "audiotest_rom_bin.h"
#include "gfxtest_rom_bin.h"
#include "menu.h"
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
#include "romlist.h"
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
#define LAST_ROM_PATH SALSA64_DATA_DIR "/ultima-rom"
#define MENU_TEST_PATH SALSA64_DATA_DIR "/menu-prueba"
#define MENU_CFG_PATH SALSA64_DATA_DIR "/menu.cfg"
#define MENU_TEST_MAX 8
#define MENU_IDLE_FIELDS 900  /* prueba automática: sin acción en tanto, falla */
#define OVERSCAN_PATH SALSA64_DATA_DIR "/overscan.cfg"
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
static uint8_t *rdram;
static volatile int running;    /* la sesión de juego sigue */
static volatile int emu_done;   /* el hilo de emulación salió de su bucle */
static volatile int app_quit;   /* «Salir del juego» del sistema: salir de la aplicación */
static int in_menu;             /* sin embedded: START + SELECT vuelve al menú */
/* Las líneas de resultado de una ROM de prueba: con el menú, la final es la
 * del conjunto (tools/rpcs3-run.py corta 30 s después de la primera). */
static const char *res_tag = "RESULTADO frontend";
static int game_auto_ok;        /* prueba automática con una ROM del disco: llegó a AUTO_FIELDS */
static int gfx_on_spu;
static struct salsa64_boot_info boot_info;
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
	if (status == SYSUTIL_EXIT_GAME) {
		app_quit = 1;
		running = 0;
	}
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

/* Controles, de presentación a emulación: el último estado (seqlock: seq
 * impar = escribiendo) y, aparte, el OR de los botones recién pulsados
 * desde que emulación los tomó, para que una pulsación publicada y soltada
 * entre dos campos llegue al PIF (que la retiene hasta entregarla al
 * juego). Solo los flancos: con todo lo pulsado, cada soltada se
 * retrasaría un campo. */
static struct {
	volatile uint32_t seq;
	struct salsa64_pad pads[SALSA64_PIF_PORTS];
	volatile uint32_t pressed[SALSA64_PIF_PORTS];
} pad_box;

static uint16_t pad_published[SALSA64_PIF_PORTS];

static void pads_publish(const struct salsa64_pad pads[SALSA64_PIF_PORTS])
{
	pad_box.seq++;
	lwsync();
	memcpy(pad_box.pads, pads, sizeof pad_box.pads);
	lwsync();
	pad_box.seq++;
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++) {
		uint16_t rise = (uint16_t)(pads[i].buttons & ~pad_published[i]);
		if (rise)
			__sync_fetch_and_or(&pad_box.pressed[i], rise);
		pad_published[i] = pads[i].buttons;
	}
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
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++) {
		if (pad_box.pressed[i])
			pads[i].buttons |= (uint16_t)__sync_fetch_and_and(&pad_box.pressed[i], 0);
		salsa64_pif_set_pad(&n64.bus, i, &pads[i]);
	}
}

/* Flancos de lo que el PIF entregó al juego (respuestas de estado): los
 * anota emulación al terminar cada campo y los escribe presentación. */
#define PIF_EDGES 32
static struct {
	struct salsa64_pad pad;
	uint32_t field, reads;
	uint8_t port;
} pif_edge[PIF_EDGES];
static volatile uint32_t pif_edge_head, pif_edge_tail, pif_edge_lost;
static uint16_t pif_edge_seen[SALSA64_PIF_PORTS];

static void pif_edges_note(void)
{
	const struct salsa64_pif *pif = &n64.bus.pif;
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++) {
		if (pif->sent[i].buttons == pif_edge_seen[i])
			continue;
		pif_edge_seen[i] = pif->sent[i].buttons;
		if (pif_edge_head - pif_edge_tail >= PIF_EDGES) {
			__sync_fetch_and_add(&pif_edge_lost, 1);
			continue;
		}
		uint32_t k = pif_edge_head % PIF_EDGES;
		pif_edge[k].pad = pif->sent[i];
		pif_edge[k].field = n64.bus.vi.fields;
		pif_edge[k].reads = pif->reads[i];
		pif_edge[k].port = (uint8_t)i;
		lwsync();
		pif_edge_head++;
	}
}

static void pif_edges_log(void)
{
	while (pif_edge_tail != pif_edge_head) {
		lwsync();  /* la entrada, después de ver head */
		uint32_t k = pif_edge_tail % PIF_EDGES;
		char desc[128];
		salsa64_pad_describe(&pif_edge[k].pad, desc, sizeof desc);
		salsa64_log("PIF → juego: control %u: %s (campo %u, lectura %u)", pif_edge[k].port + 1u, desc,
			    pif_edge[k].field, pif_edge[k].reads);
		lwsync();
		pif_edge_tail++;
	}
	uint32_t lost = pif_edge_lost ? __sync_fetch_and_and(&pif_edge_lost, 0) : 0;
	if (lost)
		salsa64_log("PIF → juego: %u flancos sin anotar (cola llena)", lost);
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

/* Lecturas de los mandos en la ventana de estadísticas y el hueco más
 * largo entre dos: una pulsación más corta que el hueco no llega a verse
 * (ioPadGetData da solo el último estado). */
static struct {
	uint64_t last_us, max_gap_us;
	uint32_t polls;
} pad_poll;

/* DualShock 3 → controles de la N64, publicados para el hilo de emulación.
 * ioPadGetData da len = 0 cuando nada cambió: se conserva el último
 * estado. Devuelve 1 si algún mando tiene START + SELECT; en `buttons`
 * (puede ser NULL), los botones de todos los mandos juntos (el menú). */
static int update_pads(uint32_t *buttons)
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
	uint32_t all = 0;

	if (buttons)
		*buttons = 0;
	uint64_t now = sysGetSystemTime();
	if (pad_poll.last_us && now - pad_poll.last_us > pad_poll.max_gap_us)
		pad_poll.max_gap_us = now - pad_poll.last_us;
	pad_poll.last_us = now;
	pad_poll.polls++;
	pif_edges_log();
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
			all |= last[i].buttons;
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
			/* El campo que emulación tenía en curso, para cotejar con el
			 * «PIF → juego» del mismo botón. */
			if (p.connected) {
				salsa64_pad_describe(&p, desc, sizeof desc);
				salsa64_log("control %u: %s (campo %u)", i + 1, desc, n64.bus.vi.fields);
			} else {
				salsa64_log("control %u: desconectado (campo %u)", i + 1, n64.bus.vi.fields);
			}
			*o = p;
		}
	}
	pads_publish(pads);
	if (buttons)
		*buttons = all;
	return exit;
}

/* ---- ROM ------------------------------------------------------------------ */

static int file_exists(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f)
		fclose(f);
	return f != NULL;
}

/* Texto del ISViewer: al log línea por línea (hilo de emulación). */
static char isv_line[256];
static uint32_t isv_len;

static void isv_out(void *ctx, const uint8_t *text, uint32_t len)
{
	char *line = isv_line;
	uint32_t n = isv_len;
	(void)ctx;
	if (selftest_on)
		selftest_isv(text, len);
	if (gfxtest_on)
		gfxtest_isv(text, len);
	if (audiotest_on)
		audiotest_isv(text, len);
	for (uint32_t i = 0; i < len; i++) {
		if (text[i] == '\n' || n == sizeof isv_line - 1) {
			line[n] = '\0';
			salsa64_log("rom: %s", line);
			n = 0;
		}
		if (text[i] != '\n')
			line[n++] = (char)text[i];
	}
	isv_len = n;
}

/* Lee una ROM entera: en `buf` (de `cap` bytes) o, con buf NULL, en
 * memoria reservada a su medida. NULL si no se pudo o no mide entre
 * 0x1000 bytes y ROM_MAX (o cap). */
static uint8_t *read_rom_file(const char *path, uint8_t *buf, uint32_t cap, uint32_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	long len = -1;
	if (fseek(f, 0, SEEK_END) == 0)
		len = ftell(f);
	uint8_t *dst = NULL;
	if (len >= 0x1000 && (uint32_t)len <= (buf ? cap : ROM_MAX) && fseek(f, 0, SEEK_SET) == 0) {
		dst = buf ? buf : memalign(128, (size_t)len);
		if (dst && fread(dst, 1, (size_t)len, f) != (size_t)len) {
			if (!buf)
				free(dst);
			dst = NULL;
		}
	}
	fclose(f);
	if (dst)
		*size = (uint32_t)len;
	return dst;
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
		uint8_t *buf = read_rom_file(path, NULL, 0, size);
		if (buf) {
			*source = path;
			if (!strncmp(name + 5, "salsa64-gfxtest", 15))
				gfxtest_on = 1;
			return buf;
		}
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
	if (file_exists(ROM_PATH)) {
		if ((rom = read_rom_file(ROM_PATH, NULL, 0, size))) {
			*source = ROM_PATH;
			return rom;
		}
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

/* Recorte del overscan: los bordes de la imagen que una tele de tubo no
 * muestra (y donde los juegos dejan basura: GoldenEye rellena de azul la
 * primera y la última columna). En píxeles de la N64 a 320 × 240, por
 * lado; se escala con la imagen del VI (a 640 de ancho recorta el doble).
 * Por omisión 8 y 6 (2,5 % por lado); overscan.cfg lo cambia con un
 * número (todos los lados), dos (horizontal, vertical) o cuatro
 * (izquierda, derecha, arriba, abajo); 0 lo apaga. Las ROMs de prueba se
 * presentan enteras (sus verificaciones leen la pantalla por posición). */
static struct { uint32_t left, right, top, bottom; } overscan = { 8, 8, 6, 6 };

static void load_overscan(void)
{
	FILE *f = fopen(OVERSCAN_PATH, "r");
	if (!f) {
		salsa64_log("overscan: recorte %u %u %u %u por omisión (no hay %s)", overscan.left, overscan.right,
			    overscan.top, overscan.bottom, OVERSCAN_PATH);
		return;
	}
	unsigned v[4] = { 0, 0, 0, 0 };
	int n = fscanf(f, "%u %u %u %u", &v[0], &v[1], &v[2], &v[3]);
	fclose(f);
	for (int i = 0; i < 4; i++)
		if (v[i] > 64)
			v[i] = 64;
	if (n == 1)
		overscan.left = overscan.right = overscan.top = overscan.bottom = v[0];
	else if (n == 2)
		overscan.left = overscan.right = v[0], overscan.top = overscan.bottom = v[1];
	else if (n == 4)
		overscan.left = v[0], overscan.right = v[1], overscan.top = v[2], overscan.bottom = v[3];
	else
		salsa64_log("overscan: %s sin números; queda el recorte por omisión", OVERSCAN_PATH);
	salsa64_log("overscan: recorte %u %u %u %u (%s)", overscan.left, overscan.right, overscan.top,
		    overscan.bottom, OVERSCAN_PATH);
}

/* El recorte de overscan.cfg como opción del menú (Opciones). */
static int overscan_preset(void)
{
	uint32_t l = overscan.left, r = overscan.right, t = overscan.top, b = overscan.bottom;
	if (!l && !r && !t && !b)
		return MENU_OVERSCAN_OFF;
	if (l == 8 && r == 8 && t == 6 && b == 6)
		return MENU_OVERSCAN_NORMAL;
	if (l == 16 && r == 16 && t == 12 && b == 12)
		return MENU_OVERSCAN_WIDE;
	return MENU_OVERSCAN_CUSTOM;
}

static void overscan_save(int preset)
{
	const char *text = preset == MENU_OVERSCAN_OFF ? "0\n" : preset == MENU_OVERSCAN_WIDE ? "16 12\n" : "8 6\n";
	FILE *f = fopen(OVERSCAN_PATH, "w");
	bool ok = f && fputs(text, f) >= 0;
	if (f && fclose(f) != 0)
		ok = false;
	if (!ok)
		salsa64_log("menú: no se pudo escribir %s", OVERSCAN_PATH);
	load_overscan();
}

/* El recorte para una imagen del VI (0 con las ROMs de prueba). */
static struct salsa64_overscan overscan_for(const struct salsa64_vi_frame *f)
{
	struct salsa64_overscan c = { 0, 0, 0, 0 };
	if (selftest_on || gfxtest_on || audiotest_on || !f->width || !f->height)
		return c;
	c.left = overscan.left * f->width / 320;
	c.right = overscan.right * f->width / 320;
	c.top = overscan.top * f->height / 240;
	c.bottom = overscan.bottom * f->height / 240;
	if (c.left + c.right >= f->width)
		c.left = c.right = 0;
	if (c.top + c.bottom >= f->height)
		c.top = c.bottom = 0;
	return c;
}

/* ---- Hilo de emulación ------------------------------------------------------ */

/* Al terminar un campo: la imagen del VI (copiada a una ranura) y el
 * mensaje para presentación. Si hay un campo anterior sin consumir y no
 * lleva verificación, se marca para saltarlo y se le suelta la ranura. */
static uint32_t post_last = ~0u;  /* índice del último campo encolado */

static void post_field(void)
{
	uint32_t last_idx = post_last;
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
	post_last = h;
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
static uint32_t unknown_gbi_next = 1;

static void log_unknown_gbi(void)
{
	const struct salsa64_rsp_stats *st = &n64.bus.rsp.stats;
	if (st->unknown_gbi < unknown_gbi_next)
		return;
	char text[96];
	salsa64_rsp_ucode_text(&n64.bus, st->unknown_data, text, sizeof text);
	salsa64_log("RSP: %u tareas gráficas con microcódigo sin HLE (datos en 0x%08x: \"%s\"); terminan sin dibujar",
		    st->unknown_gbi, (unsigned)st->unknown_data, text);
	unknown_gbi_next = st->unknown_gbi * 2;
}

/* Fondos prerrenderizados (Zelda OoT): las tareas JPEG de una sala llegan
 * en tanda al cargarla (la HLE corre en el PPE, en el hilo del CPU); al
 * terminar la tanda (un campo sin tareas nuevas), al log cuántas y cuánto
 * tardaron. */
static struct {
	uint32_t seen, tasks, mb;
	uint64_t us;
} jpeg_log;

static uint64_t clock_us(void)
{
	return (uint64_t)sysGetSystemTime();
}

static void log_jpeg(void)
{
	const struct salsa64_rsp_stats *st = &n64.bus.rsp.stats;
	if (st->jpeg < jpeg_log.tasks)
		memset(&jpeg_log, 0, sizeof jpeg_log);  /* otro juego */
	if (st->jpeg == jpeg_log.seen && st->jpeg != jpeg_log.tasks) {
		salsa64_log("JPEG: %u tareas, %u macrobloques en %.2f ms del PPE (fondo prerrenderizado); %u con "
			    "parámetros malos en total", st->jpeg - jpeg_log.tasks, st->jpeg_mb - jpeg_log.mb,
			    (st->jpeg_us - jpeg_log.us) / 1e3, st->jpeg_bad);
		jpeg_log.tasks = st->jpeg;
		jpeg_log.mb = st->jpeg_mb;
		jpeg_log.us = st->jpeg_us;
	}
	jpeg_log.seen = st->jpeg;
}

static void emu_thread(void *arg)
{
	(void)arg;
	while (running) {
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
		pads_apply();  /* después de la espera: lo más reciente de los mandos */
		uint64_t cycles = salsa64_n64_run_field(&n64);
		uint64_t t1 = sysGetSystemTime();
		pif_edges_note();
		log_unknown_gbi();
		log_jpeg();
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
	/* Retraso de la presentación: campos que la emulación lleva de ventaja
	 * al campo que se presenta (cola entre hilos), sumado y el mayor. */
	uint64_t behind_sum;
	uint32_t behind_max;
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
	uint32_t behind = n64.bus.vi.fields - m->field;  /* lectura de 32 bits del otro hilo */
	pres.behind_sum += behind;
	if (behind > pres.behind_max)
		pres.behind_max = behind;
	rsxkit_frame_begin(0x000000);
	/* Lo que se ve: la imagen del VI sin el overscan (las capturas van de
	 * ese tamaño). */
	struct salsa64_overscan crop = overscan_for(&m->f);
	struct salsa64_vi_frame shown = m->f;
	shown.width -= crop.left + crop.right;
	shown.height -= crop.top + crop.bottom;
	if (m->f.width) {
		if (salsa64_rdp_present(&m->f, m->field, &pres.view, &crop, &r)) {
			pres.rsx_frames++;
			if (gfxtest_on && pres.result == SELFTEST_RUNNING)
				check_gfx = gfxtest_want_screen();
		} else if (have) {
			check = (m->flags & FIELD_CHECK) != 0;
			uint32_t line = m->f.width * m->f.bpp;
			rsxkit_fb_upload(pres.fb, snap_buf[slot] + crop.top * line + crop.left * m->f.bpp, shown.width,
					 shown.height, line, m->f.bpp == 4 ? RSXKIT_RGBA8888 : RSXKIT_RGBA5551);
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
		salsa64_screenshot(path, &shown, &r);
		salsa64_log("captura %s", path);
	} else if (pres.shots_on && !cap_every && !selftest_on && !gfxtest_on && !audiotest_on && m->f.width &&
		   (pres.autotest ? m->field / AUTO_SHOT_FIELDS != pres.shots : now - pres.t_shot >= SHOT_US)) {
		char path[64];
		snprintf(path, sizeof path, SALSA64_DATA_DIR "/screen-%u.ppm", pres.shots % SHOT_MAX + 1);
		rsxkit_finish();
		salsa64_screenshot(path, &shown, &r);
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
		salsa64_log("%s: OK (ROM del disco, %u campos)", res_tag, m->field);
		game_auto_ok = 1;
		running = 0;
	}
	if ((selftest_on || gfxtest_on || audiotest_on) && pres.result == SELFTEST_RUNNING) {
		pres.result = selftest_on ? selftest_state(m->field)
			    : gfxtest_on  ? gfxtest_state(m->field)
					  : audiotest_state(m->field);
		if (pres.result != SELFTEST_RUNNING) {
			salsa64_log("%s: %s (campo %u)", res_tag, pres.result == SELFTEST_OK ? "OK" : "FALLO",
				    m->field);
			if (pres.autotest)
				running = 0;
			else if (pres.result == SELFTEST_OK && selftest_on)
				bench_request = 1;
		}
	}
}

/* Consume lo encolado: campos en orden y, al final, las tareas gráficas que
 * ya terminaron (son del campo siguiente). Solo lo que había al entrar: con
 * el flip en vsync y la emulación al mismo ritmo, mientras se presenta un
 * campo llega el siguiente, la cola no se vacía y el bucle de presentación
 * no volvía a leer los mandos (7,4 s sin leerlos en la selección de ranura
 * de Zelda, PS3, 2026-10-11). */
static void drain(uint64_t now, int present)
{
	uint32_t head = field_head;
	while (field_tail != head) {
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

/* Estadísticas de los dos hilos, cada STATS_US: lo visto en la ventana
 * anterior (se vacía al empezar cada juego). */
static struct {
	uint64_t t_stats, us_emu0, us_copy0, us_pace0, cyc0, idle0;
	uint32_t fields0, pif_reads0;
	struct salsa64_rdp_stats rdp0;
	uint32_t gfx_waits0, gfx_polls0, gfx_cut0;
	uint64_t gfx_skipped0, gfx_bg0;
	uint64_t gfx_ticks0, gfx_wait0, gfx_sleep0, gfx_vtx0, gfx_state0, gfx_tex0, gfx_load0;
	uint32_t lle_tasks0, lle_jobs0, lle_polls0;
	uint64_t lle_instr0, lle_ticks0, lle_wait0, lle_sleep0;
} sw;

static void stats(uint64_t now)
{
	const struct salsa64_boot_info *info = &boot_info;

	if (!sw.t_stats) {
		sw.t_stats = now;
		salsa64_rdp_get_stats(&sw.rdp0);
		pad_poll.polls = 0;
		pad_poll.max_gap_us = 0;
		return;
	}
	uint64_t dt = now - sw.t_stats;
	if (dt < STATS_US)
		return;
	/* Los contadores de emulación se leen sin lock: palabras de 64 bits
	 * alineadas, una línea de estadísticas. */
	uint32_t fields = emu_stats.fields - sw.fields0;
	uint64_t us_emu = emu_stats.us_emu - sw.us_emu0, us_copy = emu_stats.us_copy - sw.us_copy0;
	uint64_t us_pace = emu_stats.us_pace - sw.us_pace0;
	uint64_t cyc = n64.cpu.cycles - sw.cyc0, idle = n64.cpu.idle_cycles - sw.idle0;
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
	/* Retraso de lo que se ve, en campos: la cola entre hilos (campos que la
	 * emulación lleva de ventaja al presentar) y la antigüedad del destino
	 * (campos entre su dibujo y el campo que lo muestra: lo que tarda el
	 * juego en mostrar lo que dibuja, 2 con doble búfer a 30 cps). Para
	 * comparar la sensación de input lag entre compilaciones y juegos. */
	if (pres.presented)
		salsa64_log("retraso: cola %.2f campos (máx %u); destino %.2f campos de antigüedad (máx %u) en %u "
			    "campos del RSX",
			    (double)pres.behind_sum / pres.presented, pres.behind_max,
			    pres.rsx_frames ? (double)(rs.age_sum - sw.rdp0.age_sum) / pres.rsx_frames : 0.0,
			    rs.age_max, pres.rsx_frames);
	/* Controles: cada cuánto se leyeron los mandos (el hueco más largo
	 * es la pulsación más corta que puede perderse) y cuántas veces leyó
	 * el juego el control 1. */
	salsa64_log("controles: %u lecturas de los mandos (hueco máx %.1f ms); el juego leyó el control 1 "
		    "%u veces (%.1f/s)",
		    pad_poll.polls, pad_poll.max_gap_us / 1e3, n64.bus.pif.reads[0] - sw.pif_reads0,
		    (n64.bus.pif.reads[0] - sw.pif_reads0) * 1e6 / dt);
	uint32_t tasks = rs.tasks - sw.rdp0.tasks;
	if (tasks) {
		double spu_ms = 0, wait_ms = 0, sleep_ms = 0;
		uint32_t waits = 0, polls = 0;
		if (gfx_on_spu) {
			spu_ms = (gfx_spu.spu_ticks - sw.gfx_ticks0) * 1e3 / spukit_tick_hz() / tasks;
			wait_ms = (gfx_spu.wait_us - sw.gfx_wait0) / 1e3 / tasks;
			waits = gfx_spu.waits - sw.gfx_waits0;
			polls = gfx_spu.polls - sw.gfx_polls0;
			sleep_ms = (gfx_spu.sleep_us - sw.gfx_sleep0) / 1e3 / tasks;
		}
		salsa64_log("gfx: %u tareas (%.1f/s); por tarea: %.0f dibujos, %.0f triángulos, HLE en el "
			    "SPE %.2f ms, emulación esperando al SPE %.2f ms (%u esperas), %.1f sondeos y %.2f ms "
			    "dormida en bucles de espera, traducción al RSX %.2f ms; %u destinos creados, %u esperas "
			    "por un juego de búferes (%.2f ms); %u tareas cortadas, %u comandos de otro microcódigo "
			    "saltados, %u fondos de S2DEX",
			    tasks, tasks * 1e6 / dt, (double)(rs.draws - sw.rdp0.draws) / tasks,
			    (double)(rs.triangles - sw.rdp0.triangles) / tasks, spu_ms, wait_ms, waits,
			    (double)polls / tasks, sleep_ms, (rs.us_translate - sw.rdp0.us_translate) / 1e3 / tasks,
			    rs.targets_created - sw.rdp0.targets_created, rs.set_waits - sw.rdp0.set_waits,
			    (rs.us_set_wait - sw.rdp0.us_set_wait) / 1e3, n64.bus.rsp.stats.cut - sw.gfx_cut0,
			    (unsigned)(gfx_spu.skipped - sw.gfx_skipped0), (unsigned)(gfx_spu.bg - sw.gfx_bg0));
		if (gfx_on_spu) {
			double k = 1e3 / spukit_tick_hz() / tasks;
			salsa64_log("gfx en el SPE por tarea: vértices %.2f ms, estado %.2f ms (texturas %.2f ms), "
				    "cargas a la TMEM %.2f ms", (gfx_spu.t_vtx - sw.gfx_vtx0) * k,
				    (gfx_spu.t_state - sw.gfx_state0) * k, (gfx_spu.t_tex - sw.gfx_tex0) * k,
				    (gfx_spu.t_load - sw.gfx_load0) * k);
		}
		salsa64_log("gfx: fragment programs: %.1f cambios por tarea, %u dibujos con programa generado "
			    "(%.1f por tarea), %u programas en la caché (%u generados, %u vaciados)",
			    (double)(rs.fp_loads - sw.rdp0.fp_loads) / tasks, rs.draws_generated - sw.rdp0.draws_generated,
			    (double)(rs.draws_generated - sw.rdp0.draws_generated) / tasks, rs.fp_programs,
			    rs.fp_generated - sw.rdp0.fp_generated, rs.fp_evictions - sw.rdp0.fp_evictions);
	}
	if (lle_mode != SALSA64_LLE_OFF) {
		uint32_t t = rsp_spu.tasks - sw.lle_tasks0;
		salsa64_rsp_spu_stuck(&rsp_spu);
		if (t) {
			uint64_t instr = rsp_spu.instructions - sw.lle_instr0, ticks = rsp_spu.spu_ticks - sw.lle_ticks0;
			double us = ticks * 1e6 / spukit_tick_hz();
			salsa64_log("rsp lle: %u tareas (%u trabajos), %.0f instrucciones por tarea, %.2f ms en el SPE "
				    "por tarea, %.1f ns por instrucción (%.1f MIPS; el RSP real ~62), esperando %.2f ms, "
				    "%.1f sondeos y %.2f ms dormida por tarea, %u interrupciones, %u tramos del RDP",
				    t, rsp_spu.jobs - sw.lle_jobs0, (double)instr / t, us / 1e3 / t,
				    instr ? us * 1e3 / instr : 0.0, us ? instr / us : 0.0,
				    (rsp_spu.wait_us - sw.lle_wait0) / 1e3 / t, (double)(rsp_spu.polls - sw.lle_polls0) / t,
				    (rsp_spu.sleep_us - sw.lle_sleep0) / 1e3 / t, rsp_spu.irqs, rsp_spu.dpc_entries);
			salsa64_log("rsp lle COP0 por tarea: SP_STATUS %.0f lecturas/%.0f escrituras, semáforo %.0f/%.0f, "
				    "DMA %.0f filas dentro/%.0f fuera, DP %.0f lecturas, %.0f reintentos atómicos",
				    (double)rsp_spu.status_reads / rsp_spu.tasks, (double)rsp_spu.status_writes / rsp_spu.tasks,
				    (double)rsp_spu.sem_reads / rsp_spu.tasks, (double)rsp_spu.sem_writes / rsp_spu.tasks,
				    (double)rsp_spu.dma_rows_in / rsp_spu.tasks, (double)rsp_spu.dma_rows_out / rsp_spu.tasks,
				    (double)rsp_spu.dp_reads / rsp_spu.tasks, (double)rsp_spu.atomic_retries / rsp_spu.tasks);
		}
		sw.lle_tasks0 = rsp_spu.tasks;
		sw.lle_jobs0 = rsp_spu.jobs;
		sw.lle_polls0 = rsp_spu.polls;
		sw.lle_instr0 = rsp_spu.instructions;
		sw.lle_ticks0 = rsp_spu.spu_ticks;
		sw.lle_wait0 = rsp_spu.wait_us;
		sw.lle_sleep0 = rsp_spu.sleep_us;
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
	sw.gfx_vtx0 = gfx_spu.t_vtx;
	sw.gfx_state0 = gfx_spu.t_state;
	sw.gfx_tex0 = gfx_spu.t_tex;
	sw.gfx_load0 = gfx_spu.t_load;
	sw.rdp0 = rs;
	sw.gfx_ticks0 = gfx_spu.spu_ticks;
	sw.gfx_wait0 = gfx_spu.wait_us;
	sw.gfx_waits0 = gfx_spu.waits;
	sw.gfx_polls0 = gfx_spu.polls;
	sw.gfx_sleep0 = gfx_spu.sleep_us;
	sw.gfx_cut0 = n64.bus.rsp.stats.cut;
	sw.gfx_skipped0 = gfx_spu.skipped;
	sw.gfx_bg0 = gfx_spu.bg;
	sw.t_stats = now;
	sw.pif_reads0 = n64.bus.pif.reads[0];
	pad_poll.polls = 0;
	pad_poll.max_gap_us = 0;
	sw.fields0 = emu_stats.fields;
	sw.us_emu0 = emu_stats.us_emu;
	sw.us_copy0 = emu_stats.us_copy;
	sw.us_pace0 = emu_stats.us_pace;
	sw.cyc0 = n64.cpu.cycles;
	sw.idle0 = n64.cpu.idle_cycles;
	pres.us_present = pres.us_flip = 0;
	pres.presented = pres.blank = pres.rsx_frames = pres.skipped = 0;
	pres.behind_sum = pres.behind_max = 0;
}

/* ---- Sesión de juego ------------------------------------------------------------ */

enum game_end {
	GAME_TO_MENU,  /* START + SELECT con menú, o fin de una prueba abierta desde el menú */
	GAME_QUIT,     /* salir de la aplicación (sin menú, o «Salir del juego» del sistema) */
	GAME_FAILED,   /* no arrancó */
};

/* Lo que es de un juego vuelve a como al arrancar el programa: colas entre
 * hilos, ranuras de la copia del VI, contadores y estadísticas, banderas de
 * las pruebas, capturas. Antes de cada run_game. */
static void game_reset(void)
{
	running = 1;
	emu_done = 0;
	selftest_on = gfxtest_on = audiotest_on = 0;
	game_auto_ok = 0;
	use_rec = false;
	lle_mode = SALSA64_LLE_OFF;
	gfx_on_spu = 0;
	memset(&boot_info, 0, sizeof boot_info);
	memset(&save_file, 0, sizeof save_file);
	memset(field_q, 0, sizeof field_q);
	field_head = field_tail = 0;
	pif_edge_head = pif_edge_tail = pif_edge_lost = 0;
	memset(pif_edge_seen, 0, sizeof pif_edge_seen);
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++)
		pad_box.pressed[i] = 0;
	memset(pad_published, 0, sizeof pad_published);
	post_last = ~0u;
	for (int i = 0; i < SNAP_SLOTS; i++)
		snap_state[i] = SNAP_FREE;
	memset((void *)&emu_stats, 0, sizeof emu_stats);
	bench_request = 0;
	memset(&bench, 0, sizeof bench);
	memset(&gwin, 0, sizeof gwin);
	memset(&sw, 0, sizeof sw);
	cap_every = exit_field = 0;
	isv_len = 0;
	unknown_gbi_next = 1;
	pres.shots_on = 0;
	pres.result = SELFTEST_RUNNING;
	pres.dump_field = 0;
	pres.t_shot = 0;
	pres.shots = 0;
	pres.us_present = pres.us_flip = 0;
	pres.presented = pres.blank = pres.rsx_frames = pres.skipped = 0;
	pres.behind_sum = pres.behind_max = 0;
}

/* Cierra lo que es del juego con la emulación ya detenida, en orden y con
 * cada paso al log: el guardado al disco, el audio (puerto, hilo y SPE), la
 * HLE gráfica (espera la tarea en vuelo) y el LLE con sus SPEs, el
 * recompilador, las colas entre hilos y los destinos del RSX. Termina con
 * una línea que dice si quedó algo en vuelo. */
static void game_close(void)
{
	salsa64_log("cierre: emulación detenida tras %u campos (%u emulados, %u descartados por la cola, %u sin ranura)",
		    n64.bus.vi.fields, emu_stats.fields, emu_stats.dropped, emu_stats.no_slot);
	savefile_close(&save_file);
	salsa64_log("cierre: guardado escrito y desconectado");
	/* audio_out: cierre (con el hilo de emulación ya terminado). */
	audio_out_shutdown();
	salsa64_log("cierre: audio cerrado (%u tareas de audio)", n64.bus.rsp.stats.audio);
	salsa64_rsp_set_gfx(&n64.bus, NULL);  /* espera la tarea en vuelo */
	salsa64_log("cierre: HLE gráfica suelta (%u tareas gráficas)", n64.bus.rsp.stats.gfx);
	if (lle_mode != SALSA64_LLE_OFF) {
		salsa64_rsp_set_lle(&n64.bus, NULL, SALSA64_LLE_OFF);
		salsa64_rsp_set_dpc(&n64.bus, NULL);
		salsa64_log("rsp lle: %u tareas, %u trabajos, %llu instrucciones (la tarea más larga %u), %u fallidos, "
			    "%u tramos del RDP (%u perdidos); SPE detenido (salida %d)", rsp_spu.tasks, rsp_spu.jobs,
			    (unsigned long long)rsp_spu.instructions, rsp_spu.task_max, rsp_spu.failed,
			    rsp_spu.dpc_entries, rsp_spu.dpc_lost, salsa64_rsp_spu_shutdown(&rsp_spu));
		lle_mode = SALSA64_LLE_OFF;
	}
	if (gfx_on_spu) {
		salsa64_gfx_spu_shutdown(&gfx_spu);
		salsa64_log("cierre: SPE de la HLE gráfica detenido (%u tareas)", gfx_spu.tasks);
	}
	if (use_rec) {
		salsa64_rec_detach(&rec);
		salsa64_log("cierre: recompilador suelto (%u KB de caché usados; el próximo juego empieza vacío)",
			    salsa64_rec_used(&rec) / 256);
		use_rec = false;
	}
	/* Lo que quedó en las colas (sin presentar) y la memoria del RSX. */
	drain(sysGetSystemTime(), 0);
	uint32_t untranslated = salsa64_rdp_reset();
	int snaps = 0;
	for (int i = 0; i < SNAP_SLOTS; i++)
		snaps += snap_state[i] != SNAP_FREE;
	uint32_t queued = field_head - field_tail;
	bool spes = gfx_spu.worker || rsp_spu.worker;
	bool hooks = n64.bus.rsp.gfx || n64.bus.rsp.audio || n64.bus.rsp.lle || n64.bus.rsp.busy;
	bool clean = emu_done && !queued && !snaps && !untranslated && !spes && !hooks;
	salsa64_log("cierre: %s (hilo de emulación %s, %u campos en cola, %d ranuras del VI ocupadas, %u tareas "
		    "sin traducir, destinos del RSX liberados, SPEs de la HLE %s, RSP %s)",
		    clean ? "estado limpio" : "ATENCIÓN, quedó algo en vuelo", emu_done ? "terminado" : "VIVO", queued,
		    snaps, untranslated, spes ? "VIVOS" : "detenidos", hooks ? "CON BACKENDS" : "sin backends");
}

/* Una sesión de juego con la ROM ya en memoria (cualquier orden de bytes):
 * la arranca, la emula en dos hilos hasta START + SELECT, «Salir del
 * juego» o el fin de una prueba, y lo cierra todo (game_close). Llamar
 * después de game_reset y de poner las banderas de las ROMs de prueba. Si
 * no arranca, deja el motivo en `err`. */
static enum game_end run_game(uint8_t *rom, uint32_t rom_size, const char *source, char *err, size_t err_size)
{
	int rc;
	err[0] = '\0';
	salsa64_rom_normalize(rom, rom_size);
	memset(rdram, 0, SALSA64_RDRAM_MAX);  /* cada juego arranca igual, venga después del que venga */
	salsa64_n64_init(&n64, rdram, SALSA64_RDRAM_MAX);
	salsa64_bus_set_isviewer(&n64.bus, isv_buf, isv_out, NULL);

	/* HLE gráfica: en un SPE, o en el PPE si se pide. */
	gfx_on_spu = !file_exists(GFX_PPE_PATH);
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
	n64.bus.rsp.clock_us = clock_us;
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
	if (!salsa64_boot_hle(&n64, SALSA64_BOOT_TV_AUTO, &boot_info)) {
		salsa64_log("%s: FALLO al arrancar %s: %s", res_tag, source, boot_info.error);
		snprintf(err, err_size, "No arrancó %s: %s", source, boot_info.error);
		emu_done = 1;
		game_close();
		return GAME_FAILED;
	}
	char title[21];
	memcpy(title, rom + 0x20, 20);
	title[20] = '\0';
	salsa64_log("ROM %s \"%s\", %u bytes, %s, entrada 0x%08X, TV %s", source, title, rom_size,
		    boot_info.kind == SALSA64_BOOT_ELF ? "ELF (IPL3 libre)" : "carga plana", boot_info.entry,
		    boot_info.tv == SALSA64_TV_PAL ? "PAL" : boot_info.tv == SALSA64_TV_MPAL ? "MPAL" : "NTSC");
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

	/* overscan.cfg: en cada juego (se cambia desde las Opciones del menú). */
	load_overscan();
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
	update_pads(NULL);
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
	sys_ppu_thread_t emu_tid;
	if (sysThreadCreate(&emu_tid, emu_thread, NULL, EMU_PRIO, EMU_STACK, THREAD_JOINABLE,
			    (char *)"salsa64_emu") != 0) {
		salsa64_log("%s: FALLO al crear el hilo de emulación", res_tag);
		snprintf(err, err_size, "No se pudo crear el hilo de emulación");
		emu_done = 1;
		game_close();
		return GAME_FAILED;
	}
	salsa64_log("hilos: emulación con prioridad %d, presentación con 1001", EMU_PRIO);

	uint64_t now = sysGetSystemTime();
	pres.t_shot = now;
	stats(now);
	while (running) {
		sysSemWait(wake_sem, WAKE_US);
		sysUtilCheckCallback();
		if (update_pads(NULL)) {
			salsa64_log("START + SELECT: %s", in_menu ? "vuelta al menú" : "salida");
			break;
		}
		now = sysGetSystemTime();
		savefile_write_staged(&save_file, now);
		drain(now, 1);
		stats(now);
	}
	if (app_quit)
		salsa64_log("«Salir del juego» del sistema");

	/* Parar la emulación sin dejarla esperando un juego de búferes. */
	running = 0;
	while (!emu_done) {
		drain(sysGetSystemTime(), 0);
		sysUsleep(500);
	}
	u64 ret;
	sysThreadJoin(emu_tid, &ret);
	drain(sysGetSystemTime(), 0);
	game_close();
	return app_quit || !in_menu ? GAME_QUIT : GAME_TO_MENU;
}

/* ---- Menú ------------------------------------------------------------------------- */

static const char *const rom_dirs[] = {
	SALSA64_DATA_DIR,
	"/dev_usb000/salsa64", "/dev_usb001/salsa64", "/dev_usb002/salsa64", "/dev_usb003/salsa64",
	"/dev_usb004/salsa64", "/dev_usb005/salsa64", "/dev_usb006/salsa64", "/dev_usb007/salsa64",
};

static struct romlist roms;
static struct menu menu;
static struct {
	uint8_t *rom;           /* búfer de la ROM (ROM_MAX), reservado al entrar al menú */
	float lw, lh;           /* espacio lógico del menú */
	struct menu_settings settings;
	char save_cfg[4096];
	size_t save_cfg_len;
	/* Prueba automática (menu-prueba). */
	char test[MENU_TEST_MAX][ROMLIST_PATH_MAX];
	unsigned ntest, next_test;
	uint32_t step, idle;
	int games, failures;
} mn;

/* ROMs de prueba abiertas desde el menú: se verifican como las embebidas. */
static void test_flags_from_name(const char *name)
{
	if (!strncmp(name, "salsa64-vitest", 14))
		selftest_on = 1;
	else if (!strncmp(name, "salsa64-gfxtest", 15))
		gfxtest_on = 1;
	else if (!strncmp(name, "salsa64-audiotest", 17))
		audiotest_on = 1;
}

/* Arma la lista (con la fila de `keep` elegida, si sigue ahí). */
static void menu_scan(const char *keep)
{
	memset(&roms, 0, sizeof roms);
	FILE *f = fopen(SAVE_CFG, "rb");
	mn.save_cfg_len = f ? fread(mn.save_cfg, 1, sizeof mn.save_cfg - 1, f) : 0;
	if (f)
		fclose(f);
	for (unsigned i = 0; i < sizeof rom_dirs / sizeof rom_dirs[0]; i++) {
		int n = romlist_scan_dir(&roms, rom_dirs[i], mn.save_cfg, mn.save_cfg_len);
		if (n >= 0)
			salsa64_log("menú: %d ROMs en %s", n, rom_dirs[i]);
	}
	romlist_sort(&roms);
	for (uint32_t i = 0; i < roms.count; i++) {
		const struct rom_entry *e = &roms.e[i];
		salsa64_log("menú: %2u. \"%s\" %s (%s v%u, guardado %s, %s, %u bytes)", i + 1, romlist_display_title(e),
			    e->path, e->code[0] ? e->code : "sin código", e->version, romlist_save_label(e->save),
			    romlist_region_name(e->region), e->size);
	}
	if (roms.skipped)
		salsa64_log("menú: %u archivos con extensión de ROM que no se pudieron leer, muy chicos o sin lugar",
			    roms.skipped);
	menu_init(&menu, &roms, keep ? romlist_find(&roms, keep) : -1, &mn.settings);
	if (keep)
		snprintf(menu.last, sizeof menu.last, "%s", keep);
	menu.dirs[menu.ndirs++] = SALSA64_DATA_DIR;
	menu.dirs[menu.ndirs++] = "/dev_usb000/salsa64 … /dev_usb007/salsa64";
}

/* Un cuadro del menú: todo lo dibuja el RSX (rsxkit/ui.h); el PPE solo
 * arma unos miles de vértices. Con `shot`, además una captura (a la mitad
 * de la resolución de 720p) en ese archivo. */
static void menu_show(const char *shot)
{
	rsxkit_frame_begin(0x000000);
	rsxkit_ui_begin(mn.lw, mn.lh);
	menu_frame(&menu, mn.lw, mn.lh, sysGetSystemTime());
	rsxkit_ui_end();
	if (shot) {
		const rsxkit_screen *sc = rsxkit_screen_info();
		struct salsa64_vi_frame f;
		memset(&f, 0, sizeof f);
		f.width = (uint32_t)(mn.lw / 2);
		f.height = 360;
		rsxkit_rect r = { 0, 0, sc->width, sc->height };
		rsxkit_finish();
		salsa64_screenshot(shot, &f, &r);
		salsa64_log("menú: captura %s", shot);
	}
	rsxkit_frame_end();
}

static void menu_settings_load(void)
{
	char text[512];
	menu_settings_default(&mn.settings);
	FILE *f = fopen(MENU_CFG_PATH, "r");
	if (!f)
		return;
	size_t n = fread(text, 1, sizeof text - 1, f);
	fclose(f);
	menu_settings_parse(&mn.settings, text, n);
}

static void menu_settings_save(void)
{
	char text[512];
	int n = menu_settings_format(&mn.settings, text, sizeof text);
	FILE *f = fopen(MENU_CFG_PATH, "w");
	bool ok = f && fwrite(text, 1, (size_t)n, f) == (size_t)n;
	if (f && fclose(f) != 0)
		ok = false;
	salsa64_log("menú: fondo animado %s%s", mn.settings.waves ? "sí" : "no", ok ? "" : " (no se pudo guardar)");
}

/* Fila a la que va la prueba automática: la próxima ROM de menu-prueba
 * (por nombre de archivo) o «Salir al XMB»; -1 si falta. */
static int menu_test_target(void)
{
	if (mn.next_test >= mn.ntest)
		return menu_row_exit(&menu);
	for (uint32_t i = 0; i < roms.count; i++)
		if (!strcmp(romlist_name(&roms.e[i]), mn.test[mn.next_test]))
			return (int)i;
	return -1;
}

static void menu_test_load(void)
{
	FILE *f = fopen(MENU_TEST_PATH, "r");
	if (f) {
		char line[ROMLIST_PATH_MAX];
		while (mn.ntest < MENU_TEST_MAX && fgets(line, sizeof line, f)) {
			line[strcspn(line, "\r\n")] = '\0';
			if (line[0])
				memcpy(mn.test[mn.ntest++], line, sizeof line);
		}
		fclose(f);
	}
	salsa64_log("menú, prueba automática: %u ROMs que abrir (%s)", mn.ntest, MENU_TEST_PATH);
}

/* Abre la ROM elegida y vuelve cuando el juego termina. */
static enum game_end menu_open(const struct rom_entry *e)
{
	char path[ROMLIST_PATH_MAX], err[160];
	memcpy(path, e->path, sizeof path);
	salsa64_log("menú: abro %s (\"%s\")", path, romlist_display_title(e));
	menu.loading = romlist_display_title(e);
	menu_show(NULL);
	menu.loading = NULL;
	uint32_t size = 0;
	if (!read_rom_file(path, mn.rom, ROM_MAX, &size)) {
		salsa64_log("menú: no se pudo leer %s", path);
		menu_set_status(&menu, "No se pudo leer %s", path);
		mn.games++;
		mn.failures++;
		return GAME_TO_MENU;
	}
	if (!romlist_save_last(LAST_ROM_PATH, path))
		salsa64_log("menú: no se pudo escribir %s", LAST_ROM_PATH);
	game_reset();
	test_flags_from_name(romlist_name(e));
	enum game_end end = run_game(mn.rom, size, path, err, sizeof err);
	bool test = selftest_on || gfxtest_on || audiotest_on;
	bool ok = end != GAME_FAILED && (test ? pres.result == SELFTEST_OK : !pres.autotest || game_auto_ok);
	mn.games++;
	mn.failures += !ok;
	salsa64_log("menú: de vuelta de %s%s", path,
		    end == GAME_FAILED ? " (no arrancó)" : !pres.autotest ? "" : ok ? " (prueba bien)" : " (prueba MAL)");
	if (end != GAME_QUIT) {
		menu_scan(path);  /* de nuevo: pudo entrar o salir una memoria USB */
		if (err[0])
			menu_set_status(&menu, "%s", err);
	}
	return end;
}

/* El menú: lista, elige, abre y vuelve, hasta «Salir al XMB» o «Salir del
 * juego» del sistema. Devuelve 0, o 1 si la prueba automática falló. */
static int menu_loop(void)
{
	const rsxkit_screen *sc = rsxkit_screen_info();
	int rc = rsxkit_ui_init();
	mn.rom = memalign(128, ROM_MAX);
	if (rc != RSXKIT_OK || !mn.rom) {
		salsa64_log("%s: FALLO al preparar el menú (2D del RSX %d, búfer de ROM %s)",
			    pres.autotest ? "RESULTADO frontend" : "menú", rc, mn.rom ? "bien" : "sin memoria");
		free(mn.rom);
		mn.rom = NULL;
		return 1;
	}
	mn.lh = 720.0f;
	mn.lw = sc->aspect > 1.5f ? 1280.0f : 960.0f;
	menu_settings_load();
	load_overscan();
	mn.settings.overscan = overscan_preset();
	salsa64_log("menú: espacio lógico de %.0fx%.0f sobre %ux%u, búfer de ROM de %u MB, fondo animado %s", mn.lw,
		    mn.lh, sc->width, sc->height, ROM_MAX >> 20, mn.settings.waves ? "sí" : "no");

	char last[ROMLIST_PATH_MAX];
	bool have_last = romlist_load_last(LAST_ROM_PATH, last, sizeof last);
	menu_scan(have_last ? last : NULL);
	if (have_last)
		salsa64_log("menú: última ROM %s (%s)", last, romlist_find(&roms, last) >= 0 ? "elegida" : "ya no está");
	if (pres.autotest)
		menu_test_load();

	/* Captura del menú (prueba automática o SALSA64_DATA_DIR/capturas): una
	 * vez, cuando terminó de aparecer. */
	bool want_shot = pres.autotest || file_exists(SHOTS_PATH);
	uint32_t frames = 0;
	int failed = 0;
	while (!app_quit) {
		sysUtilCheckCallback();
		uint32_t buttons;
		update_pads(&buttons);
		if (pres.autotest) {
			int target = menu_test_target();
			if (target < 0) {
				salsa64_log("menú, prueba automática: FALLO, no está %s", mn.test[mn.next_test]);
				failed = 1;
				break;
			}
			if (++mn.idle > MENU_IDLE_FIELDS) {
				salsa64_log("menú, prueba automática: FALLO, el menú no avanzó en %u cuadros", MENU_IDLE_FIELDS);
				failed = 1;
				break;
			}
			/* Primero deja que el menú aparezca entero (para la captura). */
			buttons = frames < 50 ? 0 : menu_autopilot(&menu, target, mn.step++);
		}
		enum menu_action a = menu_input(&menu, buttons, sysGetSystemTime());
		if (a == MENU_SETTINGS) {
			if (menu.set.overscan != mn.settings.overscan)
				overscan_save(menu.set.overscan);
			if (menu.set.waves != mn.settings.waves) {
				mn.settings.waves = menu.set.waves;
				menu_settings_save();
			}
			mn.settings = menu.set;
		}
		if (a == MENU_QUIT) {
			salsa64_log("menú: «Salir al XMB»");
			break;
		}
		if (a == MENU_OPEN) {
			if (menu_open(&roms.e[menu_selected(&menu)]) == GAME_QUIT)
				break;
			mn.next_test++;
			mn.step = mn.idle = 0;
		}
		menu_show(want_shot && ++frames == 45 ? SALSA64_DATA_DIR "/menu.ppm" : NULL);
	}
	if (rsxkit_ui_dropped())
		salsa64_log("menú: %u quads no cupieron en algún cuadro", rsxkit_ui_dropped());
	free(mn.rom);
	mn.rom = NULL;
	if (!pres.autotest)
		return 0;
	bool ok = !failed && !mn.failures && mn.games == (int)mn.ntest;
	salsa64_log("RESULTADO frontend: %s (menú: %d juegos abiertos y cerrados, %d con fallas)", ok ? "OK" : "FALLO",
		    mn.games, mn.failures);
	return ok ? 0 : 1;
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

	char name[80] = "";
	FILE *ef = fopen(EMBED_PATH, "r");
	bool have_embed = ef != NULL;
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

	rdram = memalign(128, SALSA64_RDRAM_MAX);
	for (int i = 0; i < SNAP_SLOTS; i++)
		snap_buf[i] = memalign(128, SNAP_BYTES);
	sys_sem_attr_t sem_attr = { SYS_SEM_ATTR_PROTOCOL, SYS_SEM_ATTR_PSHARED, 0, 0, 0, "wake" };
	if (!rdram || !snap_buf[SNAP_SLOTS - 1] || sysSemCreate(&wake_sem, &sem_attr, 0, 1) != 0) {
		salsa64_log("RESULTADO frontend: FALLO sin memoria");
		salsa64_rdp_shutdown();
		rsxkit_shutdown();
		salsa64_log_shutdown();
		return 1;
	}
	ioPadInit(7);
	load_padmap();
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);

	int status = 0;
	if (have_embed) {
		/* Directo, como siempre: las pruebas automáticas dependen de esto. */
		game_reset();
		uint32_t rom_size = 0;
		const char *source = NULL;
		uint8_t *rom = load_rom(&rom_size, &source);
		if (!rom) {
			salsa64_log("RESULTADO frontend: FALLO sin memoria");
			status = 1;
		} else {
			char err[160];
			status = run_game(rom, rom_size, source, err, sizeof err) == GAME_FAILED;
		}
	} else {
		in_menu = 1;
		res_tag = "prueba del juego";
		salsa64_log("sin %s: menú", EMBED_PATH);
		status = menu_loop();
	}

	/* Después de soltar la HLE (game_close): su fin llama a front_done → wake(). */
	sysSemDestroy(wake_sem);
	salsa64_rdp_shutdown();
	rsxkit_shutdown();
	ioPadEnd();
	salsa64_log("salida normal%s", app_quit ? " («Salir del juego» del sistema)" : "");
	salsa64_log_shutdown();
	return status;
}
