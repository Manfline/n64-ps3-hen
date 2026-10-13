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
 * escribe overscan.cfg, que run_game relee en cada juego) y la resolución de
 * salida (video.cfg; vale al volver a abrir Salsa64). Cruceta para elegir, X para abrir, triángulo
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
 * con "RESULTADO frontend" del conjunto (tools/rpcs3-run.py --menu). Una
 * línea «@video 720|1080 escala» aplica la salida y la escala como «Aplicar
 * ahora», entre juegos.
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
 * Estados guardados (states.c, core/state.h, docs/estados.md): diez
 * ranuras por juego en SALSA64_DATA_DIR/states. SELECT + R1 guarda en la
 * ranura actual, SELECT + L1 carga, SELECT + izquierda/derecha cambia de
 * ranura; mientras SELECT está apretado esos botones no llegan al juego.
 * Se hacen en el límite de un campo con la emulación quieta: el hilo de
 * emulación recoge la tarea del RSP en vuelo y espera; el de presentación
 * vacía las colas, espera al RSX y guarda o carga (state_barrier). Un aviso
 * corto en pantalla (osd.c) dice qué pasó. SELECT + □ abre la lista de
 * trucos del juego con la emulación en pausa (× activa o desactiva, ○ cierra;
 * lo activo se guarda junto al .cht). SELECT + △ muestra u oculta el
 * contador de velocidad (VI/s y %, cuadros del juego y de la pantalla), que
 * se recuerda en menu.cfg.
 *
 * Guardado del cartucho (EEPROM, SRAM, FlashRAM; savefile.c): tipo por
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
 * SELECT + R2 en el juego hace lo mismo en el campo que se está mostrando,
 * a gfxdump-<campo>.bin, con la captura del RSX de ese cuadro al lado
 * (gfxdump-<campo>.ppm): para comparar el RSX con el rasterizador del host.
 *
 * Salir: «Salir al XMB» en el menú, START + SELECT sin menú (con embedded),
 * o "Salir del juego" desde el menú del sistema en cualquier momento.
 * START + SELECT pregunta antes (cuadro «¿Volver al menú?» o «¿Salir al
 * XMB?», × sí, ○ no; «confirmar = no» en menu.cfg sale directo). Con el
 * menú del sistema abierto (botón PS) o el cuadro a la vista, la emulación
 * y el audio quedan en pausa y se sigue presentando el último cuadro
 * (pausectl.h, docs/hilos-ppe.md).
 * Lo que la ROM imprime por el ISViewer va al log.
 */
#include <io/pad.h>
#include <malloc.h>
#include <ppu-lv2.h>
#include <ppu-types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/memory.h>
#include <sys/process.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>

#include <rsxkit/post.h>
#include <rsxkit/rsxkit.h>
#include <rsxkit/ui.h>
#include <spukit/spukit.h>

#include "core/boot.h"
#include "cpu/rec.h"
#include "audio_out.h"
#include "audiotest_rom_bin.h"
#include "cheats.h"
#include "cover.h"
#include "download.h"
#include "gfxtest_rom_bin.h"
#include "i18n.h"
#include "menu.h"
#include "osd.h"
#include "pacer.h"
#include "padmap.h"
#include "pausectl.h"
#include "platform/ps3/autotest.h"
#include "platform/ps3/gfx_spu.h"
#include "platform/ps3/jpeg_spu.h"
#include "platform/ps3/rsp_spu.h"
#include "platform/ps3/log.h"
#include "platform/ps3/rec_cache.h"
#include "frontend/filters/filters.h"
#include "rdp/rdp.h"
#include "rsp/gfx/hle.h"
#include "replay.h"
#include "romlist.h"
#include "savefile.h"
#include "screenshot.h"
#include "selftest.h"
#include "selftest_audio.h"
#include "selftest_gfx.h"
#include "states.h"
#include "version.h"
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
#define CHEATS_DIR    SALSA64_DATA_DIR "/cheats"
#define COVERS_DIR    SALSA64_DATA_DIR "/covers"
#define GAMES_DIR     SALSA64_DATA_DIR "/juegos"
#define MENU_TEST_MAX 8
#define MENU_IDLE_FIELDS 900  /* prueba automática: sin acción en tanto, falla */
#define OVERSCAN_PATH SALSA64_DATA_DIR "/overscan.cfg"
#define VIDEO_CFG_PATH SALSA64_DATA_DIR "/video.cfg"
/* Diagnóstico: con este archivo, el menú del sistema no pausa la emulación
 * (como hasta v0.1.0); los mandos siguen sueltos y el latido mira. */
#define NO_PAUSE_PATH SALSA64_DATA_DIR "/sin-pausa"
#define JPEG_PPE_PATH SALSA64_DATA_DIR "/jpeg-ppe"  /* diagnóstico: la tarea JPEG en el PPE */
/* Filtro de la presentación (frontend/filters): el de menu.cfg («filtro =
 * …»). Con el archivo `filtro` (una clave, p. ej. «crt-mascara»), ese,
 * también en las pruebas automáticas; con `filtros-ciclo`, todos por turnos
 * cada FILTER_CYCLE_US, para medirlos (el log dice cuánto tarda cada uno en
 * el RSX; cada vuelta cambia la reducción entre dos pasadas y una, si no
 * la fija `filtros-reducir`); con `filtros-capturas`, la primera vez de cada
 * filtro guarda la imagen que entra y la que sale (filtro-<clave>-<alto>-
 * fuente.ppm y -pantalla.ppm); con `filtros-reducir` («1» o «2»), la
 * reducción en esas pasadas; con `filtros-spe`, la señal y la
 * decodificación de S-Video y compuesto en dos SPEs (con el ciclo, cada
 * vuelta las alterna entre los SPEs y el RSX); con `capturas-pantalla`, las capturas
 * periódicas guardan además la pantalla entera como se ve (con el filtro)
 * en pantalla-N.ppm. */
#define FILTER_PATH        SALSA64_DATA_DIR "/filtro"
#define FILTER_CYCLE_PATH  SALSA64_DATA_DIR "/filtros-ciclo"
#define FILTER_SHOTS_PATH  SALSA64_DATA_DIR "/filtros-capturas"
#define FILTER_REDUCE_PATH SALSA64_DATA_DIR "/filtros-reducir"
#define FILTER_SPE_PATH    SALSA64_DATA_DIR "/filtros-spe"
#define FILTER_CYCLE_US    20000000u
/* Preparar un filtro que tarde más que esto (ms) se avisa en pantalla. */
#define FILTER_NOTICE_MS   33u
/* Con un CRT la escala interna no pasa de esta (lo elegido menor se
 * respeta): el filtro ve la imagen a la resolución de la consola y más
 * escala solo le llega como supermuestreo; a 3× ya no se nota y a 5× el
 * RSX no alcanza a 1080p (docs/perf-log.md, «Filtros CRT en el RSX»). */
#define CRT_SCALE_MAX      3
#define SHOTS_FULL_PATH   SALSA64_DATA_DIR "/capturas-pantalla"
#define FILTER_VIEW_PATH  SALSA64_DATA_DIR "/filtro-ver"
/* Con este archivo, la RDRAM emulada sale de sysMemoryAllocate con páginas
 * de 1 MB en vez del heap (memalign): para medir si el PPE pierde tiempo en
 * fallos de TLB (docs/perf-log.md). El tamaño de página real de cada zona
 * se anota en el log al arrancar (sys_memory_get_page_attribute). */
#define RDRAM_1M_PATH SALSA64_DATA_DIR "/rdram-1m"
/* Perfil persistente del recompilador (cpu/rec_profile->h): un archivo por
 * juego en PROFILE_DIR con cuántas veces entra la ejecución a cada bloque;
 * en la sesión siguiente los bloques calientes se compilan juntos. Apagado
 * por omisión (medido en la PS3 el 2026-10-12: contar cuesta ~5 % y juntar
 * los calientes no gana nada; docs/perf-log.md): el archivo `perfil-activo`
 * lo enciende, y entonces se reserva la tabla y el código generado cuenta;
 * `perfil.cfg` (`caliente = KB`, `saltos = si|no`) dimensiona la zona
 * caliente y el registro de sitios (docs/uso-ps3.md, docs/rec-ppe.md). */
#define PROFILE_DIR      SALSA64_DATA_DIR "/perfiles"
#define PROFILE_ON_PATH  SALSA64_DATA_DIR "/perfil-activo"
#define PROFILE_CFG_PATH SALSA64_DATA_DIR "/perfil.cfg"
#define PROFILE_HOT_KB   1024u            /* zona caliente por omisión */
#define PROFILE_SAVE_US  120000000ull     /* escritura periódica, además de la del cierre */
#define PROFILE_FOLD_FIELDS 60            /* campos entre sumas de rec->hits[] al perfil */
#define PROFILE_STEP_ENTRIES 512          /* entradas escritas por vuelta, si escribe el hilo de presentación */
#define PROFILE_IO_PRIO  1500             /* el hilo que escribe el perfil: por debajo de todos */
#define PROFILE_IO_STACK (64u << 10)
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
#define STILL_US     16000 /* en pausa, el último cuadro se vuelve a presentar cada tanto (flip) */
#define OVERLAY_US   10000 /* con el sistema encima, el menú atiende eventos y mandos cada tanto */
#define SLOW_FLIP_US 100000 /* un flip que tarda más, al log */
/* Latido: un hilo aparte mira que los dos se muevan y, si uno se queda
 * quieto más de STALL_US, dice dónde (docs/hilos-ppe.md). */
#define WATCH_PRIO   500
#define WATCH_US     500000u
#define STALL_US     2000000u
#define STALL_LOG_US 5000000u

static struct salsa64_n64 n64;
/* Trucos del juego que corre (cheats.h): se cargan al abrirlo y el hilo de
 * emulación los aplica al final de cada campo. */
static struct cheat_set game_cheats;
static struct salsa64_rec rec;
static struct salsa64_rec_prof rec_prof, rec_prof_prev;  /* perfil del recompilador (SALSA64_RECPROF_FLAG) */
static bool use_rec;  /* el CPU corre con el recompilador */
static struct salsa64_rec_profile *rec_profile;         /* perfil persistente por juego (1,4 MB; solo con perfil-activo) */
static struct salsa64_rec_profile_writer rec_profile_w; /* su escritura por trozos, si la hace el hilo de presentación */
static struct {
	bool on;                  /* hay perfil para este juego (rec.profile) */
	char path[SALSA64_RPF_PATH_MAX];
	uint64_t t_saved;         /* cuándo empezó la última escritura periódica */
	uint64_t session_saved;   /* rec_profile->session en esa escritura */
	uint32_t fold_fields;     /* campos desde la última suma al perfil */
	volatile uint32_t saves, save_errors;
	/* Hilo propio de baja prioridad para escribir el archivo (el disco
	 * bloquea; la presentación no debe esperarlo): la presentación solo le
	 * avisa por el semáforo. Si no se pudo crear, escribe la presentación por
	 * trozos. */
	bool thread_ok;
	volatile int stop;        /* al cerrar: que termine */
	volatile int due;         /* hay una escritura pedida */
	sys_ppu_thread_t tid;
	sys_sem_t sem;
} rprof;
/* Muestreo por tiempo del recompilador (rec_sample.h; archivo `muestreo`):
 * un hilo de prioridad SAMPLE_PRIO, por debajo de emulación y presentación
 * (corre en el hilo de hardware que esté libre: el de presentación duerme
 * el 85 %), lee cada SAMPLE_US dónde está el hilo de emulación. Al cerrar el
 * juego, los 30 bloques con más muestras van al log. */
#define SAMPLE_PRIO 1100
#define SAMPLE_US   200u
static struct salsa64_rec_sample *rec_sample;  /* ~1 MB, solo con el archivo */
static struct {
	bool on;
	volatile int stop;
	sys_ppu_thread_t tid;
	uint64_t t0;
	struct salsa64_rec_sample_prev prev;  /* para el reparto de cada ventana de estadísticas */
} rsmp;
static uint8_t isv_buf[SALSA64_ISV_SIZE] __attribute__((aligned(16)));
static struct salsa64_save save;
static struct savefile save_file;
static uint8_t *rdram;
static volatile int running;    /* la sesión de juego sigue */
static volatile int emu_done;   /* el hilo de emulación salió de su bucle */
static volatile int app_quit;   /* «Salir del juego» del sistema: salir de la aplicación */
/* Pausa (pausectl.h): el menú del sistema y el cuadro de START + SELECT.
 * pause_ctl es solo del hilo de presentación (el callback de sysutil corre
 * en él); pause_req es lo que pide a emulación, que se detiene al empezar
 * el campo siguiente y lo dice con emu_paused. */
static struct pausectl pause_ctl;
static volatile uint32_t pause_req;
static volatile int emu_paused;
static uint64_t sys_menu_t0;    /* cuándo se abrió el menú del sistema */
static bool sys_no_pause;       /* NO_PAUSE_PATH: el menú del sistema no pausa */
static volatile int emu_live;   /* el hilo de emulación existe (el latido lo espera) */
/* Dónde anda cada hilo, para el latido (watch_thread). */
static struct {
	volatile uint32_t pres, emu;  /* vueltas */
	const char *volatile pres_at, *volatile emu_at;
	volatile int quit;
} beat = { 0, 0, "arranque", "-", 0 };
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
static struct salsa64_jpeg_spu jpeg_spu;
static int jpeg_on_spu;  /* las tareas JPEG van al SPE (jpeg_spu) */
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

/* Diagnóstico de la pantalla: cuántos cuadros faltan para leer dos píxeles
 * del cuadro dibujado (frame_end); al arrancar y al volver del sistema. */
static uint32_t screen_probe = 30;

/* Cuando el sistema deja de estar encima: el estado del RSX de la
 * aplicación puede no ser el que rsxkit da por puesto (después de su menú,
 * la PS3 quedaba en negro aunque se siguiera presentando). */
static void system_overlay_off(void)
{
	int bad = rsxkit_after_system();
	salsa64_log("sistema: al volver, estado del RSX puesto de nuevo; %d programas cambiados en su memoria, %d mapeos "
		    "de la memoria principal distintos", bad, rsxkit_main_maps_bad());
	screen_probe = 30;  /* medio segundo después: lo que quedó en pantalla */
}

/* Eventos del sistema. Corre dentro de sysUtilCheckCallback, en el hilo
 * de presentación (o el del menú): puede tocar pause_ctl y escribir al log. */
