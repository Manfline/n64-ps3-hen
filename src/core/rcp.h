/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — registros del RCP mapeados en memoria (MI, SP, DP, VI, AI, PI,
 * RI, SI) y de la RDRAM.
 *
 * Estado de la Fase 1 (primer hito):
 *   MI  completo: modo, versión, interrupciones y máscara → Cause.IP2.
 *   PI  DMA cartucho ↔ RDRAM inmediato, con interrupción; SRAM en save.c.
 *       Sin latencias.
 *   SI  DMA RDRAM ↔ PIF RAM inmediato, con interrupción; comandos joybus
 *       de los controles en pif.c y de la EEPROM en save.c. Falta la latencia.
 *   SP  bits de SP_STATUS, semáforo y DMA inmediato. El RSP no ejecuta
 *       microcódigo: al arrancarlo se lee la tarea (OSTask) de la DMEM y
 *       las gráficas van a la HLE (rsp.c y el backend de src/rsp/gfx).
 *   VI  contador de líneas, interrupción en VI_V_INTR y campos con la
 *       temporización del tipo de TV (vi.c); describe el framebuffer que
 *       se ve (salsa64_vi_frame). Sin filtros (AA, divot, gamma).
 *   AI  DMA con la cola de dos lugares, temporización por AI_DACRATE y el
 *       reloj del VI, interrupción al empezar cada DMA y el error del
 *       acarreo en los límites de 8 KB (ai.c); las muestras van a un
 *       sumidero que pone el frontend.
 *   DP, RI, RDRAM: solo guardan los registros. Falta la temporización.
 *
 * Cada bloque recibe el desplazamiento del registro dentro de su página
 * (paddr & 0xFFFFF) y accesos de 32 bits.
 */
#ifndef SALSA64_CORE_RCP_H
#define SALSA64_CORE_RCP_H

#include "core/rom.h"
#include "core/types.h"

struct salsa64_bus;
struct salsa64_sched;

/* Líneas de interrupción del MI (bits de MI_INTR y MI_INTR_MASK). */
enum salsa64_irq {
	SALSA64_IRQ_SP = 0,
	SALSA64_IRQ_SI = 1,
	SALSA64_IRQ_AI = 2,
	SALSA64_IRQ_VI = 3,
	SALSA64_IRQ_PI = 4,
	SALSA64_IRQ_DP = 5,
};

#define SALSA64_MI_VERSION_VALUE 0x02020102u

struct salsa64_mi {
	uint32_t mode;
	uint32_t intr;
	uint32_t mask;
};

enum salsa64_sp_reg {
	SALSA64_SP_MEM_ADDR, SALSA64_SP_DRAM_ADDR, SALSA64_SP_RD_LEN,
	SALSA64_SP_WR_LEN, SALSA64_SP_STATUS, SALSA64_SP_DMA_FULL,
	SALSA64_SP_DMA_BUSY, SALSA64_SP_SEMAPHORE, SALSA64_SP_NREGS
};

#define SALSA64_SP_STATUS_HALT       0x0001u
#define SALSA64_SP_STATUS_BROKE      0x0002u
#define SALSA64_SP_STATUS_INTR_BREAK 0x0040u
#define SALSA64_SP_STATUS_SIG(n)     (0x0080u << (n))
/* Señales que usa libultra: 0 pedir ceder, 1 cedió, 2 tarea terminada. */
#define SALSA64_SP_STATUS_TASKDONE   SALSA64_SP_STATUS_SIG(2)

struct salsa64_sp {
	uint32_t regs[SALSA64_SP_NREGS];
	uint32_t pc;
	uint32_t pc_writes;  /* escrituras de SP_PC por el CPU (el RSP a bajo nivel distingue un PC nuevo) */
};

