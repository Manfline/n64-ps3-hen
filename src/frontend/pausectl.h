/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — pausa del juego (hilo de presentación): el menú del sistema
 * (botón PS) y el cuadro de confirmación de START + SELECT detienen la
 * emulación, y lo que se pulsa mientras tanto no llega al juego. La
 * emulación, el audio y la presentación hacen su parte en main.c
 * (docs/hilos-ppe.md, «Botón PS y pausa»); aquí solo la decisión.
 *
 * Mandos, en cada vuelta (pausectl_pads):
 *   - Con el menú del sistema abierto, o con los mandos tomados por el
 *     sistema (bit 0 de padInfo.info), el juego los recibe sueltos y nada
 *     cuenta como pulsación.
 *   - START + SELECT dentro del juego (los dos apretados y uno recién):
 *     con confirmación, abre el cuadro; sin ella, sale.
 *   - Con el cuadro a la vista: × sale; ○ o START + SELECT otra vez lo
 *     cierran y el juego sigue.
 *   - Lo que estaba apretado al abrir o cerrar el cuadro (o al salir) no
 *     llega al juego hasta soltarse: el ○ del «No» no es un B, el START del
 *     atajo no pausa el juego al volver, y el × del «Sí» no abre la ROM
 *     elegida en el menú.
 *
 * Portable (sin PSL1GHT): pruebas en tests/unit/test_pausectl.c.
 */
#ifndef SALSA64_FRONTEND_PAUSECTL_H
#define SALSA64_FRONTEND_PAUSECTL_H

#include <stdbool.h>
#include <stdint.h>

#define PAUSECTL_PORTS 4

/* Motivos de la pausa (pausectl_reasons). */
#define PAUSECTL_SYSTEM  0x1u  /* menú del sistema, o mandos tomados por el sistema */
#define PAUSECTL_CONFIRM 0x2u  /* cuadro de confirmación de START + SELECT */

enum pausectl_action {
	PAUSECTL_NONE,
	PAUSECTL_EXIT,  /* salir del juego (× en el cuadro, o el atajo sin confirmación) */
};

struct pausectl {
	bool confirm;      /* pedir confirmación (menu.cfg, «confirmar = si|no») */
	bool system;       /* entre SYSUTIL_MENU_OPEN y SYSUTIL_MENU_CLOSE */
	bool intercepted;  /* en la última vuelta, los mandos eran del sistema */
	bool dialog;       /* el cuadro está a la vista */
	uint32_t prev[PAUSECTL_PORTS];     /* botones de la vuelta anterior */
	uint32_t swallow[PAUSECTL_PORTS];  /* no llegan al juego hasta soltarse */
};

void pausectl_init(struct pausectl *p, bool confirm);
/* Del callback del sistema: SYSUTIL_MENU_OPEN (true) o SYSUTIL_MENU_CLOSE. */
void pausectl_system_menu(struct pausectl *p, bool open);
/* Una lectura de los mandos: `raw`, los botones del DualShock 3 de cada
 * puerto (bits SALSA64_DS3_* de padmap.h; 0 sin mando); `intercepted`, el
 * bit del sistema; `in_game`, dentro de un juego (en el menú de Salsa64 no
 * hay cuadro). En `game`, lo que puede llegar al juego. */
enum pausectl_action pausectl_pads(struct pausectl *p, const uint32_t raw[PAUSECTL_PORTS], bool intercepted,
				   bool in_game, uint32_t game[PAUSECTL_PORTS]);
/* PAUSECTL_* activos; 0 si el juego corre. */
uint32_t pausectl_reasons(const struct pausectl *p);
/* Fin del juego: el cuadro se va (lo apretado se sigue tragando). */
void pausectl_game_end(struct pausectl *p);

/* Geometría del cuadro en un espacio lógico lw × lh (alto 720; ancho 960
 * en 4:3, 1280 en 16:9), centrado: la pregunta arriba y las respuestas
 * abajo, con la escala del texto (celdas de RSXKIT_FONT_W × RSXKIT_FONT_H). */
struct pausectl_box {
	float x, y, w, h;
	float qx, qy, qscale;  /* la pregunta */
	float ax, ay, ascale;  /* las respuestas */
};
void pausectl_layout(const char *question, const char *answers, float lw, float lh, struct pausectl_box *b);

#endif
