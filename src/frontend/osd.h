/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — lo que se dibuja encima del juego (hilo de presentación, con
 * rsxkit/ui.h): un aviso corto (~2 s) y, si está encendido, el contador de
 * velocidad en una esquina.
 *
 * Contador: «VI 59,9 (100 %) · juego 30 · pantalla 60», promedio de ~0,5 s:
 *   VI        campos del VI emulados por segundo y, entre paréntesis, la
 *             velocidad: tiempo emulado (ciclos del VR4300) contra el real,
 *             así vale igual para NTSC, PAL y MPAL;
 *   juego     cuadros nuevos del juego por segundo: cambios del framebuffer
 *             que muestra el VI (20, 30 o 60 según el juego);
 *   pantalla  cuadros que presentó el RSX por segundo.
 * Todo lo cuenta el hilo de presentación con lecturas sueltas de lo que
 * escribe la emulación (64 bits alineados: atómicas en el PPE); nada en el
 * hilo de emulación.
 *
 * La geometría del texto es portable (osd_layout, para las pruebas); el
 * dibujo (osd_draw) es de la PS3.
 */
#ifndef SALSA64_FRONTEND_OSD_H
#define SALSA64_FRONTEND_OSD_H

#include <stdbool.h>
#include <stdint.h>

#define OSD_NOTICE_US 2000000u
#define OSD_FPS_US    500000u

struct osd_fps {
	bool on;
	/* Inicio de la ventana en curso. */
	uint64_t t0, cycles0;
	uint32_t fields0, frames0, presented0;
	/* Lo último medido. */
	bool valid;
	double vi_rate, speed, game_rate, screen_rate;
};

/* Una muestra (una vez por vuelta del hilo de presentación): campos del VI
 * y ciclos del CPU emulados hasta ahora, cuadros nuevos del juego y cuadros
 * presentados. Cada OSD_FPS_US recalcula. */
void osd_fps_sample(struct osd_fps *f, uint64_t now_us, uint32_t fields, uint64_t cycles, uint32_t game_frames,
		    uint32_t presented);
/* El texto del contador («VI 59,9 (100 %) · juego 30 · pantalla 60»). */
void osd_fps_text(const struct osd_fps *f, char *buf, unsigned size);
/* Olvida la ventana (otro juego, estado cargado: el campo retrocede). */
void osd_fps_reset(struct osd_fps *f);

/* Aviso: el texto se muestra OSD_NOTICE_US desde `now_us`. */
void osd_notice(uint64_t now_us, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void osd_notice_clear(void);

/* Dibuja el aviso y el contador sobre el cuadro en curso (entre
 * rsxkit_frame_begin y rsxkit_frame_end), en un espacio lógico de lw × lh.
 * No hace nada si no hay qué mostrar. Solo PS3. */
void osd_draw(const struct osd_fps *f, uint64_t now_us, float lw, float lh);

/* El cuadro de confirmación de START + SELECT (geometría de
 * pausectl_layout): la imagen del juego un poco oscurecida y, al centro, la
 * pregunta y las respuestas. Mismo cuadro en curso y espacio lógico que
 * osd_draw. Solo PS3. */
void osd_confirm_draw(const char *question, const char *answers, float lw, float lh);

/* La lista de trucos del juego (SELECT + □): una ventana de filas con una
 * marca [x] en los activos, la fila `sel` resaltada y la ayuda abajo. Mismo
 * cuadro en curso y espacio lógico que osd_draw. Solo PS3. */
struct cheat_set;
void osd_cheats_draw(const struct cheat_set *cs, int sel, float lw, float lh);

/* Líneas de barrido sobre la imagen del juego (filtros «Líneas» de
 * VIDEOGAMES SCZ; `level` 0 ninguna, 1 suaves, 2 fuertes): x, y, w, h en el
 * espacio lógico de osd_draw, `lines` líneas de la imagen. Las dibuja
 * osd_draw, debajo del aviso y del contador, en el mismo lote. */
void osd_scanlines_set(int level, float x, float y, float w, float h, unsigned lines);

/* Menú de pausa (START + SELECT, pausectl.h): lo que se muestra. */
struct osd_pause_menu {
	int sel;                   /* fila elegida (PM_*) */
	unsigned slot;             /* ranura actual */
	bool slot_has, fps_on, in_menu;
	unsigned cheats_total, cheats_on;
	const char *version;       /* versión corta (version.h), chica a la derecha del título; o NULL */
};
void osd_pause_menu_draw(const struct osd_pause_menu *m, float lw, float lh);

/* Página «Imagen» del menú de pausa (pausectl.h, IM_*): un cuadro abajo a la
 * izquierda, sin oscurecer la pantalla, para ver el cambio en el juego. Los
 * textos van en español y se traducen aquí. */
struct osd_image_menu {
	int sel;                /* fila elegida (IM_*) */
	const char *value[6];   /* filtro, densidad, aspecto, recorte, escala e interpolación (por IM_*) */
	bool density;           /* hay fila «Densidad» (el filtro es la máscara) */
	bool scale_pending;     /* la escala elegida se pone al seguir (lo dice una nota) */
	bool can_save;          /* hay ROM a la que guardarle los ajustes */
	const char *credit;     /* de quién es el filtro */
};
void osd_image_menu_draw(const struct osd_image_menu *m, float lw, float lh);

#endif