/* ---- Tareas del RSP (rsp.c) ------------------------------------------------
 *
 * libultra deja la tarea (OSTask) en DMEM 0xFC0 antes de arrancar el RSP.
 * Las gráficas y las de audio se procesan por HLE en backends
 * intercambiables (en la PS3, un SPE cada una; en el host, en el mismo
 * hilo); el resto de las tareas terminan sin hacer nada por ahora. La
 * tarea termina un tiempo fijo después de
 * arrancar (SP_STATUS con BROKE, HALT y TASKDONE, interrupción SP) y
 * después llega la del DP si la lista pidió sincronizar el RDP, si la HLE
 * la cortó antes (SALSA64_GFX_CUT) o si no hay HLE para su microcódigo:
 * el juego la espera por cada tarea gráfica. */

#define SALSA64_TASK_DMEM 0xFC0u

enum salsa64_task_type {
	SALSA64_TASK_GFX = 1,
	SALSA64_TASK_AUDIO = 2,
	SALSA64_TASK_JPEG = 4,   /* M_NJPEGTASK: decodificador JPEG (rsp/jpeg) */
};

/* Familia de microcódigo gráfico (codificación de la lista de comandos). */
enum salsa64_gbi {
	SALSA64_GBI_UNKNOWN = 0,
	SALSA64_GBI_F3DEX2,   /* F3DEX2, F3DLX2…: segunda generación */
	SALSA64_GBI_F3DZEX2,  /* F3DZEX (Zelda OoT/MM): F3DEX2 con G_BRANCH_W */
	SALSA64_GBI_F3D,      /* Fast3D: Super Mario 64 */
	SALSA64_GBI_F3DEX,    /* F3DEX 1.x (F3DLX, F3DLP…) */
	SALSA64_GBI_S2DEX,    /* sprites 2D (pendiente) */
	SALSA64_GBI_F3DPD,    /* Perfect Dark: Fast3D de Rare, vértices con color por índice */
	SALSA64_GBI_F3DEXBG,  /* Conker: F3DEX2 con G_TRI4, normales aparte y luces puntuales */
	SALSA64_GBI_F3DWR,    /* Wave Race 64: Fast3D 2.0D con índices de vértice ×5 y G_QUAD en 0xB5 */
};

/* Campos de OSTask (16 palabras en DMEM 0xFC0). */
struct salsa64_task {
	uint32_t type, flags;
	uint32_t ucode_boot, ucode_boot_size;
	uint32_t ucode, ucode_size;
	uint32_t ucode_data, ucode_data_size;
	uint32_t dram_stack, dram_stack_size;
	uint32_t output_buff, output_buff_size;
	uint32_t data_ptr, data_size;
	uint32_t yield_data_ptr, yield_data_size;
};

/* Lo que devuelve gfx_finish. */
#define SALSA64_GFX_FULLSYNC 0x1u  /* la lista terminó con G_RDPFULLSYNC */
#define SALSA64_GFX_CUT      0x2u  /* la HLE la cortó antes del fin: DP de todos modos */

struct salsa64_gfx_backend {
	void *ctx;
	/* Empieza a procesar la tarea; puede volver antes de terminarla. */
	void (*start)(void *ctx, const struct salsa64_task *task, enum salsa64_gbi gbi);
	/* Espera a que termine (cuando la máquina llega al fin de la tarea) y
	 * devuelve SALSA64_GFX_*. */
	uint32_t (*finish)(void *ctx);
	/* Opcional: ¿ya terminó? Sin bloquear. Si lo hay, el fin de la tarea
	 * se vuelve a programar cada poll_cycles hasta que diga que sí, y el
	 * CPU sigue emulando entre medias. `idle` avisa que el CPU está en un
	 * bucle de espera (saltó ciclos desde el sondeo anterior): el backend
	 * puede dormir un poco en vez de volver en el acto, para que el reloj
	 * emulado no corra más rápido que el real. */
	bool (*done)(void *ctx, bool idle);
	/* Opcionales (estados guardados, core/state.c), sin tarea en vuelo: el
	 * estado que la HLE conserva entre tareas (el del RDP y la TMEM) en un
	 * formato portable de state_size bytes, y de vuelta. */
	uint32_t state_size;
	bool (*save_state)(void *ctx, uint8_t *buf);
	bool (*load_state)(void *ctx, const uint8_t *buf);
};

