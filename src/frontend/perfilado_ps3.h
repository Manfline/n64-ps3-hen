/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — perfilado en la PS3 (docs/perfilado.md): la sesión de cada
 * juego en SALSA64_DATA_DIR/perfilado, el hilo que la escribe, el registro
 * del recompilador (rec_trace.h) y lo de cada tarea gráfica con sus
 * volcados automáticos.
 *
 * Solo en la app «Salsa64 perfil» (compilada con PERFILADO=1:
 * SALSA64_PERFILADO), encendido por omisión; SALSA64_DATA_DIR/perfilado.cfg
 * lo afina o lo apaga (perfilado.h, perf_cfg). La app normal no lee ese
 * archivo ni abre nada, y su recompilador queda sin rec->trace: el código
 * generado es el de siempre.
 *
 * Hilos: el que emula escribe al anillo `emu` (compilaciones, desalojos,
 * tirones, ventanas de la traza); el de presentación al anillo `pres`
 * (ventanas de estadísticas, tareas, volcados, contexto, cierre); uno de
 * prioridad PERF_IO_PRIO los vacía al disco cada medio segundo.
 */
#ifndef SALSA64_FRONTEND_PERFILADO_PS3_H
#define SALSA64_FRONTEND_PERFILADO_PS3_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu/rec.h"
#include "perfilado.h"
#include "platform/ps3/paths.h"

#ifndef SALSA64_PERFILADO
#define SALSA64_PERFILADO 0
#endif

#define PERF_ROOT     SALSA64_DATA_DIR "/perfilado"
#define PERF_CFG_PATH SALSA64_DATA_DIR "/perfilado.cfg"

struct perf_app {
	bool on;                   /* hay sesión abierta para este juego */
	struct perf_cfg cfg;
	struct perf_session ses;
	struct perf_ring emu, pres;
	uint64_t t0;               /* sysGetSystemTime al abrir */
	char game[48];             /* <juego>-<CRC> */
	struct salsa64_rec_trace *trace;  /* 3 MB, la primera vez; NULL si no hay recompilador */
	const volatile uint32_t *field;   /* el campo del VI (n64.bus.vi.fields) */
	/* Ventanas de la traza (hilo que emula). */
	uint64_t trace_next_us;
	uint32_t trace_field0, traces;
	uint32_t rng;
	/* Tareas y volcados (hilo de presentación). */
	uint32_t tasks, dumps, dump_fails;
	uint64_t dump_last_us;
	uint8_t seen_op[32];       /* opcodes desconocidos ya vistos */
	uint32_t sigs[256], nsigs; /* huellas de combiners aproximados ya vistas */
	bool cut_seen;
	/* Escritura. */
	volatile int stop;
	bool thread_ok;
	uint64_t tid;
};

extern struct perf_app perf;

/* ¿Esta app perfila por omisión? (SALSA64_PERFILADO) */
static inline bool perf_app_default(void)
{
	return SALSA64_PERFILADO != 0;
}

/* Al abrir un juego, antes de salsa64_rec_init: lee perfilado.cfg, abre la
 * sesión, arranca el hilo de escritura y, si se va a usar el recompilador,
 * le pone rec->trace. false si el perfilado no va en esta sesión. */
bool perf_app_open(const uint8_t *rom, struct salsa64_rec *rec, const volatile uint32_t *field);
/* Milisegundos desde que se abrió la sesión. */
uint32_t perf_ms(void);
/* El contexto (texto «clave=valor» por línea), una vez arrancado todo. */
void perf_app_context(const char *text);
/* Hilo que emula, al terminar cada campo (fuera del código generado): recoge
 * la ventana de la traza si se llenó y programa la siguiente. */
void perf_app_field(struct salsa64_rec *rec, uint32_t field_cycles);
/* Al cerrar, con la emulación detenida y antes de soltar el recompilador y
 * el muestreo: los JR y la tabla del muestreo. */
void perf_app_close_rec(struct salsa64_rec *rec, struct salsa64_rec_sample *sample);
/* Lo último: el registro FIN, el hilo y el archivo. */
void perf_app_close(const char *end_text);

#endif
