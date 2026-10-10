/*
 * Salsa64 — LLE del RSP en el hilo del CPU (host y PPE). Ver core.c.
 */
#ifndef SALSA64_RSP_LLE_CORE_H
#define SALSA64_RSP_LLE_CORE_H

#include "core/bus.h"
#include "rsp/lle/lle.h"

struct salsa64_lle_host {
	struct salsa64_lle_backend backend;
	struct rsp_lle rsp;
	struct salsa64_bus *bus;
	bool running;       /* hay rebanadas programadas (el RSP no está HALT) */
	bool in_slice;      /* dentro de lle_run: sync no reentra */
	uint64_t last;      /* ciclo del CPU hasta el que se emuló el RSP */
	uint32_t credit;    /* unidades que sobraron (módulo ipc_den) */
	/* Velocidad emulada: ipc_num instrucciones del RSP por cada ipc_den
	 * ciclos del CPU (por omisión 2/3: 62,5 MHz contra 93,75, una
	 * instrucción por ciclo). salsa64_lle_host_speed lo cambia. */
	uint32_t ipc_num, ipc_den;
	/* Estadísticas desde el inicio. */
	uint64_t instructions;
	uint32_t starts, slices, syncs;
	uint64_t task_start;     /* instrucciones al arrancar la tarea en curso */
	uint32_t tasks_done, task_max;  /* tareas terminadas y la más larga (instrucciones) */
};

/* Engancha el intérprete a la DMEM/IMEM del bus y registra su evento; después
 * salsa64_rsp_set_lle(bus, &h->backend, modo). */
void salsa64_lle_host_init(struct salsa64_lle_host *h, struct salsa64_bus *bus);
/* Velocidad emulada: `ipc` instrucciones del RSP por ciclo del CPU (0,667
 * es la nominal); más rápido acorta las tareas en tiempo emulado. */
void salsa64_lle_host_speed(struct salsa64_lle_host *h, double ipc);
/* Ejecuta hasta `max` instrucciones ahora mismo (pruebas), sin planificador. */
uint32_t salsa64_lle_host_run(struct salsa64_lle_host *h, uint32_t max);

#endif