/* ABI de las listas de audio (alist): cada microcódigo de audio numera y
 * codifica sus comandos a su manera. Se reconoce por unas palabras de su
 * segmento de datos (rsp_audio.c). Los juegos objetivo: Super Mario 64
 * ABI1; GoldenEye ABI1 (GE); Zelda OoT, Mario Kart 64 y Star Fox 64 nead;
 * Perfect Dark naudio con MP3. */
enum salsa64_abi {
	SALSA64_ABI_UNKNOWN = 0,
	SALSA64_ABI_AUDIO,      /* ABI1 de libultra (Super Mario 64 y muchos) */
	SALSA64_ABI_AUDIO_GE,   /* GoldenEye: ENVMIXER lineal */
	SALSA64_ABI_AUDIO_BC,   /* Blast Corps, Diddy Kong Racing */
	SALSA64_ABI_NEAD_MK,    /* Mario Kart 64, Wave Race (E) */
	SALSA64_ABI_NEAD_SF,    /* Star Fox 64 (salvo J) */
	SALSA64_ABI_NEAD_SFJ,   /* Star Fox 64 (J) */
	SALSA64_ABI_NEAD_WRJB,  /* Wave Race (J RevB) */
	SALSA64_ABI_NEAD_FZ,    /* F-Zero X */
	SALSA64_ABI_NEAD_YS,    /* Yoshi's Story */
	SALSA64_ABI_NEAD_1080,  /* 1080° Snowboarding */
	SALSA64_ABI_NEAD_OOT,   /* Zelda OoT, Zelda MM (J) */
	SALSA64_ABI_NEAD_MM,    /* Zelda MM, Pokémon Stadium 2 */
	SALSA64_ABI_NEAD_MMB,   /* Zelda MM (E beta) */
	SALSA64_ABI_NEAD_AC,    /* Animal Crossing */
	SALSA64_ABI_NEAD_EFZ,   /* F-Zero X Expansion */
	SALSA64_ABI_NAUDIO,     /* naudio de Rare y otros */
	SALSA64_ABI_NAUDIO_BK,  /* Banjo-Kazooie */
	SALSA64_ABI_NAUDIO_DK,  /* Donkey Kong 64 */
	SALSA64_ABI_NAUDIO_MP3, /* Perfect Dark, Banjo-Tooie, Jet Force Gemini */
	SALSA64_ABI_NAUDIO_CBFD,/* Conker */
	SALSA64_ABI_MUSYX1,     /* MusyX (pendiente) */
	SALSA64_ABI_MUSYX2,
	SALSA64_ABI_COUNT
};

struct salsa64_audio_backend {
	void *ctx;
	/* Empieza a procesar la tarea; puede volver antes de terminarla. */
	void (*start)(void *ctx, const struct salsa64_task *task, enum salsa64_abi abi);
	/* Espera a que termine (cuando la máquina llega al fin de la tarea) y
	 * avisa al bus qué partes de la RDRAM escribió
	 * (salsa64_bus_rdram_written). */
	void (*finish)(void *ctx, struct salsa64_bus *bus);
	/* Opcional, como en el gráfico: ¿ya terminó? Sin bloquear. */
	bool (*done)(void *ctx, bool idle);
	/* Opcionales, como en el gráfico: el estado de los ABI que persiste
	 * entre tareas (DMEM, libro de ADPCM, envolventes, ventana de MP3). */
	uint32_t state_size;
	bool (*save_state)(void *ctx, uint8_t *buf);
	bool (*load_state)(void *ctx, const uint8_t *buf);
};

/* ---- Tarea JPEG en otro procesador (src/rsp/jpeg) ------------------------
 * Opcional. Sin backend, la HLE JPEG corre en el hilo del CPU al arrancar la
 * tarea, que termina en el acto. Con backend (en la PS3, un SPE: la HLE con
 * E/S por DMA), la tarea dura jpeg_cycles y su fin se sondea como el de las
 * gráficas (done/finish); entre medias el CPU sigue emulando. */
struct salsa64_jpeg_result;

