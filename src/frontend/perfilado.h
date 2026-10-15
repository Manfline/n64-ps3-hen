/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — perfilado: el archivo de cada sesión de juego y su escritura
 * (docs/perfilado.md). Portable: lo usan el frontend de la PS3 (app
 * «Salsa64 perfil») y salsa64-run --perfilado en el host.
 *
 * Todo va en <raíz>/<juego>-<CRC>/<n>.sesion (n sube en cada sesión del
 * juego) y, si se piden, los volcados de la HLE en <juego>-<CRC>/volcados/.
 * La sesión no lleva ni un byte del juego: direcciones, cuentas, tiempos,
 * huellas de un solo sentido e ids. Los volcados sí (son la salida de la HLE
 * con sus texturas): quedan entre la consola y la Mac, nunca se comparten.
 *
 * Archivo: cabecera de 16 bytes («S64P», versión, largo de la cabecera) y
 * registros seguidos, cada uno con 4 bytes de cabecera (tipo en el byte alto,
 * largo del contenido en los 24 bajos) y su contenido. Todo big-endian, el
 * orden de la PS3, también si lo escribe el host. Un archivo cortado (la
 * consola se apagó) se lee hasta el último registro completo.
 *
 * Escritura: quien produce (el hilo que emula, el de presentación) arma el
 * registro en la pila y lo copia a su anillo en memoria (un productor y un
 * consumidor, sin candados); otro hilo de baja prioridad vacía los anillos
 * al archivo cada tanto (perf_drain). Un registro que no cabe se pierde y
 * se cuenta: el juego nunca espera al disco.
 */
#ifndef SALSA64_FRONTEND_PERFILADO_H
#define SALSA64_FRONTEND_PERFILADO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PERF_MAGIC   0x53363450u  /* "S64P" */
#define PERF_VERSION 1u
#define PERF_HEADER  16u

/* Tipos de registro (docs/perfilado.md tiene el contenido de cada uno). */
enum perf_rec {
	PERF_R_CONTEXT = 1,   /* texto «clave=valor» por línea: versión, juego, ajustes */
	PERF_R_NAMES = 2,     /* nombres de los campos de VENTANA y TIRON (texto) */
	PERF_R_WINDOW = 3,    /* cada ventana de estadísticas (5 s) */
	PERF_R_STALL = 4,     /* un campo de emulación más largo que el umbral */
	PERF_R_BLOCKS = 5,    /* compilaciones del recompilador */
	PERF_R_EVICT = 6,     /* desalojos de tramos y vaciados */
	PERF_R_TRACE = 7,     /* una ventana de la secuencia de bloques */
	PERF_R_JR = 8,        /* JR por bloque (al cerrar) */
	PERF_R_SAMPLES = 9,   /* muestreo por tiempo (al cerrar) */
	PERF_R_TASKS = 10,    /* tareas gráficas */
	PERF_R_DUMP = 11,     /* un volcado automático de la HLE */
	PERF_R_END = 12,      /* fin de la sesión (texto) */
	PERF_R_LOST = 13,     /* bytes que no cupieron en un anillo */
	PERF_R_EVENT = 14,    /* texto: algo que pasó (estado cargado, pausa, opción) */
	PERF_R_SLOW = 15,     /* contadores del camino lento desde la ventana anterior */
	PERF_R_COMPILE = 16,  /* compilaciones por motivo desde la ventana anterior */
};

/* ---- Campos de VENTANA y TIRON ------------------------------------------------
 * Cada uno es un uint32_t; el registro lleva ms, cuántos campos y los
 * valores en este orden (PERF_R_NAMES los nombra en el archivo, así el lector
 * no depende de esta lista). Las unidades van en el nombre. */
#define PERF_WINDOW_FIELDS(X) \
	X(campo) X(dur_ms) X(campos) X(cps_x100) X(velocidad_pm) X(mips_x10) X(espera_pm) \
	X(emu_us_campo) X(copia_vi_us_campo) X(ritmo_us_campo) X(emu_peor_us) \
	X(presentados) X(saltados) X(descartados) X(sin_imagen) X(del_rsx) \
	X(cola_x100) X(cola_max) X(edad_x100) X(edad_max) X(hilo1_pm) X(rsx_emu_us_campo) X(rsx_pres_us_cuadro) \
	X(tareas) X(dibujos) X(triangulos) X(spe_us) X(espera_spe_us) X(traduccion_us) \
	X(esperas_juego) X(espera_juego_us) X(copias_rdram) X(copias_rdram_us) X(copias_esperadas) \
	X(copias_espera_us) X(copias_tarde) X(copias_perdidas) X(tareas_cortadas) X(comandos_saltados) \
	X(desconocidos) X(aproximados) X(fp_cambios) X(fp_generados) X(destinos_creados) X(destinos_reciclados) \
	X(audio_salida_ms) X(audio_silencio_pm) X(audio_estirado_pm) X(audio_faltas) X(audio_silencios) \
	X(audio_saltos) X(audio_descartadas) \
	X(rec_bloques) X(rec_retrad) X(rec_palabras) X(rec_desalojos) X(rec_vaciados) X(rec_muertos) \
	X(rec_interp) X(rec_jr) X(rec_compilar_us) \
	X(m_total) X(m_nativo) X(m_lento) X(m_despachador) X(m_interprete) X(m_eventos) X(m_compilar) \
	X(m_espera) X(m_ritmo) X(m_otro) \
	X(mandos_hueco_us) X(perdidos)