static void sysutil_cb(u64 status, u64 param, void *user)
{
	(void)user;
	uint64_t now = sysGetSystemTime();
	bool was_over = pausectl_overlay(&pause_ctl);
	switch (status) {
	case SYSUTIL_EXIT_GAME:
		salsa64_log("sistema: «Salir del juego»");
		app_quit = 1;
		running = 0;
		break;
	case SYSUTIL_MENU_OPEN:
		pausectl_system_menu(&pause_ctl, true);
		sys_menu_t0 = now;
		if (emu_live)
			salsa64_log("sistema: menú abierto (campo %u)", n64.bus.vi.fields);
		else
			salsa64_log("sistema: menú abierto (sin juego)");
		break;
	case SYSUTIL_MENU_CLOSE:
		pausectl_system_menu(&pause_ctl, false);
		salsa64_log("sistema: menú cerrado tras %.1f s", sys_menu_t0 ? (now - sys_menu_t0) / 1e6 : 0.0);
		break;
	case SYSUTIL_DRAW_BEGIN:
		pausectl_system_draw(&pause_ctl, true);
		salsa64_log("sistema: empieza a dibujar encima");
		break;
	case SYSUTIL_DRAW_END:
		pausectl_system_draw(&pause_ctl, false);
		salsa64_log("sistema: deja de dibujar encima");
		break;
	default:
		salsa64_log("sistema: evento 0x%llx (parámetro 0x%llx)", (unsigned long long)status,
			    (unsigned long long)param);
		break;
	}
	if (was_over && !pausectl_overlay(&pause_ctl))
		system_overlay_off();
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
#define FIELD_STATE 0x4u  /* emulación se detiene después de este campo (estado guardado): miniatura */
#define FIELD_KEEP  (FIELD_CHECK | FIELD_SHOT | FIELD_STATE)
#define GFX_WIN_FIELDS 100
static uint32_t cap_every, exit_field;  /* capturas = "N M" */
static uint32_t trace_vi_left;          /* SALSA64_DATA_DIR/trace-vi: campos que se registran al presentar */
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
	for (unsigned spins = 0;; spins++) {
		uint32_t s1 = pad_box.seq;
		if (s1 & 1) {
			/* Presentación está escribiendo: son unos nanosegundos, salvo
			 * que el sistema la haya desalojado a la mitad; este hilo tiene
			 * más prioridad, así que no se gira para siempre. */
			if (spins > 64)
				sysUsleep(20);
			continue;
		}
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

/* ---- Cuánto le quita el hilo de presentación a la emulación ------------------
 * (docs/hilos-ppe.md). Los dos hilos comparten el núcleo del PPE: lo que
 * hace presentación le quita ciclos a emulación. Presentación publica cuánto
 * lleva despierta (ticks del timebase; dormida = el semáforo y la espera del
 * flip anterior) y emulación mide, en cada campo, cuánto se solapó con su
 * trabajo; las ventanas de 5 s agrupan los campos por solape y dan, para
 * cada grupo, ms de emulación y ns por ciclo emulado fuera de los bucles de
 * espera (lo que no depende de cuánto trabajo traiga el campo). Con
 * SALSA64_DATA_DIR/carga-hilo1 («A B [KB]», por omisión 1000 3000 512),
 * además, trabajo de mentira por campo presentado que recorre KB de memoria
 * (512: toda la L2), en fases de medio segundo: 0, A y B µs; el log compara
 * los campos de cada fase. Sin el archivo, solo los contadores. */
#define LOAD_PATH SALSA64_DATA_DIR "/carga-hilo1"
#define LOAD_KB_DEFAULT 512u
#define LOAD_PHASES 3
#define OVL_GROUPS 4

static inline uint64_t tb_now(void)
{
	uint64_t t;
	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

/* Un contador de ticks que escribe presentación y lee emulación: lo hecho
 * hasta `since` y desde cuándo corre (0: parado). */
struct busy_clock {
	volatile uint64_t done, since;
};

static void busy_start(struct busy_clock *c, uint64_t t)
{
	c->since = t;
}

static void busy_stop(struct busy_clock *c, uint64_t t)
{
	if (!c->since)
		return;
	c->done += t - c->since;
	lwsync();
	c->since = 0;
}

/* Desde el otro hilo: lo hecho hasta `t`. */
static uint64_t busy_at(const struct busy_clock *c, uint64_t t)
{
	for (;;) {
		uint64_t since = c->since;
		lwsync();
		uint64_t done = c->done;
		lwsync();
		if (since == c->since)
			return done + (since && t > since ? t - since : 0);
	}
}

/* Campos agrupados por solape (emulación; presentación los lee cada 5 s). */
struct ovl_group {
	uint64_t fields, tb_emu, tb_ovl, tb_load, cycles, sleep_us;
};

static struct {
	struct busy_clock busy, load_clock;  /* presentación despierta; carga de mentira */
	bool load;                     /* carga-hilo1 presente */
	uint32_t load_us[LOAD_PHASES], load_kb;  /* µs por campo de cada fase (la primera, 0) */
	uint8_t *load_buf;
	uint64_t load_next;            /* timebase del próximo cambio de fase */
	volatile uint32_t load_phase;  /* fase en curso (la lee emulación) */
	/* Presentación, ventana en curso (ticks): flip anterior (dormida),
	 * flush (traducción y lo que cae dentro), presentar el campo (sin el
	 * flush ni el flip anterior) y la carga de mentira; lo de rdp.tb que
	 * cayó dentro de los flush. */
	uint64_t t_flip, t_flush, t_present, t_load;
	struct salsa64_rdp_tb in_flush;
	/* Emulación: por solape (< 0,1 ms, 0,1–1, 1–3, ≥ 3) y por fase de la
	 * carga de mentira (la misma al empezar y al terminar el campo). */
	struct ovl_group by_ovl[OVL_GROUPS], by_load[LOAD_PHASES];
} h1;

static uint8_t h1_load_mem[LOAD_KB_DEFAULT << 10] __attribute__((aligned(128)));

static void load_config(void)
{
	FILE *f = fopen(LOAD_PATH, "r");
	h1.load = false;
	h1.load_phase = 0;
	h1.load_next = 0;
	if (!f)
		return;
	h1.load = true;
	h1.load_us[0] = 0;
	h1.load_kb = LOAD_KB_DEFAULT;
	int n = fscanf(f, "%u %u %u", &h1.load_us[1], &h1.load_us[2], &h1.load_kb);
	fclose(f);
	if (n < 1)
		h1.load_us[1] = 1000;
	if (n < 2)
		h1.load_us[2] = 3 * h1.load_us[1];
	if (n < 3 || !h1.load_kb || h1.load_kb > LOAD_KB_DEFAULT)
		h1.load_kb = LOAD_KB_DEFAULT;
	h1.load_buf = h1_load_mem;
	salsa64_log("carga-hilo1: trabajo de mentira por campo presentado sobre %u KB, en fases de medio segundo de 0, %u "
		    "y %u µs (diagnóstico, %s)", h1.load_kb, h1.load_us[1], h1.load_us[2], LOAD_PATH);
}

/* La carga de mentira: lee y escribe una línea de caché por vuelta sobre
 * load_kb, como la traducción que recorre la salida de la HLE y escribe
 * comandos del RSX, hasta gastar el tiempo pedido. */
static void h1_load_run(uint32_t us)
{
	static uint32_t pos;
	uint64_t hz = spukit_tick_hz(), t0 = tb_now(), end = t0 + (uint64_t)us * hz / 1000000u;
	uint32_t words = h1.load_kb << 7, acc = 0;  /* palabras de 8 bytes */
	volatile uint64_t *p = (volatile uint64_t *)h1.load_buf;
	busy_start(&h1.load_clock, t0);
	do {
		for (int k = 0; k < 256; k++) {
			acc += (uint32_t)p[pos];
			p[(pos + 8) % words] = acc;
			pos = (pos + 16) % words;  /* 128 bytes: la línea siguiente */
		}
	} while (tb_now() < end);
	uint64_t t1 = tb_now();
	busy_stop(&h1.load_clock, t1);
	h1.t_load += t1 - t0;
}

/* Al terminar cada campo (emulación). phase: la fase de la carga de
 * mentira durante todo el campo (-1: cambió en medio o sin carga). */
static void ovl_note(uint64_t tb_emu, uint64_t tb_ovl, uint64_t tb_load, uint64_t cycles, uint64_t sleep_us, int phase)
{
	uint64_t ms = spukit_tick_hz() / 1000u;
	int g = tb_ovl < ms / 10 ? 0 : tb_ovl < ms ? 1 : tb_ovl < 3 * ms ? 2 : 3;
	/* Por solape, solo los campos sin carga de mentira: así una corrida con
	 * carga-hilo1 da las dos mediciones. */
	struct ovl_group *o[2] = { tb_load || phase > 0 ? NULL : &h1.by_ovl[g], NULL };
	if (phase >= 0)
		o[1] = &h1.by_load[phase];
	for (int i = 0; i < 2; i++) {
		if (!o[i])
			continue;
		o[i]->fields++;
		o[i]->tb_emu += tb_emu;
		o[i]->tb_ovl += tb_ovl;
		o[i]->tb_load += tb_load;
		o[i]->cycles += cycles;
		o[i]->sleep_us += sleep_us;
	}
}

/* «n campos: x ms de emulación, y ns por ciclo, solape z ms» de un grupo
 * (diferencia con la ventana anterior). */
static int ovl_text(char *out, size_t n, const struct ovl_group *a, const struct ovl_group *b, bool load)
{
	uint64_t f = a->fields - b->fields, cyc = a->cycles - b->cycles;
	double k = 1e3 / spukit_tick_hz();  /* ticks → ms */
	if (!f)
		return snprintf(out, n, "0");
	double emu = (a->tb_emu - b->tb_emu) * k, sleep = (a->sleep_us - b->sleep_us) / 1e3;
	int w = snprintf(out, n, "%llu (%.2f ms, %.2f ns/ciclo, solape %.2f ms", (unsigned long long)f, emu / f,
			 cyc ? (emu - sleep) * 1e6 / cyc : 0.0, (a->tb_ovl - b->tb_ovl) * k / f);
	if (w > 0 && (size_t)w < n)
		w += snprintf(out + w, n - w, load ? ", carga %.2f ms)" : ")", (a->tb_load - b->tb_load) * k / f);
	return w;
}

/* Petición de presentación a emulación: empezar la medición A/B de vitest. */
static volatile int bench_request;

/* Estados guardados (states.c, core/state.h). Presentación pide guardar o
 * cargar (SELECT + R1 / L1); emulación, al terminar el campo, recoge la
 * tarea del RSP en vuelo, encola el campo con FIELD_STATE (de él sale la
 * miniatura) y espera quieta; presentación vacía las colas, espera al RSX,
 * hace el trabajo y la suelta (state_barrier). */
enum { STATE_NONE, STATE_SAVE, STATE_LOAD };
static volatile int state_req;     /* lo pone presentación; lo baja al terminar */
static volatile int state_parked;  /* emulación espera en el límite del campo */
static volatile int state_quiet;   /* la tarea del RSP quedó recogida (sin LLE) */
static unsigned state_slot;        /* ranura actual, 0 … SALSA64_STATE_SLOTS − 1 */
static struct osd_fps fps;         /* contador de velocidad (SELECT + △, menu.cfg) */
static bool ui_ok;                 /* rsxkit_ui listo: aviso y contador sobre el juego */
static float ui_lw, ui_lh;         /* su espacio lógico */
/* Versión corta (menú y pausa) y, en un paquete de prueba, «título · APPID»
 * (version.h); las arma main() al arrancar. */
static char version_text[64], build_text[160];

/* Sink gráfico del frontend: el del RDP más el timbre a presentación. */
static const struct salsa64_gfx_sink *rdp_sink;

static void front_acquire(void *ctx, struct gfx_job *job)
{
	(void)ctx;
	rdp_sink->acquire(rdp_sink->ctx, job);
}

/* DMA a la RDRAM (hilo de emulación): las copias del RDP no lo pisan. */
static void front_rdram_written(void *ctx, uint32_t pa, uint32_t len)
{
	(void)ctx;
	salsa64_rdp_rdram_written(pa, len, n64.bus.rsp.busy == SALSA64_TASK_GFX);
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

/* ---- Estados guardados y atajos con SELECT (hilo de presentación) ---------- */

static unsigned state_req_slot;  /* la ranura de la petición en curso */
static bool dump_now;            /* SELECT + R2: volcar la HLE en el próximo cuadro */
static int combos_on;            /* dentro del juego (no en el menú) */
static uint32_t state_done;      /* barreras terminadas */
static bool state_last_ok;       /* cómo terminó la última */

/* Prueba automática de los estados (SALSA64_DATA_DIR/estados-prueba = "A B",
 * tools/rpcs3-run.py --estados): guardar en la ranura 9 al llegar al campo
 * A y cargarla en el B; la ROM de prueba tiene que verificarse igual
 * después. */
#define STATE_TEST_PATH SALSA64_DATA_DIR "/estados-prueba"
enum { ST_OFF, ST_SAVE, ST_SAVING, ST_LOAD, ST_LOADING, ST_DONE, ST_FAILED };
static struct {
	uint32_t save_at, load_at, done0;
	int stage;
} state_test;

static void fps_setting_save(bool on);
static bool fps_setting_load(void);
static bool confirm_setting_load(void);

static void state_command(int what)
{
	uint64_t now = sysGetSystemTime();
	if (state_req || state_parked)
		return;  /* ya hay una en curso, o emulación todavía sale de la anterior */
	if (lle_mode != SALSA64_LLE_OFF) {
		osd_notice(now, "Estados no disponibles con lle");
		return;
	}
	if (what == STATE_LOAD && !states_slot_exists(&n64.bus, state_slot)) {
		osd_notice(now, "No hay estado en la ranura %u", state_slot);
		salsa64_log("estado: no hay estado en la ranura %u", state_slot);
		return;
	}
	salsa64_log("estado: %s la ranura %u (pedido en el campo %u)", what == STATE_SAVE ? "guardar en" : "cargar",
		    state_slot, n64.bus.vi.fields);
	state_req_slot = state_slot;
	lwsync();
	state_req = what;
}

static void state_slot_step(int d)
{
	state_slot = (state_slot + SALSA64_STATE_SLOTS + (unsigned)d) % SALSA64_STATE_SLOTS;
	bool have = states_slot_exists(&n64.bus, state_slot);
	osd_notice(sysGetSystemTime(), have ? "Ranura %u" : "Ranura %u (vacía)", state_slot);
}

/* Lo que pidió la lista de trucos del juego (pausectl): activar o desactivar
 * el elegido y avisar de que no hay trucos. */
static bool pm_slot_have;  /* la ranura actual tiene estado (para el menú de pausa) */

static void image_events(void);

/* Lo que pidió el menú de pausa (pausectl): estados, ranura, FPS. */
static void pause_menu_events(void)
{
	static bool was_open;
	if (pause_ctl.menu && !was_open)
		pm_slot_have = states_slot_exists(&n64.bus, state_slot);
	was_open = pause_ctl.menu;
	int d = pause_ctl.menu_do;
	if (d < 0)
		return;
	pause_ctl.menu_do = -1;
	uint64_t now = sysGetSystemTime();
	switch (d) {
	case PM_SAVE:
		state_command(STATE_SAVE);
		break;
	case PM_LOAD:
		state_command(STATE_LOAD);
		break;
	case PM_SLOT:
		state_slot = (state_slot + SALSA64_STATE_SLOTS + (unsigned)pause_ctl.menu_arg) % SALSA64_STATE_SLOTS;
		pm_slot_have = states_slot_exists(&n64.bus, state_slot);
		break;
	case PM_FPS:
		fps.on = !fps.on;
		osd_fps_reset(&fps);
		fps_setting_save(fps.on);
		break;
	case PM_CHEATS:
		osd_notice(now, "No hay trucos para este juego");
		break;
	}
}

static void cheat_panel_events(void)
{
	if (pause_ctl.panel_denied) {
		pause_ctl.panel_denied = false;
		osd_notice(sysGetSystemTime(), "No hay trucos para este juego");
	}
	int t = pause_ctl.toggle;
	if (t < 0)
		return;
	pause_ctl.toggle = -1;
	if ((unsigned)t >= game_cheats.n)
		return;
	game_cheats.c[t].on = !game_cheats.c[t].on;
	bool ok = cheats_save(&game_cheats);
	salsa64_log("trucos: «%s» %s, %u activos%s", game_cheats.c[t].desc, game_cheats.c[t].on ? "activado" : "desactivado",
		    cheats_active(&game_cheats), ok ? "" : " (no se pudo guardar)");
}

/* Atajos dentro del juego, con SELECT (la N64 no lo tiene; START + SELECT
 * vuelve al menú): + R1 guarda el estado en la ranura actual, + L1 lo
 * carga, + izquierda o derecha de la cruceta cambia de ranura, + △
 * muestra u oculta el contador de velocidad y + R2 vuelca la salida de la
 * HLE gráfica con la captura del cuadro. Mientras SELECT está apretado
 * esos botones no llegan al juego. Devuelve los botones para el mapeo. */
#define COMBO_MASK (1u << SALSA64_DS3_R1 | 1u << SALSA64_DS3_L1 | 1u << SALSA64_DS3_LEFT | \
		    1u << SALSA64_DS3_RIGHT | 1u << SALSA64_DS3_TRIANGLE | 1u << SALSA64_DS3_R2)

static uint32_t pad_combos(unsigned port, uint32_t b)
{
	static uint32_t prev[SALSA64_PIF_PORTS];
	uint32_t rise = b & ~prev[port];
	prev[port] = b;
	if (!combos_on || !(b & 1u << SALSA64_DS3_SELECT))
		return b;
	if (rise & 1u << SALSA64_DS3_R1)
		state_command(STATE_SAVE);
	if (rise & 1u << SALSA64_DS3_L1)
		state_command(STATE_LOAD);
	if (rise & 1u << SALSA64_DS3_RIGHT)
		state_slot_step(1);
	if (rise & 1u << SALSA64_DS3_LEFT)
		state_slot_step(-1);
	if (rise & 1u << SALSA64_DS3_TRIANGLE) {
		fps.on = !fps.on;
		osd_fps_reset(&fps);
		osd_notice(sysGetSystemTime(), "FPS: %s", tr(fps.on ? "sí" : "no"));
		fps_setting_save(fps.on);
	}
	if (rise & 1u << SALSA64_DS3_R2)
		dump_now = true;
	return b & ~COMBO_MASK;
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
 * estado. Con el menú del sistema abierto o los mandos tomados por el
 * sistema (bit 0 de padInfo.info), todo suelto: lo que se suelte allá no se
 * ve aquí, así que nada queda pegado al volver. pausectl decide qué llega al
 * juego (nada con el cuadro de salir a la vista; lo apretado al abrirlo o
 * cerrarlo, hasta que se suelte). Devuelve 1 si hay que salir del juego
 * (START + SELECT, confirmado o sin confirmación); en `buttons` (puede ser
 * NULL), los botones de todos los mandos juntos (el menú). */
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
	const struct salsa64_ds3 neutral = { 0, 128, 128, 128, 128 };
	struct salsa64_pad pads[SALSA64_PIF_PORTS];
	uint32_t raw[SALSA64_PIF_PORTS], game[SALSA64_PIF_PORTS];
	padInfo info;
	padData data;
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
	bool intercepted = (info.info & 1) != 0;
	if (intercepted != pause_ctl.intercepted)
		salsa64_log("mandos: %s", intercepted ? "los tiene el sistema" : "de vuelta del sistema");
	bool sys = intercepted || pausectl_overlay(&pause_ctl);
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++) {
		if (present[i] != !!info.status[i]) {
			present[i] = !!info.status[i];
			salsa64_log("mando %u %s", i + 1, present[i] ? "conectado" : "desconectado");
			last[i] = neutral;  /* nada pegado */
		}
		if (info.status[i] && ioPadGetData(i, &data) == 0 && data.len > 0 && !sys)
			last[i] = ds3_from(&data);
		if (sys)
			last[i] = neutral;
		raw[i] = info.status[i] ? last[i].buttons : 0;
	}
	pause_ctl.panel_avail = game_cheats.n > 0;
	pause_ctl.panel_rows = game_cheats.n;
	enum pausectl_action act = pausectl_pads(&pause_ctl, raw, intercepted, combos_on != 0, game);
	cheat_panel_events();
	pause_menu_events();
	image_events();
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++) {
		struct salsa64_pad p = { 0, 0, 0, i == 0 };
		if (info.status[i]) {
			/* Los sticks del DS3 tal como llegan, para revisar el mapeo. */
			const struct salsa64_ds3 *a = &last[i], *b = &raw_logged[i];
			if (abs(a->lx - b->lx) >= 40 || abs(a->ly - b->ly) >= 40 ||
			    abs(a->rx - b->rx) >= 40 || abs(a->ry - b->ry) >= 40) {
				salsa64_log("mando %u: stick izquierdo (%u,%u), derecho (%u,%u)", i + 1,
					    a->lx, a->ly, a->rx, a->ry);
				raw_logged[i] = *a;
			}
			struct salsa64_ds3 d = last[i];
			d.buttons = pad_combos(i, game[i]);
			salsa64_padmap_apply(&padmap, &d, &p);
			all |= game[i];
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
	return act == PAUSECTL_EXIT;
}

/* ---- ROM ------------------------------------------------------------------ */

static int file_exists(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f)
		fclose(f);
	return f != NULL;
}

/* Tamaño de página (KB) de la memoria donde cae `p`, por la syscall 351
 * (sys_memory_get_page_attribute, sin envoltura en PSL1GHT; disposición de
 * sys_page_attr_t como la documenta RPCS3); 0 si falla. */
static struct {
	u64 attribute, access_right;
	u32 page_size, pad;
	u64 reserve[4];
} page_attr;

static s32 page_attr_query(const void *p)
{
	lv2syscall2(351, (u64)(uintptr_t)p, (u64)(uintptr_t)&page_attr);
	return_to_user_prog(s32);
}

static unsigned page_kb(const void *p)
{
	return page_attr_query(p) == 0 ? page_attr.page_size / 1024 : 0;
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
	overscan.left = overscan.right = 8;  /* por omisión (un juego con ajustes propios los cambió) */
	overscan.top = overscan.bottom = 6;
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

/* Tareas gráficas que el RDP ya pudo dibujar, para presentación. Con el
 * RDP congelado (DPC_STATUS FREEZE) la tarea terminada no se ejecuta hasta
 * que el CPU lo descongela (rsp.c retiene su interrupción): Banjo-Kazooie
 * arranca así la del cuadro siguiente sobre el búfer que el VI todavía
 * muestra. Dibujarla antes en el RSX adelantaba ese cuadro un campo y luego
 * el otro búfer mostraba el anterior: los modelos temblaban. */
static volatile uint32_t tasks_visible;

static uint32_t visible_tasks(void)
{
	return salsa64_rdp_tasks_done() - (n64.bus.rsp.dp_held ? 1u : 0u);
}

static void post_field(uint32_t flags)
{
	uint32_t last_idx = post_last;
	struct field_msg m;
	memset(&m, 0, sizeof m);
	m.field = n64.bus.vi.fields;
	m.tasks = visible_tasks();
	tasks_visible = m.tasks;
	m.snap = -1;
	m.flags = flags;

	if (salsa64_vi_frame(&n64.bus, &m.f) && m.f.width <= FB_MAX_W && m.f.height <= FB_MAX_H) {
		salsa64_rdp_vi(m.f.origin, m.f.width);  /* qué color images son framebuffers */
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
			    "anteriores; %.1f al arena, %.1f desalojadas; %.1f sin decodificar, desde un destino), %.1f "
			    "cargas a la TMEM (%.1f lecturas); SPE %.2f ms (vértices %.2f, estado %.2f, identificar texturas "
			    "%.2f, decodificar %.2f, cargas %.2f), PPE esperando %.2f ms, traducción al RSX %.2f ms",
			    gwin.field0, field - 1, (field - gwin.field0) * 1e6 / (now - gwin.t0), gwin.worst / 1e3, tasks,
			    (double)(rs.draws - gwin.rdp.draws) / tasks, (double)(rs.triangles - gwin.rdp.triangles) / tasks,
			    (double)(s->textures - s0->textures) / tasks, (double)(s->tex_reused - s0->tex_reused) / tasks,
			    (double)(s->tex_old - s0->tex_old) / tasks, (double)(s->tex_arena - s0->tex_arena) / tasks,
			    (double)(s->tex_evicted - s0->tex_evicted) / tasks, (double)(s->tex_target - s0->tex_target) / tasks,
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
 * en tanda al cargarla (la HLE corre en un SPE o, con JPEG_PPE_PATH, en el
 * hilo de emulación); al terminar la tanda (un campo sin tareas nuevas), al
 * log cuántas, cuánto tardaron y el tirón: los campos de la tanda y el más
 * lento de emulación (sin la espera del ritmo). */
static struct {
	uint32_t seen, tasks, mb, field0, fields;
	uint64_t us, us_max, spu_ticks, sleep_us, wait_us;
	uint32_t polls, waits, on_ppe;
} jpeg_log;

static uint64_t clock_us(void)
{
	return (uint64_t)sysGetSystemTime();
}

static void log_jpeg(uint64_t us_field)
{
	const struct salsa64_rsp_stats *st = &n64.bus.rsp.stats;
	const struct salsa64_jpeg_spu *js = &jpeg_spu;
	if (st->jpeg < jpeg_log.tasks)
		memset(&jpeg_log, 0, sizeof jpeg_log);  /* otro juego */
	if (st->jpeg != jpeg_log.seen) {
		/* Un campo de la tanda. */
		if (!jpeg_log.fields++)
			jpeg_log.field0 = n64.bus.vi.fields;
		if (us_field > jpeg_log.us_max)
			jpeg_log.us_max = us_field;
	} else if (st->jpeg != jpeg_log.tasks) {
		uint32_t tasks = st->jpeg - jpeg_log.tasks;
		if (jpeg_on_spu)
			salsa64_log("JPEG: %u tareas, %u macrobloques en el SPE: %.2f ms (la más larga %.3f ms), %u devueltas al "
				    "PPE; hilo de emulación %.2f ms (%u esperas de %.2f ms, %u sondeos, %.1f ms dormido); "
				    "campos %u–%u, el más lento %.1f ms de emulación; %u con parámetros malos en total",
				    tasks, st->jpeg_mb - jpeg_log.mb, (js->spu_ticks - jpeg_log.spu_ticks) * 1e3 / spukit_tick_hz(),
				    js->spu_max * 1e3 / spukit_tick_hz(), js->on_ppe - jpeg_log.on_ppe,
				    (st->jpeg_us - jpeg_log.us) / 1e3, js->waits - jpeg_log.waits,
				    (js->wait_us - jpeg_log.wait_us) / 1e3, js->polls - jpeg_log.polls,
				    (js->sleep_us - jpeg_log.sleep_us) / 1e3, jpeg_log.field0,
				    jpeg_log.field0 + jpeg_log.fields - 1, jpeg_log.us_max / 1e3, st->jpeg_bad);
		else
			salsa64_log("JPEG: %u tareas, %u macrobloques en %.2f ms del PPE (hilo de emulación); campos %u–%u, el "
				    "más lento %.1f ms de emulación; %u con parámetros malos en total",
				    tasks, st->jpeg_mb - jpeg_log.mb, (st->jpeg_us - jpeg_log.us) / 1e3, jpeg_log.field0,
				    jpeg_log.field0 + jpeg_log.fields - 1, jpeg_log.us_max / 1e3, st->jpeg_bad);
		jpeg_log.tasks = st->jpeg;
		jpeg_log.mb = st->jpeg_mb;
		jpeg_log.us = st->jpeg_us;
		jpeg_log.spu_ticks = js->spu_ticks;
		jpeg_log.sleep_us = js->sleep_us;
		jpeg_log.wait_us = js->wait_us;
		jpeg_log.polls = js->polls;
		jpeg_log.waits = js->waits;
		jpeg_log.on_ppe = js->on_ppe;
		jpeg_log.fields = 0;
		jpeg_log.us_max = 0;
	}
	jpeg_log.seen = st->jpeg;
}

/* Estado guardado pedido: emulación queda quieta en el límite del campo
 * hasta que presentación termina (state_barrier). */
static void emu_park(void)
{
	beat.emu_at = "estado guardado";
	lwsync();
	state_parked = 1;
	wake();
	while (state_req && running)
		sysUsleep(1000);
	lwsync();
	state_parked = 0;
	/* El reloj siguió mientras tanto: el ritmo empieza de nuevo. */
	pacer_init(&pacer, pacer.enabled, pacer_sleep, NULL);
}

/* Pausa (menú del sistema, cuadro de salir): quieta en el límite del campo
 * hasta que presentación baje pause_req. Las tareas del RSP en vuelo
 * terminan solas en su SPE y su fin emulado llega en el campo siguiente;
 * el AI no entrega nada, y el audio calla aparte (audio_out_pause). */
static void emu_pause(void)
{
	beat.emu_at = "pausa";
	lwsync();
	emu_paused = 1;
	wake();
	while (pause_req && running)
		sysUsleep(2000);
	lwsync();
	emu_paused = 0;
	/* El reloj saltó lo que duró la pausa: el ritmo empieza de nuevo. */
	pacer_init(&pacer, pacer.enabled, pacer_sleep, NULL);
}

static void emu_thread(void *arg)
{
	(void)arg;
	while (running) {
		beat.emu++;
		if (pause_req) {
			emu_pause();
			continue;
		}
		uint64_t t0 = sysGetSystemTime();
		/* Al ritmo de la tele emulada; la medición A/B va al máximo y
		 * queda fuera del ritmo (ni espera ni avanza la referencia). */
		int paced = bench.stage != 1 && bench.stage != 2;
		if (paced) {
			beat.emu_at = "ritmo";
			n64.cpu.rec_mark = SALSA64_SMP_PACE;  /* muestreo (rec_sample.h) */
			pacer_wait(&pacer, t0);
			uint64_t tp = sysGetSystemTime();
			emu_stats.us_pace += tp - t0;
			t0 = tp;
		}
		n64.cpu.rec_mark = SALSA64_SMP_OTHER;
		beat.emu_at = "mandos";
		pads_apply();  /* después de la espera: lo más reciente de los mandos */
		beat.emu_at = "campo";
		uint64_t c0 = n64.cpu.cycles, i0 = n64.cpu.idle_cycles, s0 = gfx_spu.sleep_us + jpeg_spu.sleep_us;
		uint64_t tb0 = tb_now(), o0 = busy_at(&h1.busy, tb0), l0 = busy_at(&h1.load_clock, tb0);
		uint32_t ph0 = h1.load_phase;
		uint64_t cycles = salsa64_n64_run_field(&n64);
		uint64_t tb1 = tb_now();
		ovl_note(tb1 - tb0, busy_at(&h1.busy, tb1) - o0, busy_at(&h1.load_clock, tb1) - l0,
			 (n64.cpu.cycles - c0) - (n64.cpu.idle_cycles - i0), gfx_spu.sleep_us + jpeg_spu.sleep_us - s0,
			 h1.load && h1.load_phase == ph0 ? (int)ph0 : -1);
		cheats_apply(&game_cheats, &n64.bus);  /* los activos del juego (vacío si no hay) */
		uint64_t t1 = sysGetSystemTime();
		n64.cpu.rec_mark = SALSA64_SMP_OTHER;
		beat.emu_at = "fin del campo";
		pif_edges_note();
		log_unknown_gbi();
		log_jpeg(t1 - t0);
		if (paced)
			pacer_advance(&pacer, cycles);
		savefile_stage(&save_file, t1);
		if (rprof.on && ++rprof.fold_fields >= PROFILE_FOLD_FIELDS) {
			salsa64_rec_profile_fold(&rec);  /* entre campos, fuera del código generado */
			rprof.fold_fields = 0;
		}
		/* Estado guardado: la tarea del RSP en vuelo se recoge (su fin
		 * emulado no se adelanta) antes de encolar el campo, así su salida
		 * va con él y las colas quedan vacías en la barrera. */
		int req = state_req;
		if (req)
			state_quiet = salsa64_rsp_quiesce(&n64.bus);
		beat.emu_at = "encolar";
		post_field(req ? FIELD_STATE : 0);
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
		if (req)
			emu_park();
	}
	beat.emu_at = "terminado";
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
	/* Contador de velocidad: cuadros nuevos del juego (cambios del
	 * framebuffer del VI, también en campos saltados) y presentados en
	 * total (no se vacían con las estadísticas). */
	uint32_t last_origin, game_frames, frames_total;
	/* Pausa: qué fue lo último que se vio (para repetirlo, present_still),
	 * cuándo fue el último flip y cuántos cuadros se repitieron. */
	enum { STILL_NONE, STILL_RSX, STILL_FB } still;
	uint64_t t_flip;
	uint32_t still_frames;
} pres = { .view = { RSXKIT_SCALE_FIT, RSXKIT_FILTER_LINEAR, 4.0f / 3.0f } };

/* Filtro de la presentación. Con filtro, salsa64_rdp_present (o el
 * framebuffer 2D) solo elige qué se ve y el filtro lo dibuja al final del
 * cuadro; las capturas, las miniaturas de los estados y las pruebas
 * automáticas leen la imagen sin filtro, que entonces se dibuja antes
 * (filter_plain) y el filtro la tapa. */
static struct {
	int sel;        /* el elegido (menu.cfg o el archivo `filtro`) */
	int cur;        /* el que se usa: sel, o el del ciclo de medición */
	bool cycle;     /* filtros-ciclo */
	int cycle_reduce;  /* el ciclo alterna la reducción (1 o 2 pasadas) en cada vuelta; 0: no */
	bool cycle_spe;    /* el ciclo alterna el SPE en cada vuelta */
	bool spe;          /* la señal del cable va en el SPE ahora */
	int cycle_list[SALSA64_CRTS];  /* los del ciclo, desde «Ninguno» */
	int cycle_n, cycle_i;
	bool full_shots;
	bool announced; /* se avisó «Aplicando filtro…»: la preparación va en este cuadro */
	uint64_t t_cycle;
	bool pending;   /* este cuadro va con filtro: falta dibujarlo */
	bool plain;     /* ... y ya tiene la imagen sin filtro */
	struct salsa64_filter_src src;
	uint32_t full_shot;
} flt;

/* Tiempo del RSX en lo que manda la presentación, con marcas de tiempo
 * (rsxkit/post.h; las de 0 a 23 son de frontend/filters): cada tramo de
 * órdenes (traducir las tareas gráficas de la emulación; presentar un campo
 * con su filtro y lo de encima) entre una marca al empezar y otra al
 * terminar, y una barrera; se suma cuando el RSX pasó la barrera. Es una
 * cota superior: incluye lo que el RSX esperó órdenes del PPE dentro del
 * tramo. */
#define RT_STAMP0 24
#define RT_PAIRS  16
enum { RT_EMU, RT_PRES, RT_KINDS };
static struct {
	uint32_t next;
	uint8_t kind[RT_PAIRS];
	bool pending[RT_PAIRS];
	uint32_t fence[RT_PAIRS];
	uint64_t ns[RT_KINDS];
	uint32_t n[RT_KINDS];
	int open;  /* par abierto, o -1 */
} rt = { .open = -1 };

static void rt_collect(void)
{
	for (uint32_t i = 0; i < RT_PAIRS; i++) {
		if (!rt.pending[i] || !rsxkit_fence_done(rt.fence[i]))
			continue;
		rt.pending[i] = false;
		uint64_t a = rsxkit_stamp_ns(RT_STAMP0 + 2 * i), b = rsxkit_stamp_ns(RT_STAMP0 + 2 * i + 1);
		if (b < a || b - a > 200000000u)
			continue;
		rt.ns[rt.kind[i]] += b - a;
		rt.n[rt.kind[i]]++;
	}
}

static void rt_begin(int kind)
{
	if (rt.open >= 0)
		return;
	rt_collect();
	uint32_t i = rt.next++ % RT_PAIRS;
	rt.pending[i] = false;  /* si seguía en vuelo, se pierde esa medición */
	rt.kind[i] = (uint8_t)kind;
	rt.open = (int)i;
	rsxkit_stamp(RT_STAMP0 + 2 * i);
}

static void rt_end(void)
{
	if (rt.open < 0)
		return;
	uint32_t i = (uint32_t)rt.open;
	rsxkit_stamp(RT_STAMP0 + 2 * i + 1);
	rt.fence[i] = rsxkit_fence();
	rt.pending[i] = true;
	rt.open = -1;
}

static bool filter_tests(void)
{
	return selftest_on || gfxtest_on || audiotest_on;
}

/* La imagen del cuadro sin filtro, si va con filtro y aún no está (antes
 * de una captura). */
static void filter_plain(rsxkit_rect *r)
{
	if (!flt.pending || flt.plain)
		return;
	if (pres.still == STILL_RSX)
		salsa64_rdp_present_again(NULL, NULL, r);
	else if (pres.still == STILL_FB)
		rsxkit_fb_draw(pres.fb, &pres.view, r);
	flt.plain = true;
}

/* La fuente del filtro: lo que eligió salsa64_rdp_present o el framebuffer
 * 2D (`shown`: su tamaño sin el overscan). */
static bool filter_source(bool rsx, const struct salsa64_vi_frame *shown)
{
	struct salsa64_filter_src *s = &flt.src;
	memset(s, 0, sizeof *s);
	s->aspect = pres.view.aspect;
	if (rsx)
		return (s->has_gen = salsa64_rdp_present_source(NULL, NULL, &s->tex, &s->region, &s->factor, &s->gen));
	if (!rsxkit_fb_texture(pres.fb, &s->tex))
		return false;
	s->region = (rsxkit_rect){ 0, 0, shown->width, shown->height };
	s->factor = 1;
	return true;
}

/* La fuente del filtro en la pausa: lo último presentado con la vista y el
 * recorte de ahora («Imagen» del menú de pausa los cambia). */
static bool filter_source_again(const struct salsa64_overscan *crop)
{
	struct salsa64_filter_src *s = &flt.src;
	memset(s, 0, sizeof *s);
	s->aspect = pres.view.aspect;
	return (s->has_gen = salsa64_rdp_present_source(&pres.view, crop, &s->tex, &s->region, &s->factor, &s->gen));
}

/* Si preparar el filtro va a tardar más de un par de cuadros (tablas
 * nuevas, la comprobación de la primera vez), este cuadro va sin filtro
 * con el aviso «Aplicando filtro…» y la preparación va en el siguiente:
 * mientras, se ve el aviso y no una imagen quieta que parece colgada.
 * true: este cuadro va sin filtro. */
static bool filter_announce(uint64_t now)
{
	if (flt.announced || salsa64_filters_setup_ms(flt.cur, &flt.src) <= FILTER_NOTICE_MS)
		return false;
	flt.announced = true;
	osd_notice(now, "Aplicando filtro…");
	return true;
}

/* salsa64_filters_draw del filtro en curso; si se avisó «Aplicando
 * filtro…», ya está: se quita el aviso. */
static bool filter_draw(rsxkit_rect *r)
{
	bool ok = salsa64_filters_draw(flt.cur, &flt.src, r);
	if (flt.announced) {
		flt.announced = false;
		osd_notice_clear();
	}
	return ok;
}

/* Dibuja el filtro del cuadro (o, si no se pudo, la imagen sin filtro). */
static void filter_finish(rsxkit_rect *r)
{
	if (!flt.pending)
		return;
	if (!filter_draw(NULL))
		filter_plain(r);
	flt.pending = false;
}

/* Al abrir un juego: el CRT elegido en el menú (enum salsa64_crt, de
 * load_view), o el del archivo `filtro`, que manda. */
static void filter_setup(int menu_filter)
{
	memset(&flt, 0, sizeof flt);
	flt.sel = menu_filter;
	FILE *f = fopen(FILTER_PATH, "r");
	if (f) {
		char key[32] = "";
		if (fscanf(f, "%31s", key) == 1) {
			int k = salsa64_crt_from_key(key);
			if (k >= 0)
				flt.sel = k;
			else
				salsa64_log("filtros: «%s» en %s no es un filtro", key, FILTER_PATH);
		}
		fclose(f);
	}
	flt.cur = flt.sel;
	/* Diagnóstico: `filtro-ver` con «tabla [escala]» dibuja esa tabla del
	 * filtro en la esquina de la pantalla (frontend/filters/filters.h). */
	f = fopen(FILTER_VIEW_PATH, "r");
	if (f) {
		char name[32] = "";
		float scale = 1.0f;
		if (fscanf(f, "%31s %f", name, &scale) >= 1)
			salsa64_filters_debug(name, scale);
		fclose(f);
	} else {
		salsa64_filters_debug(NULL, 1.0f);
	}
	flt.cycle = file_exists(FILTER_CYCLE_PATH);
	/* El ciclo: todos, o los de la lista que tenga el archivo (claves). */
	flt.cycle_n = 0;
	flt.cycle_list[flt.cycle_n++] = SALSA64_CRT_NONE;
	f = flt.cycle ? fopen(FILTER_CYCLE_PATH, "r") : NULL;
	if (f) {
		char key[32];
		while (flt.cycle_n < SALSA64_CRTS && fscanf(f, "%31s", key) == 1) {
			int k = salsa64_crt_from_key(key);
			if (k > SALSA64_CRT_NONE)
				flt.cycle_list[flt.cycle_n++] = k;
		}
		fclose(f);
	}
	if (flt.cycle_n == 1)
		for (int k = SALSA64_CRT_NONE + 1; k < SALSA64_CRTS; k++)
			flt.cycle_list[flt.cycle_n++] = k;
	flt.cycle_i = 0;
	flt.full_shots = file_exists(SHOTS_FULL_PATH);
	if (flt.cycle)
		flt.cur = SALSA64_CRT_NONE;
	flt.t_cycle = sysGetSystemTime();
	int passes = 2;
	f = fopen(FILTER_REDUCE_PATH, "r");
	if (f) {
		if (fscanf(f, "%d", &passes) != 1 || (passes != 1 && passes != 2))
			passes = 2;
		fclose(f);
	} else if (flt.cycle && !file_exists(FILTER_SPE_PATH)) {
		flt.cycle_reduce = 2;
	}
	salsa64_filters_options(file_exists(FILTER_SHOTS_PATH), passes);
	flt.spe = file_exists(FILTER_SPE_PATH);
	flt.cycle_spe = flt.cycle && flt.spe;
	salsa64_filters_spe(flt.spe);
	salsa64_log("filtros: %s%s%s", flt.cycle ? "ciclo de medición (todos cada 20 s, desde «Ninguno»)" : "",
		    flt.cycle ? (flt.cycle_reduce ? "; cada vuelta cambia la reducción (dos pasadas, una, …)"
				 : flt.cycle_spe ? "; cada vuelta cambia la señal del cable (SPE, RSX, …)" : "")
			      : salsa64_crt_name(flt.cur),
		    flt.full_shots ? "; capturas de la pantalla entera (pantalla-N.ppm)" : "");
}

/* Cambia el CRT en el juego (el menú de pausa): vale desde el próximo
 * cuadro; las tablas se rehacen solas. Con el ciclo de medición, no. */
static void filter_select(int crt)
{
	if (flt.cycle || crt < SALSA64_CRT_NONE || crt >= SALSA64_CRTS || crt == flt.cur)
		return;
	flt.sel = flt.cur = crt;
	flt.announced = false;
	salsa64_log("filtros: «%s»", salsa64_crt_name(crt));
}

/* Ciclo de medición: el siguiente filtro cada FILTER_CYCLE_US. */
static void filter_cycle(uint64_t now)
{
	if (!flt.cycle || now - flt.t_cycle < FILTER_CYCLE_US)
		return;
	flt.t_cycle = now;
	flt.cycle_i = (flt.cycle_i + 1) % flt.cycle_n;
	flt.cur = flt.cycle_list[flt.cycle_i];
	if (flt.cur == SALSA64_CRT_NONE && flt.cycle_reduce) {
		flt.cycle_reduce = 3 - flt.cycle_reduce;
		rsxkit_post_reduce_passes(flt.cycle_reduce);
		salsa64_log("filtros: ciclo, otra vuelta: reducir en %d pasada%s", flt.cycle_reduce,
			    flt.cycle_reduce == 1 ? "" : "s");
	}
	if (flt.cur == SALSA64_CRT_NONE && flt.cycle_spe) {
		flt.spe = !flt.spe;
		salsa64_filters_spe(flt.spe);
		salsa64_log("filtros: ciclo, otra vuelta: la señal del cable en %s", flt.spe ? "el SPE" : "el RSX");
	}
	salsa64_log("filtros: ciclo → «%s»", salsa64_crt_name(flt.cur));
	osd_notice(now, "Filtro: %s", salsa64_crt_name(flt.cur));
}

/* rsxkit_frame_begin y _end con el tiempo que se esperó el flip anterior:
 * si pasa de SLOW_FLIP_US, al log con los vblanks que pasaron desde que se
 * pidió (con el sistema encima, la PS3 retenía los flips segundos: por eso
 * entonces solo se presenta con rsxkit_frame_ready). */
static uint64_t flip_vbl;  /* vblanks al pedir el último flip */

static void flip_wait_note(uint64_t t0, const char *where)
{
	uint64_t dt = sysGetSystemTime() - t0;
	if (dt > SLOW_FLIP_US)
		salsa64_log("flip: %.0f ms esperando en «%s» (sistema %s; %llu vblanks desde que se pidió)", dt / 1e3,
			    where, pausectl_overlay(&pause_ctl) ? "encima" : "sin dibujar",
			    (unsigned long long)(rsxkit_vblank_count() - flip_vbl));
}

static void frame_begin(const char *where)
{
	uint64_t t0 = sysGetSystemTime(), tb0 = tb_now();
	beat.pres_at = where;
	busy_stop(&h1.busy, tb0);
	rsxkit_frame_begin(0x000000);
	uint64_t tb1 = tb_now();
	busy_start(&h1.busy, tb1);
	h1.t_flip += tb1 - tb0;
	flip_wait_note(t0, where);
}

/* salsa64_rdp_flush con su tiempo y lo de rdp.tb que cae dentro. */
static uint32_t h1_flush(uint32_t upto)
{
	struct salsa64_rdp_tb a, b;
	salsa64_rdp_get_tb(&a);
	uint64_t t0 = tb_now();
	rt_begin(RT_EMU);
	uint32_t n = salsa64_rdp_flush(upto);
	rt_end();
	h1.t_flush += tb_now() - t0;
	salsa64_rdp_get_tb(&b);
	h1.in_flush.translate += b.translate - a.translate;
	h1.in_flush.copy += b.copy - a.copy;
	h1.in_flush.tex += b.tex - a.tex;
	h1.in_flush.fence += b.fence - a.fence;
	return n;
}

static void frame_end(const char *where)
{
	if (screen_probe && --screen_probe == 0) {
		uint32_t pitch;
		const rsxkit_screen *sc = rsxkit_screen_info();
		rsxkit_finish();
		const volatile uint32_t *px = rsxkit_back_buffer(&pitch);
		uint32_t w = pitch / 4;
		salsa64_log("pantalla: cuadro de «%s»: píxel central %08x, (%u,%u) %08x, (%u,%u) %08x", where,
			    px[sc->height / 2 * w + sc->width / 2], sc->width / 8, sc->height / 8,
			    px[sc->height / 8 * w + sc->width / 8], sc->width / 2, sc->height / 8,
			    px[sc->height / 8 * w + sc->width / 2]);
	}
	uint64_t t0 = sysGetSystemTime();
	beat.pres_at = where;
	rsxkit_frame_end();
	flip_wait_note(t0, where);
	flip_vbl = rsxkit_vblank_count();
}

/* Lo que va encima del juego: aviso, contador y, si está, el cuadro de
 * START + SELECT. */
static int scan_level;       /* 0 sin líneas, 1 suaves, 2 fuertes (filtro del juego) */
static rsxkit_rect scan_r;   /* dónde quedó la imagen, en puntos de la pantalla */
static uint32_t scan_lines;  /* líneas de la imagen del juego */
static struct salsa64_vi_frame still_f;  /* lo último presentado, para su recorte en la pausa */

/* La imagen del juego que corre (load_view; la página «Imagen» del menú de
 * pausa la cambia en vivo, image_events): aspecto, filtro, recorte
 * (MENU_OVERSCAN_*; CUSTOM es el de overscan.cfg) y escala interna, lo
 * global de cada uno (Opciones) y los ajustes del juego como están en
 * juegos/<ROM>.cfg. Sin `source` (las pruebas) no se guardan. */
static struct {
	int aspect, filter, crop, scale, density;
	int g_aspect, g_filter, g_crop, g_scale, g_density;
	struct menu_game game;
	const char *source;
} img;
static int rdp_scale_req;  /* escala interna pedida al RDP ahora (0: automática) */
static struct menu_settings video_cfg;  /* video y escala que corren (video.cfg al arrancar o lo último aplicado) */
static int scale_wanted(void);
static const char *scale_label(char *buf, size_t n);

static void present_overlay(uint64_t now)
{
	if (!ui_ok)
		return;
	if (scan_level && scan_r.w && scan_lines) {
		const rsxkit_screen *sc = rsxkit_screen_info();
		float k = ui_lw / (float)sc->width;
		osd_scanlines_set(scan_level, (float)scan_r.x * k, (float)scan_r.y * k, (float)scan_r.w * k,
				  (float)scan_r.h * k, scan_lines > 300 ? scan_lines / 2 : scan_lines);
	} else {
		osd_scanlines_set(0, 0.0f, 0.0f, 0.0f, 0.0f, 0);
	}
	osd_draw(&fps, now, ui_lw, ui_lh);
	if (pause_ctl.menu) {
		struct osd_pause_menu pm = { pause_ctl.menu_sel, state_slot, pm_slot_have, fps.on, in_menu,
					     game_cheats.n, cheats_active(&game_cheats), version_text };
		osd_pause_menu_draw(&pm, ui_lw, ui_lh);
	} else if (pause_ctl.image) {
		char sbuf[24];
		struct osd_image_menu im = {
			pause_ctl.image_sel,
			{ menu_filter_name(img.filter), menu_density_name(img.density), menu_aspect_name(img.aspect),
			  menu_overscan_name(img.crop), scale_label(sbuf, sizeof sbuf) },
			menu_filter_has_density(img.filter),
			scale_wanted() != rdp_scale_req, img.source != NULL, menu_filter_credit(img.filter),
		};
		osd_image_menu_draw(&im, ui_lw, ui_lh);
	} else if (pause_ctl.dialog)
		osd_confirm_draw(in_menu ? "¿Volver al menú?" : "¿Salir al XMB?", "×: Sí     ○: No", ui_lw, ui_lh);
	else if (pause_ctl.panel)
		osd_cheats_draw(&game_cheats, pause_ctl.panel_sel, ui_lw, ui_lh);
}

/* Presenta (o salta) un campo encolado. */
static void present_field(const struct field_msg *m, uint64_t now)
{
	h1_flush(m->tasks);  /* las tareas de este campo, no las del siguiente */
	if (m->f.width && m->f.origin != pres.last_origin) {
		pres.last_origin = m->f.origin;
		pres.game_frames++;
	}
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
	frame_begin("flip anterior");
	rt_begin(RT_PRES);
	/* Lo que se ve: la imagen del VI sin el overscan (las capturas van de
	 * ese tamaño). */
	struct salsa64_overscan crop = overscan_for(&m->f);
	struct salsa64_vi_frame shown = m->f;
	shown.width -= crop.left + crop.right;
	shown.height -= crop.top + crop.bottom;
	pres.still = STILL_NONE;
	bool filt = flt.cur != SALSA64_CRT_NONE && !filter_tests();
	flt.pending = flt.plain = false;
	salsa64_rdp_present_draw(!filt);
	if (m->f.width) {
		if (salsa64_rdp_present(&m->f, m->field, &pres.view, &crop, &r)) {
			pres.rsx_frames++;
			pres.still = STILL_RSX;
			flt.pending = filt && filter_source(true, &shown) && !filter_announce(now);
			if (filt && !flt.pending)
				salsa64_rdp_present_again(NULL, NULL, &r);
			if (gfxtest_on && pres.result == SELFTEST_RUNNING)
				check_gfx = gfxtest_want_screen();
		} else if (have) {
			check = (m->flags & FIELD_CHECK) != 0;
			uint32_t line = m->f.width * m->f.bpp;
			rsxkit_fb_upload(pres.fb, snap_buf[slot] + crop.top * line + crop.left * m->f.bpp, shown.width,
					 shown.height, line, m->f.bpp == 4 ? RSXKIT_RGBA8888 : RSXKIT_RGBA5551);
			pres.still = STILL_FB;
			flt.pending = filt && filter_source(false, &shown) && !filter_announce(now);
			if (!flt.pending)
				rsxkit_fb_draw(pres.fb, &pres.view, &r);
		} else {
			pres.blank++;
		}
	} else {
		pres.blank++;
	}
	if (trace_vi_left && m->f.width) {
		int tslot;
		uint32_t age, taddr;
		salsa64_rdp_last_present(&tslot, &age, &taddr);
		trace_vi_left--;
		salsa64_log("vi campo %u: origen %06x %ux%u → %s (destino %06x ranura %d, dibujado hace %u campos; emulación %u "
			    "campos por delante)",
			    m->field, m->f.origin, m->f.width, m->f.height,
			    pres.still == STILL_RSX ? "RSX" : pres.still == STILL_FB ? "RDRAM" : "nada", taddr, tslot, age,
			    behind);
	}
	if (have)
		snap_state[slot] = SNAP_FREE;  /* ya está en la memoria del RSX */
	if (check || check_gfx) {
		filter_plain(&r);
		rsxkit_finish();
		if (check)
			selftest_screen(&m->f, &r);
		else
			gfxtest_screen(&m->f, &r);
	} else if ((m->flags & FIELD_SHOT) && m->f.width) {
		char path[64];
		snprintf(path, sizeof path, SALSA64_DATA_DIR "/campo-%u.ppm", m->field);
		filter_plain(&r);
		rsxkit_finish();
		salsa64_screenshot(path, &shown, &r);
		salsa64_log("captura %s", path);
	} else if (pres.shots_on && !cap_every && !selftest_on && !gfxtest_on && !audiotest_on && m->f.width &&
		   (pres.autotest ? m->field / AUTO_SHOT_FIELDS != pres.shots : now - pres.t_shot >= SHOT_US)) {
		char path[64];
		snprintf(path, sizeof path, SALSA64_DATA_DIR "/screen-%u.ppm", pres.shots % SHOT_MAX + 1);
		bool with_filter = flt.pending;
		filter_plain(&r);
		rsxkit_finish();
		salsa64_screenshot(path, &shown, &r);
		salsa64_log("captura %s (campo %u)", path, m->field);
		if (with_filter && flt.full_shots) {
			/* La pantalla entera con el filtro (diagnóstico). */
			filter_finish(&r);
			rsxkit_finish();
			snprintf(path, sizeof path, SALSA64_DATA_DIR "/pantalla-%u.ppm", pres.shots % SHOT_MAX + 1);
			salsa64_screenshot_full(path);
			salsa64_log("captura %s (con «%s»)", path, salsa64_crt_name(flt.cur));
		}
		pres.shots = pres.autotest ? m->field / AUTO_SHOT_FIELDS : pres.shots + 1;
		pres.t_shot = now;
	}
	if ((m->flags & FIELD_STATE) && state_req == STATE_SAVE) {
		/* La miniatura del estado: la imagen sin el aviso encima. */
		filter_plain(&r);
		rsxkit_finish();
		states_thumb_capture(&r);
	}
	if (pres.dump_field && m->field >= pres.dump_field) {
		filter_plain(&r);
		rsxkit_finish();
		salsa64_log("volcado de la HLE en el campo %u: %s", m->field,
			    salsa64_rdp_dump(SALSA64_DATA_DIR "/gfxdump.bin") ? "bien" : "FALLO");
		pres.dump_field = 0;
	}
	if (dump_now && m->f.width) {
		char path[64], shot[64];
		dump_now = false;
		snprintf(path, sizeof path, SALSA64_DATA_DIR "/gfxdump-%u.bin", m->field);
		snprintf(shot, sizeof shot, SALSA64_DATA_DIR "/gfxdump-%u.ppm", m->field);
		filter_plain(&r);
		rsxkit_finish();
		bool ok = salsa64_rdp_dump(path);
		salsa64_screenshot(shot, &shown, &r);
		salsa64_log("volcado de la HLE (SELECT + R2) en %s con la captura %s: %s", path, shot,
			    ok ? "bien" : "FALLO");
		osd_notice(sysGetSystemTime(), ok ? "Volcado del campo %u" : "Volcado: falló", m->field);
	}
	filter_finish(&r);
	if (r.w) {
		scan_r = r;
		scan_lines = shown.height;
		still_f = m->f;
	}
	present_overlay(sysGetSystemTime());
	/* audio_out: una vez por campo presentado (estadísticas y volcado). */
	uint64_t t1 = sysGetSystemTime();
	audio_out_field(t1);
	rt_end();
	frame_end("flip");
	beat.pres_at = "cola";
	pres.frames_total++;
	uint64_t t2 = sysGetSystemTime();
	pres.t_flip = t2;
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
		if (pres.result == SELFTEST_OK && state_test.stage && state_test.stage != ST_DONE) {
			salsa64_log("estados, prueba automática: la ROM terminó antes de cargar el estado");
			pres.result = SELFTEST_FAILED;
		}
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

/* En pausa no llegan campos: se repite lo último que se vio (sin traducir
 * nada ni tocar la tabla de destinos ni la RDRAM) con lo de encima, y el
 * flip sigue al ritmo de la tele: el sistema lo necesita con su menú
 * abierto, y así el cuadro de salir se dibuja sobre el juego. */
static void present_still(void)
{
	uint64_t now = sysGetSystemTime();
	rsxkit_rect r;
	frame_begin("flip anterior (pausa)");
	beat.pres_at = "cuadro repetido";
	/* Con la vista y el recorte de ahora: la página «Imagen» los cambia en
	 * la pausa (el recorte de una imagen de la RDRAM, al seguir). */
	struct salsa64_overscan crop = overscan_for(&still_f);
	r.w = 0;
	bool filt = flt.cur != SALSA64_CRT_NONE && !filter_tests();
	if (pres.still == STILL_RSX) {
		if (!(filt && filter_source_again(&crop) && !filter_announce(now) && filter_draw(&r)) &&
		    !salsa64_rdp_present_again(&pres.view, &crop, &r))
			pres.still = STILL_NONE;  /* el destino se soltó: negro */
	} else if (pres.still == STILL_FB) {
		rsxkit_texture t;
		if (!(filt && rsxkit_fb_texture(pres.fb, &t) &&
		      (flt.src.tex = t, flt.src.factor = 1, flt.src.has_gen = false, !filter_announce(now)) &&
		      filter_draw(&r)))
			rsxkit_fb_draw(pres.fb, &pres.view, &r);
	}
	if (r.w)
		scan_r = r;
	present_overlay(now);
	frame_end("flip (pausa)");
	beat.pres_at = "pausa";
	pres.frames_total++;
	pres.still_frames++;
	pres.t_flip = sysGetSystemTime();
}

/* Consume lo encolado: campos en orden y, al final, las tareas gráficas que
 * ya terminaron y el RDP pudo dibujar (son del campo siguiente; no la que
 * espera a que el CPU descongele el RDP). Solo lo que había al entrar: con
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
			uint64_t t0 = tb_now(), flush0 = h1.t_flush, flip0 = h1.t_flip;
			present_field(&m, now);
			h1.t_present += tb_now() - t0 - (h1.t_flush - flush0) - (h1.t_flip - flip0);
		} else if (m.snap >= 0) {
			snap_cas(m.snap, SNAP_QUEUED, SNAP_FREE);
		}
		lwsync();
		field_tail++;
	}
	/* Al cerrar (sin presentar) se traduce todo: no queda nada en vuelo. */
	h1_flush(present ? tasks_visible : ~0u);
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
	uint64_t h1_busy0;
	struct salsa64_rdp_tb rdp_tb0;
	struct ovl_group ovl0[OVL_GROUPS], load0[LOAD_PHASES];
} sw;

/* Las ventanas del hilo 1 empiezan aquí. */
static void h1_window_reset(void)
{
	sw.h1_busy0 = busy_at(&h1.busy, tb_now());
	salsa64_rdp_get_tb(&sw.rdp_tb0);
	memcpy(sw.ovl0, h1.by_ovl, sizeof sw.ovl0);
	memcpy(sw.load0, h1.by_load, sizeof sw.load0);
	h1.t_flip = h1.t_flush = h1.t_present = h1.t_load = 0;
	memset(&h1.in_flush, 0, sizeof h1.in_flush);
}

/* Lo que trabajó presentación en la ventana y cómo se solapó con la
 * emulación (ver «Cuánto le quita el hilo de presentación»). */
static void h1_stats(uint32_t fields, uint64_t dt_us)
{
	double k = 1e3 / spukit_tick_hz() / (fields ? fields : 1);  /* ticks → ms por campo emulado */
	uint64_t busy = busy_at(&h1.busy, tb_now()) - sw.h1_busy0;
	struct salsa64_rdp_tb tb;
	salsa64_rdp_get_tb(&tb);
	uint64_t copy = tb.copy - sw.rdp_tb0.copy, tex = tb.tex - sw.rdp_tb0.tex, fence = tb.fence - sw.rdp_tb0.fence;
	int64_t translate = (int64_t)(h1.t_flush - h1.in_flush.copy - h1.in_flush.tex - h1.in_flush.fence);
	int64_t present = (int64_t)(h1.t_present - (copy - h1.in_flush.copy) - (tex - h1.in_flush.tex) -
				    (fence - h1.in_flush.fence));
	int64_t other = (int64_t)(busy - h1.t_flush - h1.t_present - h1.t_load);
	salsa64_log("hilo 1: despierto %.2f ms por campo emulado (%.1f %% del tiempo): traducción %.2f, copias a la "
		    "RDRAM %.2f, texturas en el PPE %.2f, presentar %.2f, carga de mentira %.2f, otros %.2f; dentro, "
		    "dormido esperando barreras del RSX %.2f; flip anterior %.2f (dormido)",
		    busy * k, dt_us ? busy * 1e8 / spukit_tick_hz() / dt_us : 0.0, translate * k, copy * k, tex * k,
		    present * k, h1.t_load * k, other * k, fence * k, h1.t_flip * k);
	char g[OVL_GROUPS][128];
	for (int i = 0; i < OVL_GROUPS; i++)
		ovl_text(g[i], sizeof g[i], &h1.by_ovl[i], &sw.ovl0[i], false);
	salsa64_log("hilo 1 sobre la emulación, campos por solape: < 0,1 ms %s; 0,1–1 ms %s; 1–3 ms %s; ≥ 3 ms %s", g[0],
		    g[1], g[2], g[3]);
	if (h1.load) {
		for (int i = 0; i < LOAD_PHASES; i++)
			ovl_text(g[i], sizeof g[i], &h1.by_load[i], &sw.load0[i], true);
		salsa64_log("carga-hilo1 por fase: 0 µs %s; %u µs %s; %u µs %s", g[0], h1.load_us[1], g[1], h1.load_us[2],
			    g[2]);
	}
	h1_window_reset();
}

static void stats(uint64_t now)
{
	const struct salsa64_boot_info *info = &boot_info;

	if (!sw.t_stats) {
		sw.t_stats = now;
		h1_window_reset();
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
	h1_stats(fields, dt);
	/* El RSX: lo de la emulación por campo emulado y lo de presentar
	 * (imagen, filtro, avisos) por campo presentado, y el filtro por
	 * pasada. */
	rt_collect();
	if (rt.n[RT_EMU] || rt.n[RT_PRES])
		salsa64_log("rsx: emulación %.2f ms por campo emulado, presentación %.2f ms por campo presentado (%.0f %% "
			    "del tiempo ocupado en total; cota superior)",
			    fields ? rt.ns[RT_EMU] / 1e6 / fields : 0.0,
			    pres.presented ? rt.ns[RT_PRES] / 1e6 / pres.presented : 0.0,
			    dt ? (rt.ns[RT_EMU] + rt.ns[RT_PRES]) / 10.0 / dt : 0.0);
	memset(rt.ns, 0, sizeof rt.ns);
	memset(rt.n, 0, sizeof rt.n);
	{
		char line[1024];
		if (salsa64_filters_report(line, sizeof line))
			for (char *p = line, *nl; p; p = nl) {
				nl = strchr(p, '\n');
				if (nl)
					*nl++ = '\0';
				salsa64_log("rsx: %s", p);
			}
	}
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
			    "dormida en bucles de espera, traducción al RSX %.2f ms; %u destinos creados (%u reciclados por "
			    "memoria), %u esperas por un juego de búferes (%.2f ms); %u tareas cortadas, %u comandos "
			    "de otro microcódigo saltados, %u fondos de S2DEX",
			    tasks, tasks * 1e6 / dt, (double)(rs.draws - sw.rdp0.draws) / tasks,
			    (double)(rs.triangles - sw.rdp0.triangles) / tasks, spu_ms, wait_ms, waits,
			    (double)polls / tasks, sleep_ms, (rs.us_translate - sw.rdp0.us_translate) / 1e3 / tasks,
			    rs.targets_created - sw.rdp0.targets_created, rs.targets_evicted - sw.rdp0.targets_evicted,
			    rs.set_waits - sw.rdp0.set_waits,
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
		uint32_t rb = rs.readbacks - sw.rdp0.readbacks, rbw = rs.readback_waits - sw.rdp0.readback_waits;
		uint32_t trsx = rs.tex_rsx - sw.rdp0.tex_rsx, trd = rs.tex_rdram - sw.rdp0.tex_rdram;
		uint32_t tself = rs.tex_self - sw.rdp0.tex_self, primes = rs.primes - sw.rdp0.primes;
		uint32_t mir = rs.mirrors - sw.rdp0.mirrors, mirs = rs.mirrors_skipped - sw.rdp0.mirrors_skipped;
		uint32_t fmir = rs.frames_mirrored - sw.rdp0.frames_mirrored, dmas = rs.dma_skipped - sw.rdp0.dma_skipped;
		uint32_t lost = rs.readbacks_lost - sw.rdp0.readbacks_lost, tfail = rs.targets_failed - sw.rdp0.targets_failed;
		uint32_t tabs = rs.targets_absurd - sw.rdp0.targets_absurd, bfail = rs.binds_failed - sw.rdp0.binds_failed;
		uint32_t tppe = rs.tex_ppe - sw.rdp0.tex_ppe, tppel = rs.tex_ppe_lost - sw.rdp0.tex_ppe_lost;
		if (rb || rbw || mir || mirs || fmir || dmas || trsx || trd || tself || primes || lost || tfail || tabs ||
		    bfail || tppe || tppel)
			salsa64_log("gfx: framebuffer: %u copias a la RDRAM (%.2f ms c/u en presentación), la emulación "
				    "esperó %u tareas (%.2f ms c/u, %u tarde), %u copias espejo (%u sin búfer), %u cuadros "
				    "copiados, %u copias descartadas por un DMA posterior; texturas desde un destino: %u del "
				    "RSX, %u de la RDRAM, %u del ligado, %u decodificadas en el PPE (%u sin lugar); %u destinos "
				    "cebados; %u copias perdidas, %u destinos sin superficie, %u color images imposibles, %u "
				    "dibujos sin ligar (ninguna profundidad cubría el destino)",
				    rb, rb ? (rs.us_readback - sw.rdp0.us_readback) / 1e3 / rb : 0.0, rbw,
				    rbw ? (rs.us_readback_wait - sw.rdp0.us_readback_wait) / 1e3 / rbw : 0.0,
				    rs.readback_late - sw.rdp0.readback_late, mir, mirs, fmir, dmas, trsx, trd, tself, tppe,
				    tppel, primes, lost, tfail, tabs, bfail);
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
			    "%llu enlaces, %llu invalidados, %llu vaciados (ids %llu, enlaces %llu, RDRAM %llu, bucles %llu, "
			    "ROM %llu, estado %llu, otros %llu), %llu desalojos, %llu traducciones, %llu dependen de FR "
			    "(%llu gemelos, %llu salidas por FR, %llu por CU1), %llu instr. interpretadas",
			    (unsigned long long)st->compiled, (unsigned long long)st->compiled_x,
			    salsa64_rec_used(&rec) / 256, (unsigned long long)st->entries,
			    (unsigned long long)st->linked, (unsigned long long)st->invalidated,
			    (unsigned long long)st->flushes, (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_IDS],
			    (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_LINKS],
			    (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_RDRAM],
			    (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_IDLE],
			    (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_ROM],
			    (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_STATE],
			    (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_OTHER], (unsigned long long)st->evictions,
			    (unsigned long long)st->vt_probes, (unsigned long long)st->fr_dep,
			    (unsigned long long)st->fr_twins, (unsigned long long)st->fr_exits,
			    (unsigned long long)st->cu1_exits, (unsigned long long)st->interp_steps);
		if (rsmp.on) {
			char text[320];
			salsa64_rec_sample_split(rec_sample, &rsmp.prev, text, sizeof text);
			salsa64_log("%s", text);
		}
		if (rprof.on)
			salsa64_log("perfil del recompilador: %u entradas (%u calientes, %u sin lugar) y %u sitios de salida "
				    "(%u sin lugar), %llu bloques en la zona caliente (%u de %u KB en la mitad %u, %llu no "
				    "cupieron, %llu desalojos de mitad), %llu entradas a bloques contadas, %u escrituras (%u "
				    "fallidas)",
				    rec_profile->count, rec_profile->hot_entries, rec_profile->full, rec_profile->nsites,
				    rec_profile->sites_full, (unsigned long long)st->hot_placed, salsa64_rec_hot_used(&rec) / 256,
				    rec.hot_words / 256, rec.cur_hot, (unsigned long long)st->hot_full,
				    (unsigned long long)st->hot_evictions, (unsigned long long)rec_profile->session, rprof.saves,
				    rprof.save_errors);
		if (rec.prof) {
			char text[2048];
			salsa64_rec_prof_report(rec.prof, &rec_prof_prev, spukit_tick_hz(), fields, text, sizeof text);
			for (char *l = text, *e; l && *l; l = e) {
				if ((e = strchr(l, '\n')))
					*e++ = '\0';
				salsa64_log("%s", l);
			}
		}
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

/* Con la emulación quieta en el límite de un campo (state_parked): lo que
 * quedaba en las colas se presenta (el campo con FIELD_STATE deja la
 * miniatura), el RSX termina todo y se guarda o se carga. Al cargar se
 * olvida lo que era de la imagen anterior: destinos del RSX, audio en cola,
 * el último campo encolado y las ventanas que cuentan campos y ciclos (el
 * número de campo retrocede). La caché del recompilador la vacía
 * salsa64_state_load. */
static void state_barrier(void)
{
	salsa64_log("estado: barrera: emulación quieta en el campo %u (tarea del RSP %s)", n64.bus.vi.fields,
		    state_quiet ? "recogida" : "sin recoger");
	drain(sysGetSystemTime(), 1);
	rsxkit_finish();
	salsa64_log("estado: barrera: colas vacías y RSX terminado");
	/* Ninguna copia RSX → RDRAM en vuelo: al guardar quedan en el estado y
	 * al cargar no llegan tarde sobre la RDRAM cargada. */
	salsa64_rdp_settle();
	salsa64_log("estado: barrera: copias RSX → RDRAM en vuelo terminadas");
	int req = state_req;
	unsigned slot = state_req_slot;
	char err[160] = "";
	struct states_timing t;
	uint32_t field0 = n64.bus.vi.fields;
	bool ok = false;
	if (!state_quiet) {
		osd_notice(sysGetSystemTime(), "Estados no disponibles con lle");
	} else if (req == STATE_SAVE) {
		ok = states_save(&n64, slot, &t, err, sizeof err);
		if (ok) {
			osd_notice(sysGetSystemTime(), "Estado guardado en ranura %u", slot);
			salsa64_log("estado: guardado en la ranura %u (campo %u): %.1f ms, de ellos %.1f ms escribiendo; %u KB; "
				    "miniatura %.1f ms", slot, field0, t.us_total / 1e3, t.us_io / 1e3, t.bytes >> 10,
				    states_thumb_us() / 1e3);
		} else {
			osd_notice(sysGetSystemTime(), "No se pudo guardar: %s", err);
			salsa64_log("estado: FALLO al guardar en la ranura %u: %s", slot, err);
		}
	} else if (req == STATE_LOAD) {
		bool broken = false;
		ok = states_load(&n64, slot, &t, err, sizeof err, &broken);
		if (ok) {
			/* La tarea que esperaba al RDP congelado era de antes de
			 * cargar: se traduce ya, sobre destinos que se sueltan. */
			salsa64_rdp_flush(~0u);
			salsa64_rdp_drop_targets();
			audio_out_flush();
			post_last = ~0u;
			for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++)
				pif_edge_seen[i] = n64.bus.pif.sent[i].buttons;
			gwin.field0 = n64.bus.vi.fields;
			gwin.t0 = 0;
			sw.cyc0 = n64.cpu.cycles;
			sw.idle0 = n64.cpu.idle_cycles;
			sw.pif_reads0 = n64.bus.pif.reads[0];
			osd_fps_reset(&fps);
			osd_notice(sysGetSystemTime(), "Estado cargado");
			salsa64_log("estado: cargado de la ranura %u: campo %u → %u, %.1f ms, de ellos %.1f ms leyendo; %u KB",
				    slot, field0, n64.bus.vi.fields, t.us_total / 1e3, t.us_io / 1e3, t.bytes >> 10);
		} else {
			osd_notice(sysGetSystemTime(), "No se pudo cargar: %s", err);
			salsa64_log("estado: FALLO al cargar la ranura %u: %s%s", slot, err,
				    broken ? " (la emulación quedó a medias: se cierra el juego)" : "");
			if (broken)
				running = 0;
		}
	}
	states_thumb_clear();
	state_last_ok = ok;
	state_done++;
	lwsync();
	state_req = STATE_NONE;
}

static void state_test_step(void)
{
	uint32_t field = n64.bus.vi.fields;
	switch (state_test.stage) {
	case ST_SAVE:
	case ST_LOAD:
		if (state_req || state_parked ||
		    field < (state_test.stage == ST_SAVE ? state_test.save_at : state_test.load_at))
			return;
		state_test.done0 = state_done;
		state_slot = 9;
		state_command(state_test.stage == ST_SAVE ? STATE_SAVE : STATE_LOAD);
		state_test.stage++;
		return;
	case ST_SAVING:
	case ST_LOADING:
		if (state_done == state_test.done0)
			return;
		salsa64_log("estados, prueba automática: %s %s (campo %u)",
			    state_test.stage == ST_SAVING ? "guardar" : "cargar", state_last_ok ? "bien" : "FALLO", field);
		state_test.stage = !state_last_ok ? ST_FAILED : state_test.stage == ST_SAVING ? ST_LOAD : ST_DONE;
		if (state_test.stage == ST_FAILED && pres.result == SELFTEST_RUNNING) {
			pres.result = SELFTEST_FAILED;
			salsa64_log("%s: FALLO (estado guardado, campo %u)", res_tag, field);
			if (pres.autotest)
				running = 0;
		}
		return;
	default:
		return;
	}
}

/* Lo que pide pause_ctl (menú del sistema, cuadro de salir) pasa a la
 * emulación y al audio; cada cambio, al log. */
static struct {
	uint64_t t0;      /* inicio de la pausa en curso */
	uint32_t still0;  /* cuadros repetidos al empezarla */
} pause_log;

static void image_scale_apply(void);

static void pause_update(void)
{
	uint32_t want = pausectl_reasons(&pause_ctl), was = pause_req;
	if (sys_no_pause)
		want &= ~PAUSECTL_SYSTEM;
	if (want == was)
		return;
	uint64_t now = sysGetSystemTime();
	if (want & ~was & PAUSECTL_CONFIRM)
		salsa64_log("salida: START + SELECT, cuadro «%s» (× sí, ○ no)", in_menu ? "¿Volver al menú?" : "¿Salir al XMB?");
	if (was & ~want & PAUSECTL_CONFIRM)
		salsa64_log("salida: «No», el juego sigue");
	if (!was) {
		pause_log.t0 = now;
		pause_log.still0 = pres.still_frames;
		audio_out_pause(true);
		salsa64_log("pausa: emulación detenida en el campo %u (%s%s%s%s)", n64.bus.vi.fields,
			    want & PAUSECTL_SYSTEM ? "menú del sistema" : "",
			    (want & PAUSECTL_SYSTEM) && (want & ~PAUSECTL_SYSTEM) ? " y " : "",
			    want & PAUSECTL_CONFIRM ? "cuadro de salir" : "", want & PAUSECTL_PANEL ? "lista de trucos" : want & PAUSECTL_MENU ? "menú de pausa" : "");
	} else if (!want) {
		if (scale_wanted() != rdp_scale_req)
			image_scale_apply();  /* con la emulación todavía quieta */
		audio_out_pause(false);
		salsa64_log("pausa: la emulación sigue tras %.1f s (%u cuadros repetidos, emulación %s)",
			    (now - pause_log.t0) / 1e6, pres.still_frames - pause_log.still0,
			    emu_paused ? "quieta en el límite del campo" : "todavía sin detenerse");
	}
	lwsync();
	pause_req = want;
}

static void settings_read(struct menu_settings *s);
static void rom_names(const char *path, const char **title, const char **code, uint32_t *crc1, uint32_t *crc2);

/* Los trucos del juego (game_cheats) se cargan al abrirlo. */
static void load_cheats(const char *source)
{
	const char *name = source ? strrchr(source, '/') : NULL;
	name = name ? name + 1 : source;
	memset(&game_cheats, 0, sizeof game_cheats);
	if (!name || !name[0])
		return;
	const char *title = NULL, *code = NULL;
	uint32_t crc1 = 0, crc2 = 0;
	rom_names(source, &title, &code, &crc1, &crc2);
	unsigned n = cheats_load(&game_cheats, CHEATS_DIR, name, title, code, crc1, crc2);
	if (!n) {
		salsa64_log("trucos: ninguno (%s)", game_cheats.cht_path);
		return;
	}
	salsa64_log("trucos: %u en %s, %u activos%s", n, game_cheats.cht_path, cheats_active(&game_cheats),
		    game_cheats.unsupported ? " (hay líneas que no se aplican)" : "");
	if (game_cheats.crc_mismatch)
		salsa64_log("trucos: el CRC de la ROM no es el de ninguna sección del .cht (se usó la primera)");
}

/* juegos/<ROM sin extensión>.cfg: lo que un juego cambia de lo global. */
static void game_cfg_path(const char *rom, char *out, size_t n)
{
	const char *name = strrchr(rom, '/');
	name = name ? name + 1 : rom;
	const char *dot = strrchr(name, '.');
	int len = (int)(dot && dot > name ? (size_t)(dot - name) : strlen(name));
	snprintf(out, n, GAMES_DIR "/%.*s.cfg", len, name);
}

static void game_cfg_read(const char *rom, struct menu_game *g)
{
	char path[320], text[512];
	menu_game_default(g);
	game_cfg_path(rom, path, sizeof path);
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	size_t n = fread(text, 1, sizeof text - 1, f);
	fclose(f);
	menu_game_parse(g, text, n);
}

static bool game_cfg_write(const char *rom, const struct menu_game *g)
{
	char path[320], text[512];
	game_cfg_path(rom, path, sizeof path);
	mkdir(GAMES_DIR, 0777);
	int n = menu_game_format(g, text, sizeof text);
	FILE *f = fopen(path, "w");
	bool ok = f && fwrite(text, 1, (size_t)n, f) == (size_t)n;
	if (f && fclose(f) != 0)
		ok = false;
	return ok;
}

/* El CRT de view_apply (filter_setup lo pone al abrir el juego). */
static int view_crt;

/* El filtro CRT (enum salsa64_crt) de un filtro del menú y su densidad (la
 * máscara fina es otro filtro para frontend/filters); SALSA64_CRT_NONE para
 * los demás. Para cambiarlo en vivo: filter_select(menu_filter_crt(…)). */
static int menu_filter_crt(int filter, int density)
{
	if (filter == MENU_FILTER_CRT_MASK && density == MENU_DENSITY_FINE)
		return SALSA64_CRT_MASK_FINE;
	return filter >= MENU_FILTER_CRT_MASK && filter <= MENU_FILTER_CRT_TRINITRON
		       ? SALSA64_CRT_MASK + (filter - MENU_FILTER_CRT_MASK)
		       : SALSA64_CRT_NONE;
}

/* Pide al RDP la escala interna `scale` (0: automática) si no es la que
 * tiene: sin juego corriendo o con la emulación quieta en el límite de un
 * campo (salsa64_rdp_set_scale suelta los destinos; el juego los vuelve a
 * crear a la escala nueva en su cuadro siguiente). */
static void rdp_scale_set(int scale, const char *why)
{
	if (scale == rdp_scale_req)
		return;
	bool cut = false;
	salsa64_rdp_set_scale((uint32_t)scale);
	rdp_scale_req = scale;
	uint32_t es = salsa64_rdp_scale(&cut);
	salsa64_log("escala interna: %u× (%s%s), %s", es, scale ? "pedida" : "automática",
		    cut ? ", reducida por memoria del RSX" : "", why);
}

/* Si la escala elegida (img.scale; 0 automática) pasa de CRT_SCALE_MAX con
 * un CRT en pantalla (o con el ciclo de medición, que los pasa todos). */
static bool scale_capped(void)
{
	bool crt = !filter_tests() && (flt.cycle || flt.cur != SALSA64_CRT_NONE);
	int chosen = img.scale ? img.scale : video_cfg.resolution == MENU_RES_1080P ? 4 : 3;  /* menu_scale_name */
	return crt && chosen > CRT_SCALE_MAX;
}

/* La escala que se le pide al RDP: la elegida o, con un CRT, el tope. */
static int scale_wanted(void)
{
	return scale_capped() ? CRT_SCALE_MAX : img.scale;
}

/* La escala para mostrar: la elegida o «3× (tope con CRT)». */
static const char *scale_label(char *buf, size_t n)
{
	return scale_capped() ? "3× (tope con CRT)" : menu_scale_name(img.scale, video_cfg.resolution, buf, n);
}

/* Lo de img a la presentación: vista (aspecto y filtro), líneas de barrido
 * y recorte. La escala va aparte (rdp_scale_set). */
static void view_apply(void)
{
	pres.view.aspect = img.aspect == MENU_ASPECT_16_9 ? 16.0f / 9.0f : 4.0f / 3.0f;
	pres.view.filter = img.filter == MENU_FILTER_SHARP ? RSXKIT_FILTER_NEAREST : RSXKIT_FILTER_LINEAR;
	scan_level = img.filter == MENU_FILTER_SCAN_SOFT ? 1 : img.filter == MENU_FILTER_SCAN_STRONG ? 2 : 0;
	view_crt = menu_filter_crt(img.filter, img.density);
	pause_ctl.image_density = menu_filter_has_density(img.filter);
	filter_select(view_crt);
	if (img.crop >= MENU_OVERSCAN_OFF && img.crop <= MENU_OVERSCAN_WIDE) {
		uint32_t h = img.crop == MENU_OVERSCAN_WIDE ? 16 : img.crop == MENU_OVERSCAN_NORMAL ? 8 : 0;
		overscan.left = overscan.right = h;
		overscan.top = overscan.bottom = h * 3 / 4;
	}
}

static void view_log(const char *what)
{
	char sc[16] = "automática";
	if (img.scale)
		snprintf(sc, sizeof sc, "%d×", img.scale);
	salsa64_log("imagen: %s, filtro %s%s%s, recorte %u %u %u %u, escala %s%s%s",
		    img.aspect == MENU_ASPECT_16_9 ? "16:9 estirada" : "4:3", menu_filter_name(img.filter),
		    menu_filter_has_density(img.filter) ? ", densidad " : "",
		    menu_filter_has_density(img.filter) ? (img.density == MENU_DENSITY_FINE ? "fina" : "normal") : "",
		    overscan.left, overscan.right, overscan.top, overscan.bottom, sc,
		    scale_capped() ? " (con el CRT, 3×)" : "", what);
}

/* Aspecto, filtro, recorte y escala del juego: lo suyo (juegos/) o, si no,
 * lo de menu.cfg, overscan.cfg y video.cfg. Antes de cada juego, después de
 * load_overscan. */
static void load_view(const char *source)
{
	struct menu_settings s;
	struct menu_game g;
	settings_read(&s);
	s.overscan = overscan_preset();  /* el de overscan.cfg */
	s.scale = video_cfg.scale;
	img.source = source;
	if (source)
		game_cfg_read(source, &img.game);
	else
		menu_game_default(&img.game);
	g = img.game;
	if (selftest_on || gfxtest_on || audiotest_on) {
		/* Sus verificaciones leen la pantalla: aspecto, filtro y recorte de
		 * siempre, sin guardar. La escala del juego sí vale (verifican a
		 * cualquier escala): así se prueba el cambio al abrir y al cerrar. */
		int scale = g.scale;
		menu_game_default(&g);
		g.scale = scale;
		s.aspect = MENU_ASPECT_4_3;
		s.filter = MENU_FILTER_SMOOTH;
		img.source = NULL;
	}
	img.g_aspect = s.aspect;
	img.g_filter = s.filter;
	img.g_crop = s.overscan;
	img.g_scale = s.scale;
	img.g_density = s.density;
	menu_game_effective(&s, &g, &img.aspect, &img.filter, &img.crop, &img.scale);
	img.density = menu_game_density(&s, &g);
	view_apply();
	scan_r.w = 0;
	scan_lines = 0;
	view_log(g.aspect >= 0 || g.filter >= 0 || g.overscan >= 0 || g.scale >= 0 ? " (con ajustes de este juego)" : "");
}

/* «Guardar para este juego»: en juegos/<ROM>.cfg lo que se ve. Lo que
 * coincide con lo global y el juego no tenía propio queda «global» (sigue
 * a Opciones); un recorte personalizado de overscan.cfg, también. */
static void image_save(void)
{
	uint64_t now = sysGetSystemTime();
	if (!img.source) {
		osd_notice(now, "No se pudieron guardar los ajustes");
		return;
	}
	struct menu_game g = img.game;
	g.aspect = img.aspect == img.g_aspect && img.game.aspect < 0 ? -1 : img.aspect;
	g.filter = img.filter == img.g_filter && img.game.filter < 0 ? -1 : img.filter;
	g.overscan = img.crop > MENU_OVERSCAN_WIDE || (img.crop == img.g_crop && img.game.overscan < 0) ? -1 : img.crop;
	g.scale = img.scale == img.g_scale && img.game.scale < 0 ? -1 : img.scale;
	g.density = img.density == img.g_density && img.game.density < 0 ? -1 : img.density;
	bool ok = game_cfg_write(img.source, &g);
	if (ok)
		img.game = g;
	salsa64_log("imagen: ajustes de este juego %s (aspecto %d, filtro %d, densidad %d, recorte %d, escala %d)",
		    ok ? "guardados" : "NO guardados", g.aspect, g.filter, g.density, g.overscan, g.scale);
	osd_notice(now, ok ? "Ajustes guardados para este juego" : "No se pudieron guardar los ajustes");
}

/* La página «Imagen» del menú de pausa (pausectl): filtro, aspecto y
 * recorte se ven al momento, también en el cuadro en pausa; la escala se
 * pone al seguir jugando (image_scale_apply), porque soltar los destinos
 * con el juego quieto dejaría la pantalla en negro. */
static void image_events(void)
{
	int d = pause_ctl.image_do, k = pause_ctl.image_arg;
	if (d < 0)
		return;
	pause_ctl.image_do = -1;
	switch (d) {
	case IM_FILTER:
		img.filter = (img.filter + k + MENU_FILTERS) % MENU_FILTERS;
		break;
	case IM_DENSITY:
		img.density = (img.density + k + MENU_DENSITIES) % MENU_DENSITIES;
		break;
	case IM_ASPECT:
		img.aspect = img.aspect == MENU_ASPECT_16_9 ? MENU_ASPECT_4_3 : MENU_ASPECT_16_9;
		break;
	case IM_CROP:
		/* Apagado → normal → amplio; uno personalizado pasa al primero o al último. */
		img.crop = img.crop > MENU_OVERSCAN_WIDE ? (k > 0 ? MENU_OVERSCAN_OFF : MENU_OVERSCAN_WIDE)
							  : (img.crop + k + 3) % 3;
		break;
	case IM_SCALE:
		img.scale = (img.scale + k + MENU_SCALE_MAX + 1) % (MENU_SCALE_MAX + 1);
		return;  /* al seguir */
	case IM_SAVE:
		image_save();
		return;
	default:
		return;
	}
	view_apply();
	view_log(" (menú de pausa)");
}

/* Al seguir jugando con otra escala elegida en «Imagen»: con la emulación
 * quieta en el límite del campo (antes de soltar pause_req). */
static void image_scale_apply(void)
{
	for (int i = 0; i < 100 && !emu_paused && running; i++)
		sysUsleep(2000);  /* una pausa cortísima: que termine su campo */
	if (!emu_paused) {
		salsa64_log("escala interna: la emulación no se detuvo; queda %d", rdp_scale_req);
		if (!scale_capped())
			img.scale = rdp_scale_req;
		return;
	}
	rdp_scale_set(scale_wanted(), scale_capped() ? "menú de pausa, tope de 3× con un CRT" : "menú de pausa");
	char sbuf[24];
	osd_notice(sysGetSystemTime(), "Escala interna: %s", tr(scale_label(sbuf, sizeof sbuf)));
}

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
	memset(h1.by_ovl, 0, sizeof h1.by_ovl);
	memset(h1.by_load, 0, sizeof h1.by_load);
	load_config();
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
	pres.last_origin = pres.game_frames = pres.frames_total = 0;
	pres.still = STILL_NONE;
	pres.t_flip = 0;
	pres.still_frames = 0;
	pause_req = 0;
	emu_paused = 0;
	pausectl_game_end(&pause_ctl);
	state_req = STATE_NONE;
	state_parked = 0;
	state_quiet = 0;
	state_slot = 0;
	combos_on = 0;
	state_done = 0;
	state_last_ok = false;
	memset(&state_test, 0, sizeof state_test);
	osd_notice_clear();
	osd_fps_reset(&fps);
	states_thumb_clear();
}

/* Cierra lo que es del juego con la emulación ya detenida, en orden y con
 * cada paso al log: el guardado al disco, el audio (puerto, hilo y SPE), la
 * HLE gráfica (espera la tarea en vuelo) y el LLE con sus SPEs, el
 * recompilador, las colas entre hilos y los destinos del RSX. Termina con
 * una línea que dice si quedó algo en vuelo. */
/* ---- Perfil persistente del recompilador (cpu/rec_profile->h) ----------------- */

/* perfil.cfg: `caliente = KB` (0: contar sin zona caliente) y `saltos =
 * si|no` (registrar los sitios de salida de los bloques calientes). */
static uint32_t profile_hot_kb(bool *sites)
{
	uint32_t kb = PROFILE_HOT_KB;
	*sites = true;
	FILE *f = fopen(PROFILE_CFG_PATH, "r");
	if (!f)
		return kb;
	char line[128], word[16];
	while (fgets(line, sizeof line, f)) {
		unsigned v;
		if (sscanf(line, " caliente = %u", &v) == 1)
			kb = v;
		else if (sscanf(line, " saltos = %15s", word) == 1)
			*sites = word[0] != 'n' && word[0] != 'N' && word[0] != '0';
	}
	fclose(f);
	return kb;
}

static void profile_io_thread(void *arg);

/* ---- Muestreo por tiempo (rec_sample.h) ------------------------------------------- */

static void sample_thread(void *arg)
{
	(void)arg;
	while (!rsmp.stop) {
		sysUsleep(SAMPLE_US);
		salsa64_rec_sample_tick(rec_sample, &n64.cpu);
	}
	sysThreadExit(0);
}

static void sample_line(void *ctx, const char *text)
{
	(void)ctx;
	salsa64_log("%s", text);
}

/* Con el archivo `muestreo` y el recompilador activo: la tabla, el hilo y
 * rec.sample (desde entonces cada bloque compilado deja su id al entrar). */
static void sample_open(void)
{
	if (!use_rec || !salsa64_ps3_rec_sample_wanted())
		return;
	if (!rec_sample && !(rec_sample = memalign(128, sizeof *rec_sample))) {
		salsa64_log("muestreo del recompilador: sin memoria para la tabla (%u KB)", (unsigned)(sizeof *rec_sample >> 10));
		return;
	}
	salsa64_rec_sample_init(rec_sample);
	memset(&rsmp, 0, sizeof rsmp);
	rec.sample = rec_sample;
	if (sysThreadCreate(&rsmp.tid, sample_thread, NULL, SAMPLE_PRIO, 0x4000, THREAD_JOINABLE, (char *)"salsa64_muestreo") !=
	    0) {
		rec.sample = NULL;
		salsa64_log("muestreo del recompilador: no se pudo crear el hilo");
		return;
	}
	rsmp.on = true;
	rsmp.t0 = sysGetSystemTime();
	salsa64_log("muestreo del recompilador: una muestra cada %u µs desde un hilo de prioridad %d (archivo muestreo); "
		    "la entrada de cada bloque deja su id (2 palabras)",
		    SAMPLE_US, SAMPLE_PRIO);
}

/* Al cerrar el juego, con el hilo de emulación ya terminado. */
static void sample_close(void)
{
	u64 rv;
	rsmp.stop = 1;
	sysThreadJoin(rsmp.tid, &rv);
	double secs = (sysGetSystemTime() - rsmp.t0) / 1e6;
	salsa64_log("cierre: muestreo: %llu muestras en %.1f s (%.0f por segundo; pedidas %.0f)",
		    (unsigned long long)rec_sample->ticks, secs, secs > 0 ? rec_sample->ticks / secs : 0.0, 1e6 / SAMPLE_US);
	salsa64_rec_sample_report(&rec, 30, sample_line, NULL);
	rec.sample = NULL;
	rsmp.on = false;
}

/* Al abrir un juego, con el recompilador ya iniciado: carga su perfil (o
 * empieza uno), marca los calientes y lo conecta. */
static void profile_open(const uint8_t *rom)
{
	char base[48];
	memset(&rprof, 0, sizeof rprof);
	memset(&rec_profile_w, 0, sizeof rec_profile_w);
	if (!use_rec)
		return;
	if (!file_exists(PROFILE_ON_PATH)) {
		salsa64_log("perfil del recompilador: apagado (sin %s)", PROFILE_ON_PATH);
		return;
	}
	/* La tabla, la primera vez que se enciende (fuera del bucle de
	 * emulación); queda para los juegos siguientes. */
	if (!rec_profile && !(rec_profile = memalign(128, sizeof *rec_profile))) {
		salsa64_log("perfil del recompilador: sin memoria para la tabla (%zu bytes): apagado", sizeof *rec_profile);
		return;
	}
	mkdir(PROFILE_DIR, 0777);
	salsa64_save_basename(rom, base, sizeof base);
	snprintf(rprof.path, sizeof rprof.path, "%s/%s.prf", PROFILE_DIR, base);
	struct salsa64_rec_profile_game game;
	salsa64_rec_profile_game_of(&game, rom);
	bool loaded = salsa64_rec_profile_load(rec_profile, rprof.path, &game);
	salsa64_rec_profile_mark_hot(rec_profile, salsa64_rec_hot_capacity(&rec) * 3 / 4);
	bool sites;
	profile_hot_kb(&sites);
	rec.profile_sites = sites;
	rec.profile = rec_profile;
	rprof.on = true;
	rprof.t_saved = sysGetSystemTime();
	{
		sys_sem_attr_t attr;
		memset(&attr, 0, sizeof attr);
		attr.attr_protocol = SYS_SEM_ATTR_PROTOCOL;
		attr.attr_pshared = SYS_SEM_ATTR_PSHARED;
		rprof.thread_ok = sysSemCreate(&rprof.sem, &attr, 0, 1) == 0;
		if (rprof.thread_ok && sysThreadCreate(&rprof.tid, profile_io_thread, NULL, PROFILE_IO_PRIO, PROFILE_IO_STACK,
						       THREAD_JOINABLE, (char *)"salsa64_perfil") != 0) {
			sysSemDestroy(rprof.sem);
			rprof.thread_ok = false;
		}
	}
	salsa64_log("perfil del recompilador: %s %s (%u entradas y %u sitios de salida; %u calientes con %u KB, "
		    "umbral %u); zona caliente de %u KB en dos mitades, sitios de salida %s, se guarda cada %llu s y "
		    "al cerrar (%s)",
		    loaded ? "cargado" : "nuevo", rprof.path, rec_profile->count, rec_profile->nsites,
		    rec_profile->hot_entries, rec_profile->hot_words / 256, rec_profile->hot_threshold,
		    salsa64_rec_hot_capacity(&rec) / 256, sites ? "sí" : "no (saltos = no)",
		    (unsigned long long)(PROFILE_SAVE_US / 1000000),
		    rprof.thread_ok ? "hilo propio de baja prioridad" : "por trozos desde la presentación");
}

/* El hilo que escribe el perfil: duerme en el semáforo y, cuando la
 * presentación le pide una escritura, escribe el archivo entero (la tabla
 * sigue recibiendo cuentas mientras tanto: lee count y nsites una vez). */
static void profile_io_thread(void *arg)
{
	(void)arg;
	while (!rprof.stop) {
		sysSemWait(rprof.sem, 1000000);
		if (rprof.stop)
			break;
		if (!rprof.due)
			continue;
		rprof.due = 0;
		if (salsa64_rec_profile_save(rec_profile, rprof.path)) {
			rprof.saves++;
		} else {
			rprof.save_errors++;
			salsa64_log("perfil del recompilador: FALLO al escribir %s", rprof.path);
		}
	}
	sysThreadExit(0);
}

/* Hilo de presentación, cada vuelta: si pasó el plazo y hubo cuentas
 * nuevas, pide una escritura al hilo del perfil (o, sin él, la sigue por
 * trozos: unas entradas por vuelta, para no detener la presentación). */
static void profile_write_step(uint64_t now)
{
	if (!rprof.on)
		return;
	if (rprof.thread_ok) {
		if (now - rprof.t_saved < PROFILE_SAVE_US || rec_profile->session == rprof.session_saved || rprof.due)
			return;
		rprof.t_saved = now;
		rprof.session_saved = rec_profile->session;
		rprof.due = 1;
		sysSemPost(rprof.sem, 1);  /* si ya está en 1, da error: no importa */
		return;
	}
	if (rec_profile_w.f) {
		int r = salsa64_rec_profile_save_step(rec_profile, &rec_profile_w, PROFILE_STEP_ENTRIES);
		if (r < 0) {
			rprof.save_errors++;
			salsa64_log("perfil del recompilador: FALLO al escribir %s", rprof.path);
		} else if (r > 0) {
			rprof.saves++;
		}
		return;
	}
	if (now - rprof.t_saved < PROFILE_SAVE_US || rec_profile->session == rprof.session_saved)
		return;
	rprof.t_saved = now;
	rprof.session_saved = rec_profile->session;
	if (!salsa64_rec_profile_save_begin(rec_profile, &rec_profile_w, rprof.path)) {
		rprof.save_errors++;
		salsa64_log("perfil del recompilador: FALLO al abrir %s.tmp", rprof.path);
	}
}

/* Al cerrar el juego (el hilo de emulación ya terminó): lo contado al
 * perfil, el archivo de una vez y el perfil suelto. */
static void profile_close(void)
{
	const struct salsa64_rec_stats *st = &rec.stats;
	if (rprof.thread_ok) {
		u64 rv;
		rprof.stop = 1;
		sysSemPost(rprof.sem, 1);
		sysThreadJoin(rprof.tid, &rv);
		sysSemDestroy(rprof.sem);
		rprof.thread_ok = false;
	}
	salsa64_rec_profile_save_abort(&rec_profile_w);
	salsa64_rec_profile_fold(&rec);
	bool ok = salsa64_rec_profile_save(rec_profile, rprof.path);
	salsa64_log("cierre: perfil del recompilador %s %s: %u entradas (%u cargadas, %u sin lugar) y %u sitios de "
		    "salida (%u sin lugar), %llu entradas a bloques contadas (sesión %s), %llu bloques en la zona "
		    "caliente (%llu no cupieron, %llu desalojos de mitad), %u escrituras periódicas (%u fallidas)",
		    ok ? "guardado en" : "NO se pudo guardar en", rprof.path, rec_profile->count, rec_profile->loaded,
		    rec_profile->full, rec_profile->nsites, rec_profile->sites_full, (unsigned long long)rec_profile->session,
		    rec_profile->session >= SALSA64_RPF_LONG_SESSION ? "larga" : "corta",
		    (unsigned long long)st->hot_placed, (unsigned long long)st->hot_full,
		    (unsigned long long)st->hot_evictions, rprof.saves, rprof.save_errors);
	salsa64_rec_profile_detach(&rec);
	rprof.on = false;
}

static void game_close(void)
{
	salsa64_filters_release();
	salsa64_log("cierre: emulación detenida tras %u campos (%u emulados, %u descartados por la cola, %u sin ranura)",
		    n64.bus.vi.fields, emu_stats.fields, emu_stats.dropped, emu_stats.no_slot);
	savefile_close(&save_file);
	salsa64_log("cierre: guardado escrito y desconectado");
	/* audio_out: cierre (con el hilo de emulación ya terminado). */
	audio_out_shutdown();
	salsa64_log("cierre: audio cerrado (%u tareas de audio)", n64.bus.rsp.stats.audio);
	salsa64_rsp_set_gfx(&n64.bus, NULL);  /* espera la tarea en vuelo */
	salsa64_log("cierre: HLE gráfica suelta (%u tareas gráficas)", n64.bus.rsp.stats.gfx);
	if (jpeg_on_spu) {
		salsa64_rsp_set_jpeg(&n64.bus, NULL);
		salsa64_log("cierre: SPE de la tarea JPEG detenido (%u tareas, %u macrobloques, %u en el PPE; salida %d)",
			    jpeg_spu.tasks, jpeg_spu.macroblocks, jpeg_spu.on_ppe, salsa64_jpeg_spu_shutdown(&jpeg_spu));
		jpeg_on_spu = 0;
	}
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
	if (rsmp.on)
		sample_close();
	if (rprof.on)
		profile_close();
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
	bool spes = gfx_spu.worker || rsp_spu.worker || jpeg_spu.worker;
	bool hooks = n64.bus.rsp.gfx || n64.bus.rsp.audio || n64.bus.rsp.lle || n64.bus.rsp.jpeg_be || n64.bus.rsp.busy;
	bool clean = emu_done && !queued && !snaps && !untranslated && !spes && !hooks;
	salsa64_log("cierre: %s (hilo de emulación %s, %u campos en cola, %d ranuras del VI ocupadas, %u tareas "
		    "sin traducir, destinos del RSX liberados, SPEs de la HLE %s, RSP %s)",
		    clean ? "estado limpio" : "ATENCIÓN, quedó algo en vuelo", emu_done ? "terminado" : "VIVO", queued,
		    snaps, untranslated, spes ? "VIVOS" : "detenidos", hooks ? "CON BACKENDS" : "sin backends");
}

/* rsxkit_ui: el menú y lo que se dibuja sobre el juego (aviso, contador);
 * se prepara una vez (2 MB mapeados, para siempre). */
static int ui_init_once(void)
{
	static int rc = 1;
	if (rc == 1) {
		rc = rsxkit_ui_init();
		if (rc != RSXKIT_OK)
			salsa64_log("2D del RSX no disponible (%d): sin avisos en pantalla", rc);
	}
	/* La salida puede cambiar sin reiniciar («Aplicar ahora»). */
	const rsxkit_screen *sc = rsxkit_screen_info();
	ui_lh = 720.0f;
	ui_lw = sc->aspect > 1.5f ? 1280.0f : 960.0f;
	ui_ok = rc == RSXKIT_OK;
	return rc;
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
	n64.bus.rdram_written = front_rdram_written;
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
	/* Tarea JPEG (fondos prerrenderizados de Zelda OoT) en otro SPE, salvo
	 * que se pida el PPE o que el SPE no dé lo mismo que el PPE. */
	jpeg_on_spu = 0;
	if (!file_exists(JPEG_PPE_PATH)) {
		bool same;
		spukit_init(6);
		if ((rc = salsa64_jpeg_spu_init(&jpeg_spu, &n64.bus, &same)) != 0) {
			salsa64_log("JPEG: no arrancó el SPE (%d)", rc);
		} else if (!same) {
			salsa64_jpeg_spu_shutdown(&jpeg_spu);
		} else {
			salsa64_rsp_set_jpeg(&n64.bus, &jpeg_spu.backend);
			jpeg_on_spu = 1;
		}
	}
	salsa64_log("JPEG: tareas en %s", jpeg_on_spu ? "un SPE"
						      : file_exists(JPEG_PPE_PATH) ? "el PPE (" JPEG_PPE_PATH ")" : "el PPE");
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
	if (salsa64_ps3_rec_legacy_wanted())
		rec.opts |= SALSA64_REC_OPT_LEGACY;
	if (salsa64_ps3_rec_fpu_res_wanted())
		rec.opts |= SALSA64_REC_OPT_FPU_RES;
	if (salsa64_ps3_rec_cop0_exec_wanted())
		rec.opts |= SALSA64_REC_OPT_COP0_EXEC;
	if (salsa64_ps3_rec_no_evict_wanted())
		rec.opts |= SALSA64_REC_OPT_NO_EVICT;
	if (salsa64_ps3_rec_wide_wanted())
		rec.opts |= SALSA64_REC_OPT_WIDE;
	rec.prof = salsa64_ps3_rec_prof_wanted() ? &rec_prof : NULL;
	{
		bool sites;
		rec.hot_bytes = file_exists(PROFILE_ON_PATH) ? profile_hot_kb(&sites) << 10 : 0;
	}
	if (!salsa64_ps3_rec_wanted())
		salsa64_log("recompilador apagado (%s): intérprete", SALSA64_NOREC_FLAG);
	else if (!(use_rec = salsa64_rec_init(&rec, &n64.cpu, salsa64_ps3_rec_cache(), SALSA64_REC_CACHE_BYTES)))
		salsa64_log("recompilador no disponible: intérprete");
	else
		salsa64_log("páginas: RDRAM %u KB, rec %u KB, n64 %u KB, caché de código %u KB, pila %u KB (0 = sin dato)",
			    page_kb(rdram), page_kb(&rec), page_kb(&n64), page_kb(salsa64_ps3_rec_cache()),
			    page_kb(__builtin_frame_address(0))),
		salsa64_log("recompilador activo: caché de %u MB en %p, llamadas a C %s, opciones 0x%x%s%s%s%s%s%s",
			    SALSA64_REC_CACHE_BYTES >> 20, salsa64_ps3_rec_cache(), rec.calls_near ? "con bl" : "por mtctr",
			    rec.opts, rec.prof ? ", con perfil" : "",
			    rec.opts & SALSA64_REC_OPT_LEGACY ? ", camino viejo (jit-viejo)" : "",
			    rec.opts & SALSA64_REC_OPT_FPU_RES ? ", FPU residente (jit-fpu-residente)" : "",
			    rec.opts & SALSA64_REC_OPT_COP0_EXEC ? ", COP0 por el intérprete (jit-cop0-viejo)" : "",
			    rec.opts & SALSA64_REC_OPT_NO_EVICT ? ", sin tramos (jit-sin-tramos)" : "",
			    rec.opts & SALSA64_REC_OPT_WIDE ? ", código ancho (jit-ancho)" : "");
	profile_open(rom);
	sample_open();

	/* overscan.cfg: en cada juego (se cambia desde las Opciones del menú). */
	load_overscan();
	load_view(source);
	filter_setup(view_crt);  /* antes de la escala: el CRT la limita */
	rdp_scale_set(scale_wanted(), scale_capped() ? "al abrir el juego, tope de 3× con un CRT" : "al abrir el juego");
	load_cheats(source);
	if (selftest_on)
		selftest_start();
	if (gfxtest_on)
		gfxtest_start();
	if (audiotest_on)
		audiotest_start();
	{
		/* SALSA64_DATA_DIR/fbfx.cfg: 0 sin efectos de framebuffer, 1 solo
		 * render a textura, 2 también copias a la RDRAM (por omisión). */
		int fbfx = 2;
		FILE *ff = fopen(SALSA64_DATA_DIR "/fbfx.cfg", "r");
		if (ff) {
			if (fscanf(ff, "%d", &fbfx) != 1)
				fbfx = 2;
			fclose(ff);
		}
		salsa64_rdp_fbfx(fbfx);
		salsa64_log("efectos de framebuffer: %s", fbfx <= 0 ? "apagados" : fbfx == 1 ? "render a textura" :
								"render a textura y copias a la RDRAM");
	}
	{
		/* SALSA64_DATA_DIR/trace-vi: registrar qué se presenta en cada uno
		 * de los primeros N campos (por omisión 300). */
		FILE *tf = fopen(SALSA64_DATA_DIR "/trace-vi", "r");
		trace_vi_left = 0;
		if (tf) {
			if (fscanf(tf, "%u", &trace_vi_left) != 1)
				trace_vi_left = 300;
			fclose(tf);
		}
	}
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
	{
		FILE *tf = fopen(STATE_TEST_PATH, "r");
		if (tf) {
			if (fscanf(tf, "%u %u", &state_test.save_at, &state_test.load_at) == 2 &&
			    state_test.save_at < state_test.load_at) {
				state_test.stage = ST_SAVE;
				salsa64_log("estados, prueba automática: guardar en el campo %u y cargar en el %u (ranura 9)",
					    state_test.save_at, state_test.load_at);
			}
			fclose(tf);
		}
	}

	/* El hilo de emulación. */
	sys_ppu_thread_t emu_tid;
	if (sysThreadCreate(&emu_tid, emu_thread, NULL, EMU_PRIO, EMU_STACK, THREAD_JOINABLE,
			    (char *)"salsa64_emu") != 0) {
		salsa64_log("%s: FALLO al crear el hilo de emulación", res_tag);
		snprintf(err, err_size, "No se pudo crear el hilo de emulación");
		emu_done = 1;
		game_close();
		rdp_scale_set(video_cfg.scale, "vuelve la global");
		return GAME_FAILED;
	}
	emu_live = 1;
	salsa64_log("hilos: emulación con prioridad %d, presentación con 1001", EMU_PRIO);
	/* Avisos y atajos con SELECT (estados guardados, contador de FPS). */
	ui_init_once();
	fps.on = fps_setting_load();
	osd_fps_reset(&fps);
	pause_ctl.confirm = confirm_setting_load();
	sys_no_pause = file_exists(NO_PAUSE_PATH);
	if (sys_no_pause)
		salsa64_log("pausa: %s presente, el menú del sistema no pausa la emulación (diagnóstico)", NO_PAUSE_PATH);
	combos_on = 1;
	salsa64_log("estados: %s/<juego>.st0 … .st%d; SELECT + R1 guarda, + L1 carga, + izquierda/derecha cambia de "
		    "ranura; FPS %s (SELECT + △); SELECT + R2 vuelca la HLE; START + SELECT %s", STATES_DIR,
		    SALSA64_STATE_SLOTS - 1, fps.on ? "sí" : "no",
		    pause_ctl.confirm ? "pregunta antes de salir" : "sale directo (confirmar = no)");

	uint64_t now = sysGetSystemTime();
	pres.t_shot = now;
	stats(now);
	while (running) {
		beat.pres++;
		beat.pres_at = "espera";
		busy_stop(&h1.busy, tb_now());
		sysSemWait(wake_sem, WAKE_US);
		busy_start(&h1.busy, tb_now());
		beat.pres_at = "sysutil";
		sysUtilCheckCallback();
		beat.pres_at = "mandos";
		if (update_pads(NULL)) {
			salsa64_log("menú de pausa%s: %s", pause_ctl.confirm ? " y confirmación" : "", in_menu ? "vuelta al menú" : "salida");
			break;
		}
		pause_update();
		now = sysGetSystemTime();
		filter_cycle(now);
		beat.pres_at = "guardado";
		savefile_write_staged(&save_file, now);
		profile_write_step(now);
		/* Con el sistema encima (su menú, un aviso), el sistema dibuja lo
		 * suyo en cada flip de la aplicación: el último cuadro se sigue
		 * presentando, pero solo si el flip anterior ya salió, para no
		 * esperarlo. Los campos quedan en cola (emulación en pausa); los
		 * eventos y los mandos, cada vuelta. */
		bool overlay = pausectl_overlay(&pause_ctl);
		uint32_t shown = pres.frames_total;
		if (!overlay) {
			beat.pres_at = "cola";
			drain(now, 1);
			if (h1.load) {
				uint64_t t = tb_now();
				if (t >= h1.load_next) {
					h1.load_phase = (h1.load_phase + 1) % LOAD_PHASES;
					h1.load_next = t + spukit_tick_hz() / 2;
				}
				uint32_t us = h1.load_us[h1.load_phase] * (pres.frames_total - shown);
				if (us)
					h1_load_run(us);
			}
			if (state_parked && state_req) {
				beat.pres_at = "barrera";
				state_barrier();
			}
		} else {
			beat.pres_at = "sistema encima";
		}
		/* En pausa: el último cuadro otra vez si no llegó ninguno. */
		if (pause_req && pres.frames_total == shown && sysGetSystemTime() - pres.t_flip >= STILL_US &&
		    (!overlay || rsxkit_frame_ready()))
			present_still();
		if (state_test.stage)
			state_test_step();
		if (fps.on)
			osd_fps_sample(&fps, now, n64.bus.vi.fields, n64.cpu.cycles, pres.game_frames, pres.frames_total);
		stats(now);
	}
	beat.pres_at = "cierre";
	combos_on = 0;
	if (app_quit)
		salsa64_log("«Salir del juego» del sistema");
	if (pause_req)
		salsa64_log("pausa: el juego se cierra en pausa (%.1f s)", (sysGetSystemTime() - pause_log.t0) / 1e6);
	pausectl_game_end(&pause_ctl);

	/* Parar la emulación sin dejarla esperando un juego de búferes (si
	 * estaba en pausa, running = 0 la suelta). */
	running = 0;
	while (!emu_done) {
		drain(sysGetSystemTime(), 0);
		sysUsleep(500);
	}
	u64 ret;
	sysThreadJoin(emu_tid, &ret);
	emu_live = 0;
	drain(sysGetSystemTime(), 0);
	game_close();
	rdp_scale_set(video_cfg.scale, "vuelve la global");  /* la de un juego o la del menú de pausa */
	return app_quit || !in_menu ? GAME_QUIT : GAME_TO_MENU;
}

/* ---- Menú ------------------------------------------------------------------------- */

static const char *const rom_dirs[] = {
	SALSA64_DATA_DIR,
	"/dev_usb000/salsa64", "/dev_usb001/salsa64", "/dev_usb002/salsa64", "/dev_usb003/salsa64",
	"/dev_usb004/salsa64", "/dev_usb005/salsa64", "/dev_usb006/salsa64", "/dev_usb007/salsa64",
};

static void video_load(struct menu_settings *set);
static void video_save(const struct menu_settings *set);

static struct romlist roms;

/* Título y código de una ROM de la lista (NULL si no está: p. ej. sin menú). */
static void rom_names(const char *path, const char **title, const char **code, uint32_t *crc1, uint32_t *crc2)
{
	int i = romlist_find(&roms, path);
	if (i < 0)
		return;
	*title = romlist_display_title(&roms.e[i]);
	*code = roms.e[i].code;
	*crc1 = roms.e[i].crc1;
	*crc2 = roms.e[i].crc2;
}
static struct menu menu;
static struct cheat_set menu_cheats;  /* los de la ROM cuya página está abierta */
static struct {
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
	const rsxkit_screen *osc = rsxkit_screen_info();
	menu.out_w = osc->width;
	menu.out_h = osc->height;
	menu.res_run = video_cfg.resolution;
	menu.scale_run = video_cfg.scale;
	menu.dl_enabled = dl_enabled();  /* experimental: solo con descargas.cfg «activar = si» */
	menu.dirs[menu.ndirs++] = SALSA64_DATA_DIR;
	menu.dirs[menu.ndirs++] = "/dev_usb000/salsa64 … /dev_usb007/salsa64";
	menu.version = version_text;
	menu.build = build_text[0] ? build_text : NULL;
}

static const rsxkit_config *video_config_for(int resolution, rsxkit_config *cfg);

/* Espacio lógico del menú según la pantalla en curso. */
static void menu_layout(void)
{
	mn.lh = 720.0f;
	mn.lw = rsxkit_screen_info()->aspect > 1.5f ? 1280.0f : 960.0f;
}

/* «Aplicar ahora»: pone la salida y la escala elegidas sin cerrar Salsa64.
 * No hay juego abierto (el RDP quedó limpio en game_close). Se rehacen solo
 * los búferes de pantalla del RSX (rsxkit_video_reconfigure: el contexto, la
 * memoria mapeada, el 2D del menú y el framebuffer se conservan) y la escala
 * del RDP (salsa64_rdp_set_scale: conserva su memoria mapeada). Si la tele no
 * acepta el modo, rsxkit deja el anterior y la escala no se toca. */
static void menu_apply_video(void)
{
	const struct menu_settings want = menu.set;
	const rsxkit_screen *sc = rsxkit_screen_info();
	unsigned old_w = sc->width, old_h = sc->height;
	int rc = RSXKIT_OK;
	rsxkit_config vcfg;

	/* Solo se toca el video si cambió la resolución pedida. */
	if (want.resolution != video_cfg.resolution) {
		rsxkit_finish();
		rc = rsxkit_video_reconfigure(video_config_for(want.resolution, &vcfg));
	}
	if (rc != RSXKIT_OK) {
		salsa64_log("video: no se pudo cambiar a %s (%d); sigue %ux%u, escala %u×", want.resolution == MENU_RES_1080P ? "1080p" : "720p",
			    rc, old_w, old_h, salsa64_rdp_scale(NULL));
		snprintf(menu.note, sizeof menu.note, tr("No se pudo: sigue %ux%u"), old_w, old_h);
		return;
	}
	salsa64_rdp_set_scale((uint32_t)want.scale);
	rdp_scale_req = want.scale;
	bool cut;
	uint32_t es = salsa64_rdp_scale(&cut);
	video_cfg.resolution = want.resolution;
	video_cfg.scale = want.scale;
	sc = rsxkit_screen_info();
	menu_layout();
	menu.out_w = sc->width;
	menu.out_h = sc->height;
	menu.res_run = want.resolution;
	menu.scale_run = want.scale;
	snprintf(menu.note, sizeof menu.note, tr("Aplicado: %ux%u, escala %u×"), sc->width, sc->height, es);
	salsa64_log("video: %ux%u, escala %u× (%s%s), aplicado sin reiniciar", sc->width, sc->height, es,
		    want.scale ? "pedida" : "automática", cut ? ", reducida por memoria del RSX" : "");
	if (want.resolution == MENU_RES_1080P && sc->height != 1080)
		salsa64_log("video: se pidió 1080p y la tele dio %ux%u", sc->width, sc->height);
}

/* Un cuadro del menú: todo lo dibuja el RSX (rsxkit/ui.h); el PPE solo
 * arma unos miles de vértices. Con `shot`, además una captura (a la mitad
 * de la resolución de 720p) en ese archivo. */
/* Carátula de la ROM elegida: cuando la selección lleva quieta un momento la
 * busca en COVERS_DIR, la decodifica y la sube a la ranura 0 de la UI (entre
 * cuadros, que es cuando rsxkit_ui_image_set puede esperar al RSX). */
#define COVER_SETTLE_US 200000u
static uint8_t cover_rgba[COVER_RGBA_BYTES];
static int cover_tried = -1;
static uint64_t cover_since;

static void menu_cover_update(void)
{
	if (!menu_settled(&menu)) {
		cover_since = 0;
		return;
	}
	uint64_t now = sysGetSystemTime();
	int s = menu_selected(&menu);
	if (!cover_since)
		cover_since = now;
	if (now - cover_since < COVER_SETTLE_US || s == menu.cover_rom || s == cover_tried)
		return;
	cover_tried = s;
	const struct rom_entry *e = &roms.e[s];
	char path[256];
	if (!cover_find(COVERS_DIR, romlist_name(e), romlist_display_title(e), e->code, path, sizeof path)) {
		salsa64_log("carátula: ninguna para %s (título «%s», código %s)", romlist_name(e), romlist_display_title(e),
			    e->code[0] ? e->code : "-");
		return;
	}
	unsigned w = 0, h = 0;
	enum cover_result r = cover_load_png(path, cover_rgba, &w, &h);
	if (r != COVER_OK) {
		salsa64_log("carátula: %s: %s", path, cover_result_name(r));
		return;
	}
	int rc = rsxkit_ui_image_set(0, cover_rgba, w, h);
	if (rc != RSXKIT_OK) {
		salsa64_log("carátula: %s: no se pudo subir (%d)", path, rc);
		return;
	}
	menu.cover_w = w;
	menu.cover_h = h;
	menu.cover_rom = s;
	salsa64_log("carátula: %s (%ux%u)", path, w, h);
}

/* Descargas (download.h): lo que se va viendo en el pie del menú. */
static bool dl_shown;

static void download_begin(int what)
{
	static struct dl_rom list[ROMLIST_MAX];
	if (!roms.count) {
		menu_set_status(&menu, "No encontré ROMs (.z64, .v64, .n64) en:");
		return;
	}
	unsigned n = 0;
	for (uint32_t i = 0; i < roms.count; i++) {
		const struct rom_entry *e = &roms.e[i];
		snprintf(list[n].name, sizeof list[n].name, "%s", romlist_name(e));
		snprintf(list[n].title, sizeof list[n].title, "%s", romlist_display_title(e));
		snprintf(list[n].code, sizeof list[n].code, "%s", e->code);
		n++;
	}
	if (!dl_start(what, list, n)) {
		menu_set_status(&menu, "No se pudo bajar el índice: %s", "hilo");
		return;
	}
	dl_shown = true;
	menu_set_status(&menu, what ? "Descargando trucos…" : "Descargando carátulas…");
}

/* Una vez por cuadro del menú: avance, cancelación con ○ y resultado. */
static void download_poll(uint32_t buttons)
{
	if (!dl_shown)
		return;
	struct dl_status st;
	dl_get(&st);
	if (st.state == DL_RUNNING) {
		if (buttons & (1u << SALSA64_DS3_CIRCLE))
			dl_cancel();
		char line[MENU_STATUS_MAX];
		snprintf(line, sizeof line, tr("Descargando… %u de %u"), st.done, st.total);
		menu_set_status(&menu, "%s   %s", line, tr("○  Cancelar"));
		return;
	}
	dl_shown = false;
	dl_shutdown();
	if (st.state == DL_FAILED) {
		menu_set_status(&menu, "%s", st.err);
	} else if (st.state == DL_CANCELLED) {
		menu_set_status(&menu, "Descarga cancelada");
	} else {
		menu_set_status(&menu, "%s: %u nuevas, %u ya estaban, %u sin coincidencia", tr(st.what ? "Trucos" : "Carátulas"),
				st.got, st.have, st.missing);
		if (!st.what && st.got)
			menu.cover_rom = cover_tried = -1;  /* a buscar las carátulas de nuevo */
	}
}

static void menu_show(const char *shot)
{
	frame_begin("menú: flip anterior");
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
	frame_end("menú: flip");
}

static void settings_read(struct menu_settings *s)
{
	char text[512];
	menu_settings_default(s);
	FILE *f = fopen(MENU_CFG_PATH, "r");
	if (!f)
		return;
	size_t n = fread(text, 1, sizeof text - 1, f);
	fclose(f);
	menu_settings_parse(s, text, n);
}

static bool settings_write(const struct menu_settings *s)
{
	char text[512];
	int n = menu_settings_format(s, text, sizeof text);
	FILE *f = fopen(MENU_CFG_PATH, "w");
	bool ok = f && fwrite(text, 1, (size_t)n, f) == (size_t)n;
	if (f && fclose(f) != 0)
		ok = false;
	return ok;
}

static void menu_settings_load(void)
{
	settings_read(&mn.settings);
	i18n_set(mn.settings.lang);
}

static void menu_settings_save(void)
{
	bool ok = settings_write(&mn.settings);
	salsa64_log("menú: fondo animado %s, FPS %s, confirmar al salir %s, filtro %s%s%s", mn.settings.waves ? "sí" : "no",
		    mn.settings.fps ? "sí" : "no", mn.settings.confirm ? "sí" : "no", menu_filter_name(mn.settings.filter),
		    menu_filter_has_density(mn.settings.filter) ? (mn.settings.density == MENU_DENSITY_FINE ? ", densidad fina" : ", densidad normal") : "",
		    ok ? "" : " (no se pudo guardar)");
}

/* El contador de velocidad se recuerda en menu.cfg («fps = si|no»), también
 * con embedded (sin menú): se relee el archivo y se cambia solo esa clave. */
static bool fps_setting_load(void)
{
	struct menu_settings s;
	settings_read(&s);
	return s.fps;
}

/* «Confirmar al salir» (menu.cfg, «confirmar = si|no»; sí sin el archivo),
 * también con embedded. */
static bool confirm_setting_load(void)
{
	struct menu_settings s;
	settings_read(&s);
	return s.confirm;
}

static void fps_setting_save(bool on)
{
	struct menu_settings s;
	settings_read(&s);
	s.fps = on;
	mn.settings.fps = on;
	salsa64_log("FPS: %s%s", on ? "sí" : "no", settings_write(&s) ? "" : " (no se pudo guardar en menu.cfg)");
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

/* Línea «@video 720|1080 escala» de menu-prueba: «Aplicar ahora» con esos
 * valores, sin tocar video.cfg. Cuenta como un paso del conjunto. */
static void menu_test_video(const char *line)
{
	int res = 0, scale = 0;
	bool ok = sscanf(line, "@video %d %d", &res, &scale) == 2 && (res == 720 || res == 1080) && scale >= 0 &&
		  scale <= 5;
	if (ok) {
		menu.set.resolution = res == 1080 ? MENU_RES_1080P : MENU_RES_720P;
		menu.set.scale = scale;
		menu_apply_video();
		ok = !menu_video_pending(&menu) && (res == 1080 || rsxkit_screen_info()->height == 720) &&
		     (!scale || salsa64_rdp_scale(NULL) <= (uint32_t)scale);
	}
	salsa64_log("menú, prueba automática: «%s»: %s", line, ok ? "bien" : "MAL");
	mn.games++;
	mn.failures += !ok;
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
	/* Búfer a la medida de la ROM, solo mientras dura el juego. */
	beat.pres_at = "menú: leyendo la ROM";
	uint8_t *rom = read_rom_file(path, NULL, 0, &size);
	beat.pres_at = "arranque del juego";
	if (!rom) {
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
	enum game_end end = run_game(rom, size, path, err, sizeof err);
	free(rom);  /* run_game ya cerró el juego: nada apunta a la ROM */
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
	int rc = ui_init_once();
	if (rc != RSXKIT_OK) {
		salsa64_log("%s: FALLO al preparar el menú (2D del RSX %d)", pres.autotest ? "RESULTADO frontend" : "menú",
			    rc);
		return 1;
	}
	menu_layout();
	menu_settings_load();
	load_overscan();
	mn.settings.overscan = overscan_preset();
	{
		struct menu_settings v = mn.settings;
		video_load(&v);
		mn.settings.resolution = v.resolution;
		mn.settings.scale = v.scale;
	}
	salsa64_log("menú: espacio lógico de %.0fx%.0f sobre %ux%u, ROM con búfer a su medida (máx. %u MB), fondo animado %s", mn.lw,
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
		beat.pres++;
		beat.pres_at = "menú: sysutil";
		sysUtilCheckCallback();
		beat.pres_at = "menú: mandos";
		uint32_t buttons;
		update_pads(&buttons);
		beat.pres_at = "menú";
		/* Con el sistema encima el menú sigue presentándose (el sistema
		 * dibuja el suyo en cada flip de la aplicación: sin flips se queda
		 * en el reloj de espera), pero solo si el flip anterior ya salió,
		 * para no dejar de atender sus eventos. */
		if (pausectl_overlay(&pause_ctl) && !rsxkit_frame_ready()) {
			beat.pres_at = "menú: sistema encima";
			sysUsleep(OVERLAY_US);
			continue;
		}
		if (pres.autotest && mn.next_test < mn.ntest && mn.test[mn.next_test][0] == '@') {
			menu_test_video(mn.test[mn.next_test++]);
			continue;
		}
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
			if (menu.set.resolution != mn.settings.resolution || menu.set.scale != mn.settings.scale)
				video_save(&menu.set);
			/* Todo lo de menu.cfg (antes, la densidad no estaba en la
			 * lista y se perdía al abrir el juego). */
			bool cfg = menu_settings_cfg_differs(&menu.set, &mn.settings);
			mn.settings = menu.set;
			if (cfg) {
				i18n_set(mn.settings.lang);
				menu_settings_save();
			}
		}
		if (a == MENU_APPLY)
			menu_apply_video();
		if (a == MENU_CHEATS) {
			const struct rom_entry *e = &roms.e[menu_selected(&menu)];
			if (cheats_load(&menu_cheats, CHEATS_DIR, romlist_name(e), romlist_display_title(e), e->code, e->crc1, e->crc2)) {
				menu_cheats_open(&menu, &menu_cheats, romlist_display_title(e));
				if (menu_cheats.crc_mismatch)
					menu_set_status(&menu, "Ojo: el CRC de la ROM no es el de esos trucos");
			} else {
				const char *f = strrchr(menu_cheats.cht_path, '/');
				menu_set_status(&menu, "No hay trucos: pon cheats/%s", f ? f + 1 : menu_cheats.cht_path);
				salsa64_log("trucos: ninguno para %s (%s)", romlist_name(e), menu_cheats.cht_path);
			}
		}
		if (a == MENU_GAME) {
			const struct rom_entry *e = &roms.e[menu_selected(&menu)];
			struct menu_game g;
			game_cfg_read(romlist_name(e), &g);
			menu_game_open(&menu, &g, romlist_display_title(e));
		}
		if (a == MENU_GAME_CHANGED) {
			const struct rom_entry *e = &roms.e[menu.game_rom];
			bool ok = game_cfg_write(romlist_name(e), &menu.game);
			salsa64_log("ajustes de %s: aspecto %d, filtro %d, densidad %d, recorte %d, escala %d%s", romlist_name(e),
				    menu.game.aspect, menu.game.filter, menu.game.density, menu.game.overscan, menu.game.scale,
				    ok ? "" : " (no se pudo guardar)");
			if (!ok)
				menu_set_status(&menu, "No se pudieron guardar los ajustes");
		}
		if (a == MENU_CHEATS_CHANGED) {
			bool ok = cheats_save(&menu_cheats);
			salsa64_log("trucos: %u activos de %u%s", cheats_active(&menu_cheats), menu_cheats.n,
				    ok ? "" : " (no se pudo guardar)");
			if (!ok)
				menu_set_status(&menu, "No se pudieron guardar los trucos");
		}
		if (a == MENU_DOWNLOAD)
			download_begin(menu.dl_what);
		download_poll(buttons);
		if (a == MENU_QUIT) {
			salsa64_log("menú: «Salir al XMB»");
			break;
		}
		if (a == MENU_OPEN) {
			dl_shutdown();
			dl_shown = false;
			if (menu_open(&roms.e[menu_selected(&menu)]) == GAME_QUIT)
				break;
			mn.next_test++;
			mn.step = mn.idle = 0;
			menu.cover_rom = cover_tried = -1;
		}
		menu_cover_update();
		menu_show(want_shot && ++frames == 45 ? SALSA64_DATA_DIR "/menu.ppm" : NULL);
	}
	dl_shutdown();
	if (rsxkit_ui_dropped())
		salsa64_log("menú: %u quads no cupieron en algún cuadro", rsxkit_ui_dropped());
	if (!pres.autotest)
		return 0;
	bool ok = !failed && !mn.failures && mn.games == (int)mn.ntest;
	salsa64_log("RESULTADO frontend: %s (menú: %d juegos abiertos y cerrados, %d con fallas)", ok ? "OK" : "FALLO",
		    mn.games, mn.failures);
	return ok ? 0 : 1;
}

/* video.cfg: «salida = 1080» prefiere 1080p (también la línea sola «1080» del
 * formato viejo); «escala = auto|1..5» es el tamaño del render de la N64
 * (auto: 4× a 1080p, 3× a 720p). Sin archivo, 720p y escala automática.
 * Si la tele no acepta 1080, sigue con el siguiente modo de la lista. */
static const int32_t modes_1080[] = {
	VIDEO_RESOLUTION_1080, VIDEO_RESOLUTION_720, VIDEO_RESOLUTION_480, VIDEO_RESOLUTION_576,
};

static void video_load(struct menu_settings *set)
{
	char text[256];
	size_t n = 0;
	FILE *f = fopen(VIDEO_CFG_PATH, "r");
	if (f) {
		n = fread(text, 1, sizeof text - 1, f);
		fclose(f);
	}
	menu_video_parse(set, text, n);
}

/* Opciones «Resolución» y «Escala interna» del menú: video.cfg con
 * «salida» y «escala». Se leen al arrancar; «Aplicar ahora» los pone sin
 * reiniciar (menu_apply_video). */
static void video_save(const struct menu_settings *set)
{
	char text[256];
	int n = menu_video_format(set, text, sizeof text);
	FILE *f = fopen(VIDEO_CFG_PATH, "w");
	bool ok = f && fwrite(text, 1, (size_t)n, f) == (size_t)n;
	if (f && fclose(f) != 0)
		ok = false;
	if (!ok)
		salsa64_log("menú: no se pudo escribir %s", VIDEO_CFG_PATH);
}

static const rsxkit_config *video_config_for(int resolution, rsxkit_config *cfg)
{
	if (resolution != MENU_RES_1080P)
		return NULL;
	memset(cfg, 0, sizeof *cfg);
	cfg->modes = modes_1080;
	cfg->num_modes = sizeof modes_1080 / sizeof modes_1080[0];
	return cfg;
}

/* Latido: cada WATCH_US mira que presentación (juego o menú) y emulación
 * (cuando corre: no en pausa ni en la barrera de un estado) sigan dando
 * vueltas. Si una se queda quieta más de STALL_US, al log dónde está cada
 * una (beat.*_at) y lo que hay en vuelo, y otra vez cada STALL_LOG_US: un
 * cuelgue (el del botón PS, 2026-10-11) deja su huella en el log UDP aunque
 * el de archivo pierda las últimas líneas. Solo lee; con más prioridad que
 * los dos para correr aunque uno gire. */
static void watch_thread(void *arg)
{
	(void)arg;
	uint32_t pres0 = beat.pres, emu0 = beat.emu;
	uint64_t now = sysGetSystemTime(), pres_t = now, emu_t = now, pres_said = 0, emu_said = 0;
	while (!beat.quit) {
		sysUsleep(WATCH_US);
		now = sysGetSystemTime();
		if (beat.pres != pres0 || !beat.pres) {  /* antes de la primera vuelta: el arranque */
			if (pres_said)
				salsa64_log("latido: presentación se movió de nuevo tras %.1f s", (now - pres_t) / 1e6);
			pres0 = beat.pres;
			pres_t = now;
			pres_said = 0;
		} else if (now - pres_t >= STALL_US && (!pres_said || now - pres_said >= STALL_LOG_US)) {
			pres_said = now;
			salsa64_log("latido: presentación quieta hace %.1f s en «%s» (emulación en «%s»; campo %u, %u en "
				    "cola; sistema %s, pausa 0x%x, emulación %s; audio %llu bloques)",
				    (now - pres_t) / 1e6, beat.pres_at, emu_live ? beat.emu_at : "sin juego", n64.bus.vi.fields,
				    field_head - field_tail, pausectl_overlay(&pause_ctl) ? "encima" : "sin dibujar", pause_req,
				    emu_paused ? "quieta" : "andando", (unsigned long long)audio_out_blocks());
		}
		bool expect = emu_live && running && !emu_done && !emu_paused && !state_parked && !pause_req;
		if (!expect || beat.emu != emu0) {
			if (emu_said && expect)
				salsa64_log("latido: emulación se movió de nuevo tras %.1f s", (now - emu_t) / 1e6);
			emu0 = beat.emu;
			emu_t = now;
			emu_said = 0;
		} else if (now - emu_t >= STALL_US && (!emu_said || now - emu_said >= STALL_LOG_US)) {
			emu_said = now;
			salsa64_log("latido: emulación quieta hace %.1f s en «%s» (campo %u, %u emulados, %u en cola, RSP "
				    "%s, %u tareas gráficas terminadas, %u esperas al SPE; presentación en «%s»; pausa 0x%x)",
				    (now - emu_t) / 1e6, beat.emu_at, n64.bus.vi.fields, emu_stats.fields,
				    field_head - field_tail, n64.bus.rsp.busy ? "ocupado" : "libre", salsa64_rdp_tasks_done(),
				    gfx_spu.waits, beat.pres_at, pause_req);
		}
	}
	sysThreadExit(0);
}

int main(void)
{
	char banner[192];
	version_banner(banner, sizeof banner);
	salsa64_log_init_banner("salsa64", banner);
	version_label(salsa64_version, salsa64_commit, version_text, sizeof version_text);
	if (!version_build(salsa64_title, salsa64_appid, build_text, sizeof build_text))
		build_text[0] = '\0';
	pres.autotest = salsa64_autotest();
	sys_ppu_thread_t watch_tid;
	bool watching = sysThreadCreate(&watch_tid, watch_thread, NULL, WATCH_PRIO, 0x10000, THREAD_JOINABLE,
					(char *)"salsa64_latido") == 0;
	if (!watching)
		salsa64_log("latido: no se pudo crear su hilo");
	pausectl_init(&pause_ctl, true);

	menu_settings_default(&video_cfg);
	video_load(&video_cfg);
	rsxkit_config vcfg;
	const rsxkit_config *vc = video_config_for(video_cfg.resolution, &vcfg);
	int rc = rsxkit_init(vc);
	pres.fb = rc == RSXKIT_OK ? rsxkit_fb_create(FB_MAX_W, FB_MAX_H) : NULL;
	if (!pres.fb) {
		salsa64_log("RESULTADO frontend: FALLO al iniciar el RSX (%d)", rc);
		salsa64_log_shutdown();
		return 1;
	}
	const rsxkit_screen *sc = rsxkit_screen_info();
	salsa64_log("RSX listo: %ux%u, tele %s%s", sc->width, sc->height, sc->aspect > 1.5f ? "16:9" : "4:3",
		    vc ? " (video.cfg: 1080)" : "");
	rdp_scale_req = video_cfg.scale;
	if ((rc = salsa64_rdp_init((uint32_t)video_cfg.scale)) != RSXKIT_OK) {
		salsa64_log("RESULTADO frontend: FALLO al preparar el RDP (%d)", rc);
		rsxkit_shutdown();
		salsa64_log_shutdown();
		return 1;
	}
	{
		bool cut;
		uint32_t es = salsa64_rdp_scale(&cut);
		salsa64_log("RDP listo: escala-objetivo %u× (%u líneas por imagen; cada destino, la entera que llega ahí), %s%s", es, 240 * es,
			    video_cfg.scale ? "pedida" : "automática", cut ? ", reducida por memoria del RSX" : "");
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

	rdram = NULL;
	if (file_exists(RDRAM_1M_PATH)) {
		sys_mem_addr_t addr = 0;
		if (sysMemoryAllocate(SALSA64_RDRAM_MAX, SYS_MEMORY_PAGE_SIZE_1M, &addr) == 0)
			rdram = (uint8_t *)(uintptr_t)addr;
		salsa64_log("RDRAM con sysMemoryAllocate de 1 MB (rdram-1m): %s", rdram ? "bien" : "FALLO, por el heap");
	}
	if (!rdram)
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
	/* Efectos de framebuffer: copias RSX → RDRAM y el despertar del hilo de
	 * presentación cuando una tarea las necesita (docs/framebuffer.md). */
	salsa64_rdp_set_memory(rdram, SALSA64_RDRAM_MAX, wake);
	salsa64_rdp_set_log(salsa64_log);
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
	if (watching) {
		u64 rv;
		beat.quit = 1;
		sysThreadJoin(watch_tid, &rv);
	}
	salsa64_log("salida normal%s", app_quit ? " («Salir del juego» del sistema)" : "");
	salsa64_log_shutdown();
	return status;
}