struct salsa64_jpeg_backend {
	void *ctx;
	/* Publica la tarea (kind: enum salsa64_jpeg_kind). Vuelve enseguida. */
	void (*start)(void *ctx, const struct salsa64_task *task, unsigned kind);
	/* ¿Terminó? Sin bloquear; con el CPU en un bucle de espera (`idle`)
	 * puede dormir un poco antes de contestar. */
	bool (*done)(void *ctx, bool idle);
	/* Espera la tarea en vuelo y deja su resultado (la RDRAM ya escrita). */
	void (*finish)(void *ctx, struct salsa64_jpeg_result *res);
};

/* ---- RSP a bajo nivel (LLE, Fase 6: src/rsp/lle) ---------------------------
 * Un backend opcional que ejecuta el microcódigo real. Al quitarle HALT al
 * RSP, rsp.c le pasa el arranque si la tarea no tiene HLE (modo `auto`) o
 * siempre (`all`); el microcódigo corre desde SP_PC con la DMEM y la IMEM
 * del bus. En el host corre en rebanadas intercaladas con el CPU (sync lo
 * pone al día cuando el CPU toca un registro del SP o del DP); en la PS3,
 * en un SPE, con fin por sondeo como los demás backends (done/finish). */
enum salsa64_lle_mode {
	SALSA64_LLE_OFF = 0,
	SALSA64_LLE_AUTO,  /* solo lo que no tiene HLE */
	SALSA64_LLE_ALL,   /* todo el microcódigo, también el que tiene HLE */
};

#define SALSA64_TASK_LLE 0xFFu  /* rsp.busy: el microcódigo corre en el SPE */

struct salsa64_lle_backend {
	void *ctx;
	/* El RSP salió de HALT sin HLE: ejecutar desde SP_PC. Vuelve enseguida. */
	void (*start)(void *ctx);
	/* El CPU va a leer o escribir un registro del SP o del DP: poner al
	 * día al RSP (host). Opcional. */
	void (*sync)(void *ctx);
	/* Reset del RSP. Opcional. */
	void (*reset)(void *ctx);
	/* Opcionales, juntos (SPE): ¿terminó? sin bloquear, y recoger. Si
	 * `done` es NULL el RSP corre en el hilo del CPU y no se sondea. */
	bool (*done)(void *ctx, bool idle);
	void (*finish)(void *ctx);
};

/* RDP a bajo nivel: quien consume el flujo de comandos que llega por
 * DPC_START/DPC_END. `dmem` es NULL si los comandos están en la RDRAM o,
 * con xbus, la imagen de 4 KB de la DMEM de donde leerlos (módulo 4 KB).
 * Devuelve SALSA64_GFX_FULLSYNC si vio un SYNC_FULL (interrupción del DP). */
struct salsa64_dpc_backend {
	void *ctx;
	uint32_t (*run)(void *ctx, uint32_t start, uint32_t end, const uint8_t *dmem);
};

/* DPC_STATUS: bits de estado y, al escribir, pares (limpiar, poner). */
#define SALSA64_DP_STATUS_XBUS        0x001u
#define SALSA64_DP_STATUS_FREEZE      0x002u
#define SALSA64_DP_STATUS_FLUSH       0x004u
#define SALSA64_DP_STATUS_START_GCLK  0x008u
#define SALSA64_DP_STATUS_CBUF_READY  0x080u
#define SALSA64_DP_STATUS_END_VALID   0x200u
#define SALSA64_DP_STATUS_START_VALID 0x400u

struct salsa64_rsp_stats {
	uint32_t gfx, audio, other, unknown_gbi, unknown_abi;
	uint32_t polls;        /* fines de tarea vueltos a programar (SPE ocupado) */
	uint32_t starts_busy;  /* arranques ignorados con una tarea en curso */
	uint32_t unknown_data; /* ucode_data de la última con microcódigo desconocido */
	uint32_t lle;          /* arranques que fueron al microcódigo real */
	uint32_t dpc_runs;     /* tramos de comandos del RDP consumidos */
	uint32_t cut;          /* tareas gráficas cortadas por la HLE (DP forzada) */
	uint32_t jpeg;         /* tareas JPEG hechas por la HLE (rsp/jpeg) */
	uint32_t jpeg_mb;      /* sus macrobloques */
	uint32_t jpeg_bad;     /* con parámetros que no valían */
	uint64_t jpeg_us;      /* tiempo del hilo del CPU en ellas (con clock_us) */
};