#define PERF_STALL_FIELDS(X) \
	X(campo) X(emu_us) X(ciclos) X(ciclos_espera) X(compilados) X(compilar_us) X(palabras) X(retrad) \
	X(desalojos) X(vaciados) X(muertos) X(interp) X(espera_spe_us) X(dormido_us) X(espera_juego_us) \
	X(copias_espera_us) X(copia_vi_us) X(jpeg_us) X(tareas) X(audio_tareas) X(solape_hilo1_us) \
	X(m_total) X(m_nativo) X(m_lento) X(m_despachador) X(m_interprete) X(m_eventos) X(m_compilar) \
	X(m_espera) X(m_ritmo) X(m_otro) X(invalidados) X(pi_dma_kb)

#define PERF_ENUM(n) PERF_W_##n,
enum perf_window_field { PERF_WINDOW_FIELDS(PERF_ENUM) PERF_W_N };
#undef PERF_ENUM
#define PERF_ENUM(n) PERF_S_##n,
enum perf_stall_field { PERF_STALL_FIELDS(PERF_ENUM) PERF_S_N };
#undef PERF_ENUM

/* ---- Entradas de BLOQUES y TAREAS (largo fijo, lo dice el registro) ------------ */
#define PERF_BLOCK_BYTES 88u
#define PERF_TASK_BYTES  48u

/* Una tarea gráfica: lo que da la HLE (gfx_result) y lo que mide la
 * traducción al RSX. */
struct perf_task {
	uint32_t ms, field, task;
	uint32_t commands, triangles;
	uint16_t draws, textures, unknown;
	uint8_t last_unknown, flags;      /* flags: GFX_RES_* (gfx.h) */
	uint16_t approx_cc, approx_draws, skipped, dl_errors;
	uint16_t spe_us, translate_us, set_wait_us, fp_loads;
	uint8_t readbacks, news;          /* news: PERF_TASK_* */
};
#define PERF_TASK_NEW_UNKNOWN 1u   /* un comando desconocido que no se había visto en la sesión */
#define PERF_TASK_NEW_APPROX  2u   /* un combiner aproximado nuevo */
#define PERF_TASK_DUMPED      4u   /* se volcó */

/* ---- Configuración (perfilado.cfg) ------------------------------------------------ */
struct perf_cfg {
	bool on;            /* «perfilado = si|no» */
	bool trace;         /* «traza»: ventanas de la secuencia de bloques */
	bool jr;            /* «jr»: JR por bloque */
	bool blocks;        /* «bloques»: cada compilación */
	bool samples;       /* «muestreo»: por tiempo, desde un hilo */
	bool profile;       /* «perfil»: el persistente (contar entradas, ~5 %) */
	bool tasks;         /* «tareas»: cada tarea gráfica */
	uint32_t dumps;     /* «volcados»: cuántos volcados automáticos por sesión (0: ninguno) */
	uint32_t cap_mb;    /* «tope_mb»: todo el perfilado (se borran las sesiones más viejas) */
	uint32_t stall_ms;  /* «tiron_ms»: campos de emulación más largos van como TIRON */
	uint32_t trace_s;   /* «traza_cada_s»: una ventana cada tanto */
	uint32_t trace_len; /* «traza_largo»: entradas por ventana */
	uint32_t dump_gap_s;  /* «volcados_cada_s»: separación mínima entre volcados */
};

void perf_cfg_defaults(struct perf_cfg *c);
/* Lee «clave = valor» por línea (# comenta); lo desconocido se ignora. */
void perf_cfg_parse(struct perf_cfg *c, const char *text);
/* El texto de la configuración en uso, para el contexto. */
void perf_cfg_text(const struct perf_cfg *c, char *buf, size_t size);

/* ---- Anillos ---------------------------------------------------------------------- */
struct perf_ring {
	uint8_t *buf;
	uint32_t size;               /* potencia de dos */
	volatile uint32_t head;      /* lo escribe el productor */
	volatile uint32_t tail;      /* lo escribe el consumidor */
	volatile uint32_t lost;      /* bytes de registros que no cupieron (productor) */
	uint32_t lost_seen;          /* consumidor */
};

void perf_ring_init(struct perf_ring *r, uint8_t *buf, uint32_t size);
/* Un registro entero (cabecera y contenido) o nada. */
bool perf_put(struct perf_ring *r, unsigned type, const void *payload, uint32_t len);

/* ---- Sesión ------------------------------------------------------------------------ */
#define PERF_PATH_MAX 192
#define PERF_RINGS 3

