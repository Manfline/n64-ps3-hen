/*
 * Salsa64 — menú para elegir ROM (la lógica y el dibujo; main.c lo presenta).
 *
 * Una fila por ROM de la lista (romlist.h) más «Salir al XMB» al final.
 * Cruceta arriba/abajo mueve la selección (repite al mantenerla: primero
 * MENU_REPEAT_DELAY_US, después cada MENU_REPEAT_US y, pasado
 * MENU_REPEAT_FAST_AFTER_US, cada MENU_REPEAT_FAST_US); izquierda/derecha
 * avanzan una página; X abre la ROM o sale. Al llegar al final, una pulsación
 * nueva da la vuelta (la repetición no). Hasta ver todos los botones
 * sueltos no acepta nada: el START + SELECT con que se volvió del juego, o
 * el X con que se abrió, no cuentan dos veces.
 *
 * El dibujo va a un lienzo de rsxkit (rsxkit/text.h) de cualquier tamaño
 * (640 × 360 para una tele 16:9, 640 × 480 para 4:3) y solo cuando algo
 * cambió (`dirty`). No depende de PSL1GHT: pruebas en tests/unit/test_menu.c.
 */
#ifndef SALSA64_FRONTEND_MENU_H
#define SALSA64_FRONTEND_MENU_H

#include <rsxkit/text.h>

#include "romlist.h"

#define MENU_REPEAT_DELAY_US      400000u
#define MENU_REPEAT_US            110000u
#define MENU_REPEAT_FAST_AFTER_US 1500000u
#define MENU_REPEAT_FAST_US       45000u
#define MENU_STATUS_MAX           120
#define MENU_BACK_RGB             0x10161Eu  /* fondo (para limpiar fuera del lienzo) */
#define MENU_DIRS_MAX             4

enum menu_action {
	MENU_NONE,
	MENU_OPEN,  /* abrir menu_selected() */
	MENU_QUIT,  /* «Salir al XMB» */
};

struct menu {
	const struct romlist *list;
	int sel;               /* 0 .. list->count; list->count es «Salir al XMB» */
	int top;               /* primera fila visible */
	int rows;              /* filas visibles (las pone menu_draw) */
	bool armed;            /* ya se vieron todos los botones sueltos */
	bool dirty;            /* hay que volver a dibujar */
	uint32_t held;         /* botones del llamado anterior */
	uint32_t rep_button;   /* bit de la dirección que repite, o 0 */
	uint64_t rep_since, rep_next;
	char status[MENU_STATUS_MAX];  /* mensaje (p. ej. un error al abrir) */
	const char *dirs[MENU_DIRS_MAX];  /* dónde se buscó (para la lista vacía) */
	unsigned ndirs;
};

/* `sel`: fila inicial (la última ROM abierta; fuera de rango = la primera). */
void menu_init(struct menu *m, const struct romlist *list, int sel);
/* Botones del DualShock 3 (bits SALSA64_DS3_* de padmap.h; el OR de todos
 * los mandos) y la hora en µs. */
enum menu_action menu_input(struct menu *m, uint32_t buttons, uint64_t now_us);
/* Índice de la ROM elegida en la lista, o -1 si es «Salir al XMB». */
int menu_selected(const struct menu *m);
void menu_set_status(struct menu *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Dibuja el menú entero (fondo incluido) y limpia `dirty`. */
void menu_draw(struct menu *m, const rsxkit_canvas *c);
/* Pantalla de «cargando» mientras se lee una ROM. */
void menu_draw_loading(const rsxkit_canvas *c, const struct rom_entry *e);

/* Para la prueba automática: los botones que acercan la selección a la
 * fila `target` (0 .. list->count) y la abren; suelta todo en los llamados
 * impares de `step` para que cada uno cuente como pulsación nueva. */
uint32_t menu_autopilot(const struct menu *m, int target, uint32_t step);

#endif