struct salsa64_rsp {
	struct salsa64_sched *sched;
	const struct salsa64_gfx_backend *gfx;
	const struct salsa64_audio_backend *audio;
	const struct salsa64_lle_backend *lle;
	const struct salsa64_jpeg_backend *jpeg_be;  /* NULL: la HLE JPEG en el hilo del CPU */
	enum salsa64_lle_mode lle_mode;
	/* Diagnóstico: las tareas JPEG van al microcódigo real (LLE) aunque
	 * tengan HLE, para compararlas (salsa64-run --lle-check-jpeg). */
	bool jpeg_lle;
	/* Opcional: reloj en µs para medir lo que se hace en el hilo del CPU
	 * (la HLE JPEG). */
	uint64_t (*clock_us)(void);
	const struct salsa64_dpc_backend *dpc;
	uint32_t busy;          /* tipo de la tarea en curso; 0 = ninguna */
	enum salsa64_gbi gbi;   /* de la tarea en curso */
	bool gfx_nonear;        /* su microcódigo no recorta el plano cercano (".NoN") */
	enum salsa64_abi abi;   /* de la última tarea de audio */
	/* Duración aparente de cada tarea, en ciclos del VR4300. */
	uint32_t gfx_cycles, audio_cycles, dp_cycles, jpeg_cycles;
	/* Interrupción del DP retenida: el RDP terminó (SYNC_FULL) mientras
	 * estaba congelado (DPC_STATUS FREEZE); llega al descongelarlo. */
	bool dp_held;
	/* Cada cuántos ciclos se vuelve a preguntar al backend si terminó. */
	uint32_t poll_cycles;
	uint64_t poll_idle;     /* idle_cycles del CPU en el sondeo anterior */
	/* La tarea en curso ya se recogió del backend (salsa64_rsp_quiesce): su
	 * fin llega a su hora con estas banderas, sin volver a llamarlo. */
	bool collected;
	uint32_t collected_flags;
	struct salsa64_rsp_stats stats;
	/* Opcional (depuración): cada arranque del RSP con su tarea y la familia
	 * de microcódigo detectada (SALSA64_GBI_UNKNOWN si no es gráfica). */
	void (*trace)(void *ctx, const struct salsa64_task *task, enum salsa64_gbi gbi);
	void *trace_ctx;
};

enum salsa64_dp_reg {
	SALSA64_DP_START, SALSA64_DP_END, SALSA64_DP_CURRENT, SALSA64_DP_STATUS,
	SALSA64_DP_CLOCK, SALSA64_DP_BUFBUSY, SALSA64_DP_PIPEBUSY, SALSA64_DP_TMEM,
	SALSA64_DP_NREGS
};

enum salsa64_vi_reg {
	SALSA64_VI_CONTROL, SALSA64_VI_ORIGIN, SALSA64_VI_WIDTH, SALSA64_VI_V_INTR,
	SALSA64_VI_V_CURRENT, SALSA64_VI_BURST, SALSA64_VI_V_SYNC, SALSA64_VI_H_SYNC,
	SALSA64_VI_H_SYNC_LEAP, SALSA64_VI_H_VIDEO, SALSA64_VI_V_VIDEO,
	SALSA64_VI_V_BURST, SALSA64_VI_X_SCALE, SALSA64_VI_Y_SCALE, SALSA64_VI_NREGS
};

/* VI_CONTROL[1:0]: formato del framebuffer. */
enum salsa64_vi_type {
	SALSA64_VI_BLANK = 0,
	SALSA64_VI_RESERVED = 1,
	SALSA64_VI_RGBA5551 = 2,
	SALSA64_VI_RGBA8888 = 3,
};