struct perf_session {
	FILE *f;
	char dir[PERF_PATH_MAX];     /* <raíz>/<juego>-<CRC> */
	char path[PERF_PATH_MAX];    /* .../<n>.sesion */
	unsigned n;
	uint64_t written;            /* bytes escritos en esta sesión */
	uint64_t cap;                /* tope de toda la carpeta (bytes) */
	uint64_t others;             /* lo que ocupa el resto de la carpeta al abrir */
	bool over;                   /* se pasó del tope: solo lo liviano */
	uint32_t write_errors;
	struct perf_ring *rings[PERF_RINGS];
	uint32_t removed;            /* sesiones viejas borradas al abrir */
};

/* La carpeta del juego, el número de sesión y el archivo (con su
 * cabecera); antes borra las sesiones más viejas de toda la raíz hasta que
 * quede lugar (cap - reserve). false si no se pudo crear. */
bool perf_session_open(struct perf_session *s, const char *root, const char *game, uint64_t cap, uint64_t reserve);
/* Vacía los anillos al archivo (con PERF_R_LOST si hubo pérdidas). Desde
 * un solo hilo. Devuelve los bytes escritos. */
size_t perf_drain(struct perf_session *s);
/* ¿Los registros pesados (traza, bloques, tareas) caben todavía? */
static inline bool perf_room(const struct perf_session *s)
{
	return s->f && !s->over;
}
void perf_session_close(struct perf_session *s);

/* Lo que ocupa la raíz entera y cuántas sesiones hay (para el log). */
uint64_t perf_dir_bytes(const char *root, unsigned *sessions);
/* Borra las sesiones más viejas (por fecha, y el número dentro de un juego)
 * hasta que la raíz ocupe a lo más `limit`; no toca `keep` (la sesión en
 * curso). Devuelve cuántas borró. */
unsigned perf_rotate(const char *root, uint64_t limit, const char *keep);

/* ---- Codificación ------------------------------------------------------------------
 * Un registro se escribe directo en el anillo: perf_begin reserva cabecera
 * y contenido (o falla y cuenta la pérdida), perf_w* escriben big-endian y
 * perf_end lo publica. Escribir de más que lo reservado no hace nada. */
struct perf_rw {
	struct perf_ring *r;
	uint32_t pos, end;   /* posiciones absolutas (se enmascaran al escribir) */
};
bool perf_begin(struct perf_rw *w, struct perf_ring *r, unsigned type, uint32_t len);
static inline void perf_w8(struct perf_rw *w, uint32_t v)
{
	if (w->pos != w->end)
		w->r->buf[w->pos++ & (w->r->size - 1)] = (uint8_t)v;
}
static inline void perf_w16(struct perf_rw *w, uint32_t v)
{
	perf_w8(w, v >> 8);
	perf_w8(w, v);
}
static inline void perf_w32(struct perf_rw *w, uint32_t v)
{
	perf_w16(w, v >> 16);
	perf_w16(w, v);
}
static inline void perf_w64(struct perf_rw *w, uint64_t v)
{
	perf_w32(w, (uint32_t)(v >> 32));
	perf_w32(w, (uint32_t)v);
}
void perf_wbytes(struct perf_rw *w, const void *p, uint32_t n);
/* Publica el registro (lo que falte hasta el largo reservado va en 0). */
void perf_end(struct perf_rw *w);
static inline uint32_t perf_sat16(uint64_t v)
{
	return v > 0xFFFF ? 0xFFFF : (uint32_t)v;
}

/* Registros de uso común. */
bool perf_put_text(struct perf_ring *r, unsigned type, uint32_t ms, const char *text);
/* VENTANA o TIRON: ms, cuántos y los valores. */
bool perf_put_values(struct perf_ring *r, unsigned type, uint32_t ms, const uint32_t *v, unsigned n);
bool perf_put_task(struct perf_ring *r, const struct perf_task *t);
/* Los nombres de VENTANA y TIRON («ventana=a,b,…\ntiron=…»). */
void perf_names_text(char *buf, size_t size);

struct salsa64_rec_trace;
struct salsa64_rec_trace_block;
struct salsa64_rec_sample;
struct salsa64_rec_trace_slow_stats;
struct salsa64_rec_trace_compile_stats;
bool perf_put_slow(struct perf_ring *r, uint32_t ms, const struct salsa64_rec_trace_slow_stats *s);
bool perf_put_compile(struct perf_ring *r, uint32_t ms, const struct salsa64_rec_trace_compile_stats *s);
bool perf_put_block(struct perf_ring *r, uint32_t ms, uint32_t field, const struct salsa64_rec_trace_block *b);
bool perf_put_evict(struct perf_ring *r, uint32_t ms, uint32_t field, unsigned kind, unsigned seg, unsigned killed);
/* La ventana recién recogida (salsa64_rec_trace_take devolvió `len`). */
bool perf_put_trace(struct perf_ring *r, uint32_t ms, uint32_t field, uint64_t cycles, const struct salsa64_rec_trace *t,
		    unsigned len);
/* Los JR plegados por dirección (salsa64_rec_trace_fold_all antes). */
bool perf_put_jr(struct perf_ring *r, uint32_t ms, const struct salsa64_rec_trace *t);
/* La tabla del muestreo (salsa64_rec_sample_fold_all antes). */
bool perf_put_samples(struct perf_ring *r, uint32_t ms, const struct salsa64_rec_sample *s);

#endif
