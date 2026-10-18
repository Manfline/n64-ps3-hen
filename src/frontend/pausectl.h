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
 *     abre el menú de pausa (PM_*): Continuar, Guardar estado, Cargar estado,
 *     Ranura (←→), Trucos, Imagen, Mostrar FPS, Reiniciar (botón Reset),
 *     Reiniciar desde cero y Salir. ↑↓ elige, × acepta, ○,
 *     START o START + SELECT otra vez lo cierran. «Salir» pide confirmación
 *     en el cuadro de abajo si `confirm`, y si no sale; los dos «Reiniciar»
 *     preguntan siempre («¿Seguro?», dialog_for dice cuál) y devuelven
 *     PAUSECTL_RESET o PAUSECTL_RESTART con ×.
 *   - «Imagen» abre su página (IM_*): Filtro, Densidad (solo con
 *     `image_density`: el CRT de máscara), Aspecto, Recorte, Escala
 *     interna e Interpolación (←→ o × cambian el valor: image_do con
 *     image_arg ±1),
 *     «Guardar para este juego» (×) y Volver. ○ o «Volver» regresan al menú
 *     de pausa; START o START + SELECT cierran todo y el juego sigue.
 *   - Con el cuadro a la vista: × sale; ○ o START + SELECT otra vez lo
 *     cierran y el juego sigue.
 *   - SELECT + □ dentro del juego abre la lista de trucos (si hay: panel_avail)
 *     con el juego en pausa: ↑↓ elige (mantenido repite; ←→ o L1/R1 saltan
 *     de a PAUSECTL_PANEL_PAGE), × pide activar o desactivar el elegido
 *     (toggle), ○, START o SELECT + □ de nuevo la cierran.
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
#define PAUSECTL_SYSTEM  0x1u  /* menú del sistema, su dibujo encima o mandos tomados por el sistema */
#define PAUSECTL_CONFIRM 0x2u  /* cuadro de confirmación de START + SELECT */
#define PAUSECTL_PANEL   0x4u  /* lista de trucos (SELECT + □) */
#define PAUSECTL_MENU    0x8u  /* menú de pausa (START + SELECT) */

/* Filas del menú de pausa y de su página «Imagen». */
enum { PM_RESUME, PM_SAVE, PM_LOAD, PM_SLOT, PM_CHEATS, PM_IMAGE, PM_FPS, PM_RESET, PM_RESTART, PM_EXIT, PM_ROWS };
enum { IM_FILTER, IM_DENSITY, IM_ASPECT, IM_CROP, IM_SCALE, IM_INTERP, IM_SAVE, IM_BACK, IM_ROWS };

enum pausectl_action {
	PAUSECTL_NONE,
	PAUSECTL_EXIT,  /* salir del juego (× en el cuadro, o el atajo sin confirmación) */
	PAUSECTL_RESET,    /* «Reiniciar (botón Reset)»: × en su cuadro (PRE_NMI y NMI) */
	PAUSECTL_RESTART,  /* «Reiniciar desde cero»: × en su cuadro (arranque en frío) */
};

struct pausectl {
	bool confirm;      /* pedir confirmación (menu.cfg, «confirmar = si|no») */
	bool system;       /* entre SYSUTIL_MENU_OPEN y SYSUTIL_MENU_CLOSE */
	bool drawing;      /* entre SYSUTIL_DRAW_BEGIN y SYSUTIL_DRAW_END */
	bool intercepted;  /* en la última vuelta, los mandos eran del sistema */
	bool dialog;       /* el cuadro está a la vista */
	enum pausectl_action dialog_for;  /* qué confirma el cuadro: PAUSECTL_EXIT, _RESET o _RESTART */
	/* Menú de pausa (START + SELECT): menu_sel es la fila elegida; menu_do, lo
	 * que se pidió (PM_SAVE, PM_LOAD, PM_SLOT con menu_arg ±1, PM_FPS, o
	 * PM_CHEATS sin trucos; -1 nada: quien llama lo lee y lo baja). */
	bool menu;
	int menu_sel, menu_do, menu_arg;
	/* Página «Imagen»: image_sel es la fila (IM_*); image_do, lo que se pidió
	 * (IM_FILTER … IM_INTERP con image_arg ±1, o IM_SAVE; -1 nada: quien
	 * llama lo lee y lo baja). */
	bool image;
	int image_sel, image_do, image_arg;
	bool image_density;  /* hay fila «Densidad» (la pone quien llama: el filtro es la máscara) */
	/* Lista de trucos. `panel_avail` y `panel_rows` los pone quien llama (hay
	 * trucos y cuántos); panel_sel es el elegido; toggle, el que se pidió
	 * cambiar (-1 ninguno: quien llama lo lee y lo baja); denied, se pidió
	 * abrirla sin trucos (quien llama avisa y lo baja). */
	bool panel, panel_avail, panel_denied;
	unsigned panel_rows;
	int panel_sel, toggle;
	uint32_t hold_bit, hold_n;  /* dirección mantenida y cuántas vueltas*/
	uint32_t prev[PAUSECTL_PORTS];     /* botones de la vuelta anterior */
	uint32_t swallow[PAUSECTL_PORTS];  /* no llegan al juego hasta soltarse */
};

void pausectl_init(struct pausectl *p, bool confirm);
/* Del callback del sistema: SYSUTIL_MENU_OPEN (true) o SYSUTIL_MENU_CLOSE. */
void pausectl_system_menu(struct pausectl *p, bool open);
/* SYSUTIL_DRAW_BEGIN (true) o SYSUTIL_DRAW_END: el sistema dibuja encima
 * (su menú o un aviso). También pausa. */
void pausectl_system_draw(struct pausectl *p, bool on);
/* El sistema está encima (menú o dibujo): en la PS3 retiene los flips de la
 * aplicación (segundos cada uno), así que no hay que esperarlos. */
static inline bool pausectl_overlay(const struct pausectl *p)
{
	return p->system || p->drawing;
}
/* Una lectura de los mandos: `raw`, los botones del DualShock 3 de cada
 * puerto (bits SALSA64_DS3_* de padmap.h; 0 sin mando); `intercepted`, el
 * bit del sistema; `in_game`, dentro de un juego (en el menú de Salsa64 no
 * hay cuadro). En `game`, lo que puede llegar al juego. */
#define PAUSECTL_PANEL_PAGE 8  /* filas que saltan ←→ y L1/R1 */
#define PAUSECTL_PANEL_VISIBLE 11  /* filas a la vista */

/* Primera fila a la vista de una lista de `rows` filas con `sel` elegida en
 * una ventana de PAUSECTL_PANEL_VISIBLE filas (elegida a media ventana). */
unsigned pausectl_panel_top(int sel, unsigned rows);

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