#define SALSA64_VI_CONTROL_TYPE    0x0003u
#define SALSA64_VI_CONTROL_SERRATE 0x0040u  /* entrelazado */
#define SALSA64_VI_V_INTR_RESET    0x3FFu   /* nunca coincide: no interrumpe */

struct salsa64_vi {
	uint32_t regs[SALSA64_VI_NREGS];
	uint32_t clock_hz;      /* reloj del VI, según el tipo de TV */
	uint32_t lines;         /* líneas del campo en curso */
	uint32_t field;         /* 0/1: campo en curso (solo entrelazado) */
	uint32_t fields;        /* campos completos desde el reset */
	uint64_t field_start;   /* ciclo del VR4300 en que empezó el campo */
	uint64_t field_cycles;  /* duración del campo en curso */
	struct salsa64_sched *sched;
};

/* Imagen que saca el VI: la parte visible del framebuffer en la RDRAM. */
struct salsa64_vi_frame {
	enum salsa64_vi_type type;
	uint32_t origin;  /* dirección física de la primera línea */
	uint32_t width;   /* píxeles visibles por línea */
	uint32_t height;  /* líneas visibles */
	uint32_t stride;  /* bytes por línea (VI_WIDTH × bytes por píxel) */
	uint32_t bpp;     /* bytes por píxel: 2 o 4 */
};

enum salsa64_ai_reg {
	SALSA64_AI_DRAM_ADDR, SALSA64_AI_LEN, SALSA64_AI_CONTROL, SALSA64_AI_STATUS,
	SALSA64_AI_DACRATE, SALSA64_AI_BITRATE, SALSA64_AI_NREGS
};

#define SALSA64_AI_STATUS_FULL    0x80000001u  /* bits 31 y 0 */
#define SALSA64_AI_STATUS_BUSY    0x40000000u
#define SALSA64_AI_STATUS_ENABLED 0x02000000u
#define SALSA64_AI_STATUS_ONES    0x01100000u  /* bits 24 y 20: siempre 1 */

/* Sumidero de las muestras del AI (ai.c). Recibe las de cada DMA cuando
 * empieza a sonar: 16 bits con signo, estéreo intercalado (izquierda,
 * derecha), en el orden de bytes de la N64; la frecuencia de muestreo del
 * DAC y el ciclo del VR4300 en que empieza. `samples` apunta a la RDRAM
 * (o a ceros si el DMA se sale): vale solo durante la llamada. */
typedef void (*salsa64_ai_push_fn)(void *ctx, const uint8_t *samples, uint32_t bytes, uint32_t rate_hz,
				   uint64_t cycle);

struct salsa64_ai {
	uint32_t regs[SALSA64_AI_NREGS];  /* lo último escrito */
	/* Cola de DMA de dos lugares: [0] suena, [1] espera. */
	uint32_t count;
	uint32_t addr[2], len[2];
	bool carry;        /* el DMA anterior terminó justo en un límite de 8 KB */
	bool started;      /* el DMA en curso ya empezó a avanzar (y se entregó) */
	uint32_t left;     /* bytes del DMA en curso que faltaban en `mark` */
	uint64_t mark;     /* ciclo de esa cuenta */
	struct salsa64_sched *sched;
	salsa64_ai_push_fn push;
	void *push_ctx;
	uint32_t dmas;     /* DMAs que empezaron a sonar desde el reset */
};

enum salsa64_pi_reg {
	SALSA64_PI_DRAM_ADDR, SALSA64_PI_CART_ADDR, SALSA64_PI_RD_LEN,
	SALSA64_PI_WR_LEN, SALSA64_PI_STATUS, SALSA64_PI_BSD_DOM1_LAT,
	SALSA64_PI_BSD_DOM1_PWD, SALSA64_PI_BSD_DOM1_PGS, SALSA64_PI_BSD_DOM1_RLS,
	SALSA64_PI_BSD_DOM2_LAT, SALSA64_PI_BSD_DOM2_PWD, SALSA64_PI_BSD_DOM2_PGS,
	SALSA64_PI_BSD_DOM2_RLS, SALSA64_PI_NREGS
};

#define SALSA64_PI_STATUS_DMA_BUSY 0x01u
#define SALSA64_PI_STATUS_IO_BUSY  0x02u
#define SALSA64_PI_STATUS_INTR     0x08u

enum salsa64_si_reg {
	SALSA64_SI_DRAM_ADDR, SALSA64_SI_PIF_ADDR_RD64B, SALSA64_SI_R2,
	SALSA64_SI_R3, SALSA64_SI_PIF_ADDR_WR64B, SALSA64_SI_R5,
	SALSA64_SI_STATUS, SALSA64_SI_NREGS
};

#define SALSA64_SI_STATUS_INTR 0x1000u

#define SALSA64_RI_NREGS    8
#define SALSA64_RDRAM_NREGS    16
#define SALSA64_RDRAM_REGS_END 0x03F80000u  /* después: escrituras "broadcast" */

/* Interrupciones del MI. Actualizan Cause.IP2 del CPU. */
void salsa64_mi_raise(struct salsa64_bus *bus, enum salsa64_irq irq);
void salsa64_mi_clear(struct salsa64_bus *bus, enum salsa64_irq irq);

void salsa64_rcp_reset(struct salsa64_bus *bus);

uint32_t salsa64_mi_read(struct salsa64_bus *bus, uint32_t reg);
void salsa64_mi_write(struct salsa64_bus *bus, uint32_t reg, uint32_t value);
uint32_t salsa64_sp_read(struct salsa64_bus *bus, uint32_t reg);
void salsa64_sp_write(struct salsa64_bus *bus, uint32_t reg, uint32_t value);

/* Tareas del RSP (rsp.c). salsa64_rsp_init registra sus eventos; el backend
 * gráfico se cambia con salsa64_rsp_set_gfx (NULL: las tareas gráficas
 * terminan sin dibujar). salsa64_rsp_start lo llama el SP al quitarle HALT. */
void salsa64_rsp_init(struct salsa64_bus *bus, struct salsa64_sched *sched);
void salsa64_rsp_reset(struct salsa64_bus *bus);
void salsa64_rsp_set_gfx(struct salsa64_bus *bus, const struct salsa64_gfx_backend *gfx);
void salsa64_rsp_start(struct salsa64_bus *bus);
void salsa64_rsp_read_task(const struct salsa64_bus *bus, struct salsa64_task *task);
/* Familia de microcódigo de una tarea gráfica, por los textos de su
 * segmento de datos (ucode_data) o, si no trae texto, por el CRC-32 de su
 * código. */
enum salsa64_gbi salsa64_rsp_detect_gbi(const struct salsa64_bus *bus, const struct salsa64_task *task);
/* El texto que identifica a un microcódigo ("RSP Gfx ucode …", "RSP SW
 * Version: …") en sus datos, en `out` (vacío si no hay). Para los logs. */
void salsa64_rsp_ucode_text(const struct salsa64_bus *bus, uint32_t ucode_data, char *out, size_t n);
/* Tareas de audio (rsp_audio.c): el backend de la HLE (NULL: terminan sin
 * producir nada) y el ABI de una tarea por las palabras de su ucode_data. */
void salsa64_rsp_set_audio(struct salsa64_bus *bus, const struct salsa64_audio_backend *audio);
enum salsa64_abi salsa64_rsp_detect_abi(const struct salsa64_bus *bus, const struct salsa64_task *task);
const char *salsa64_abi_name(enum salsa64_abi abi);
/* RSP a bajo nivel: el backend (NULL lo quita) y el modo; el consumidor
 * del RDP (NULL: los comandos solo se descartan y DPC_CURRENT avanza). */
void salsa64_rsp_set_lle(struct salsa64_bus *bus, const struct salsa64_lle_backend *lle,
			 enum salsa64_lle_mode mode);
void salsa64_rsp_set_dpc(struct salsa64_bus *bus, const struct salsa64_dpc_backend *dpc);
/* Tareas JPEG en otro procesador (NULL: en el hilo del CPU). */
void salsa64_rsp_set_jpeg(struct salsa64_bus *bus, const struct salsa64_jpeg_backend *jpeg);
/* Un tramo de comandos del RDP ya entregado (lo que un SPE anotó al correr
 * el microcódigo): deja DPC_START/END/CURRENT como si el RDP lo hubiera
 * consumido y lo pasa al consumidor. `dmem` como en salsa64_dpc_backend. */
void salsa64_dp_submit(struct salsa64_bus *bus, uint32_t start, uint32_t end, const uint8_t *dmem);
/* Estados guardados: con la emulación quieta, espera a que el backend
 * termine la tarea en curso (gráfica, de audio o JPEG) y guarda su resultado; el
 * fin emulado de la tarea no se adelanta (llega a su hora con ese
 * resultado). Devuelve false si el RSP corre microcódigo real (LLE): sin
 * estados guardados. */
bool salsa64_rsp_quiesce(struct salsa64_bus *bus);
/* El CPU va a tocar un registro del SP o del DP (sp.c): el RSP a bajo
 * nivel se pone al día. */
void salsa64_rsp_sync(struct salsa64_bus *bus);
/* El RDP vio un SYNC_FULL: interrupción del DP dp_cycles después o, si el
 * RDP está congelado (DPC_STATUS FREEZE), dp_cycles después de que el CPU lo
 * descongele (salsa64_rsp_dp_unfreeze, desde la escritura de DPC_STATUS). */
void salsa64_rsp_dp_done(struct salsa64_bus *bus);
void salsa64_rsp_dp_unfreeze(struct salsa64_bus *bus);
uint32_t salsa64_dp_read(struct salsa64_bus *bus, uint32_t reg);
void salsa64_dp_write(struct salsa64_bus *bus, uint32_t reg, uint32_t value);
uint32_t salsa64_vi_read(struct salsa64_bus *bus, uint32_t reg);
void salsa64_vi_write(struct salsa64_bus *bus, uint32_t reg, uint32_t value);

/* Temporización del VI (vi.c). salsa64_vi_init registra sus eventos en el
 * planificador; salsa64_vi_start empieza un campo en el ciclo actual del CPU
 * (después de cada reset). */
void salsa64_vi_init(struct salsa64_bus *bus, struct salsa64_sched *sched);
void salsa64_vi_start(struct salsa64_bus *bus);
/* Reloj del VI según el tipo de TV; se aplica desde el campo siguiente. */
void salsa64_vi_set_tv(struct salsa64_bus *bus, enum salsa64_tv_type tv);
/* Ciclos que faltan para que termine el campo en curso (al menos 1). */
uint64_t salsa64_vi_cycles_left(const struct salsa64_bus *bus);
/* Describe la imagen visible; false si el VI está apagado o sus registros no
 * forman una imagen dentro de la RDRAM. */
bool salsa64_vi_frame(const struct salsa64_bus *bus, struct salsa64_vi_frame *frame);
/* Audio Interface (ai.c). salsa64_ai_init registra su evento; el sumidero
 * se cambia con salsa64_ai_set_sink (NULL: las muestras se descartan, pero
 * la temporización y las interrupciones siguen igual). */
void salsa64_ai_init(struct salsa64_bus *bus, struct salsa64_sched *sched);
void salsa64_ai_reset(struct salsa64_bus *bus);
void salsa64_ai_set_sink(struct salsa64_bus *bus, salsa64_ai_push_fn push, void *ctx);
/* Frecuencia de muestreo que da AI_DACRATE con el reloj del VI (Hz). */
uint32_t salsa64_ai_rate(const struct salsa64_bus *bus);
uint32_t salsa64_ai_read(struct salsa64_bus *bus, uint32_t reg);
void salsa64_ai_write(struct salsa64_bus *bus, uint32_t reg, uint32_t value);
uint32_t salsa64_pi_read(struct salsa64_bus *bus, uint32_t reg);
void salsa64_pi_write(struct salsa64_bus *bus, uint32_t reg, uint32_t value);
uint32_t salsa64_si_read(struct salsa64_bus *bus, uint32_t reg);
void salsa64_si_write(struct salsa64_bus *bus, uint32_t reg, uint32_t value);

#endif
