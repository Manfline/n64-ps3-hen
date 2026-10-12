/*
 * Salsa64 — menú para elegir ROM, con una disposición inspirada en el XMB
 * (sin ningún recurso de Sony: todo se dibuja con rsxkit/ui.h).
 *
 * Página de juegos: una columna vertical con las ROMs de la lista
 * (romlist.h), «Opciones» y «Salir al XMB» al final. La fila elegida va
 * más grande y más brillante, con el nombre de archivo debajo; la columna
 * se desliza con suavizado al cambiar. A la derecha, un panel con lo que se
 * sabe de la elegida (archivo, ubicación, código, tamaño, región, tipo de
 * guardado) que aparece cuando la columna se detiene. De fondo, un
 * degradado que cambia despacio y ondas animadas (se pueden apagar en
 * Opciones). Texto con sombra.
 *
 * Controles: cruceta arriba/abajo (repite al mantenerla: primero
 * MENU_REPEAT_DELAY_US, después cada MENU_REPEAT_US y, pasado
 * MENU_REPEAT_FAST_AFTER_US, cada MENU_REPEAT_FAST_US; la vuelta de un
 * extremo al otro solo con una pulsación nueva), izquierda/derecha una
 * página, X abre, triángulo va a Opciones (fondo animado y recorte del
 * borde). En Opciones, X o izquierda/derecha cambian el valor y círculo
 * vuelve. Hasta ver todos los botones
 * sueltos no acepta nada (el START + SELECT con que se volvió del juego o el
 * X con que se abrió no cuentan dos veces).
 *
 * La lógica (menu_input) y la geometría (menu_frame, con rsxkit_ui_*) no
 * dependen de PSL1GHT: pruebas y vista previa en tests/unit/test_menu.c.
 */
#ifndef SALSA64_FRONTEND_MENU_H
#define SALSA64_FRONTEND_MENU_H

#include <stdbool.h>
#include <stddef.h>

#include "romlist.h"

#define MENU_REPEAT_DELAY_US      400000u
#define MENU_REPEAT_US            110000u
#define MENU_REPEAT_FAST_AFTER_US 1500000u
#define MENU_REPEAT_FAST_US       45000u
#define MENU_STATUS_MAX           120
#define MENU_DIRS_MAX             4

enum menu_action {
	MENU_NONE,
	MENU_OPEN,      /* abrir menu_selected() */
	MENU_QUIT,      /* «Salir al XMB» */
	MENU_SETTINGS,  /* cambió un ajuste: guardarlo */
	MENU_APPLY,     /* «Aplicar ahora»: poner la salida y la escala elegidas */
};

/* Recorte del overscan (SALSA64_DATA_DIR/overscan.cfg, ver main.c). */
enum menu_overscan {
	MENU_OVERSCAN_OFF,     /* 0: la imagen entera */
	MENU_OVERSCAN_NORMAL,  /* 8 y 6 píxeles por lado a 320 × 240 (por omisión) */
	MENU_OVERSCAN_WIDE,    /* 16 y 12 */
	MENU_OVERSCAN_CUSTOM,  /* otros números en overscan.cfg (se muestran, no se eligen) */
};

/* Resolución de salida (SALSA64_DATA_DIR/video.cfg, ver main.c): se lee al
 * arrancar y la fila «Aplicar ahora» la pone sin cerrar Salsa64. */
enum menu_resolution {
	MENU_RES_720P,
	MENU_RES_1080P,
};

/* Ajustes del menú: el fondo va en SALSA64_DATA_DIR/menu.cfg; el recorte
 * es el de overscan.cfg y la resolución la de video.cfg, que leen y
 * escriben en main.c. */
struct menu_settings {
	bool waves;     /* fondo animado («fondo = si|no») */
	int overscan;   /* enum menu_overscan */
	int resolution; /* enum menu_resolution */
	int scale;      /* escala interna del render: 0 automática, 1 .. 5 */
	bool fps;       /* contador de velocidad en el juego («fps = si|no»; SELECT + △) */
};

enum { MENU_PAGE_GAMES, MENU_PAGE_OPTIONS };
enum { MENU_OPT_WAVES, MENU_OPT_OVERSCAN, MENU_OPT_RESOLUTION, MENU_OPT_SCALE, MENU_OPT_APPLY, MENU_OPT_FPS, MENU_OPT_BACK,
       MENU_OPTS };
/* «Aplicar ahora» solo existe mientras lo elegido difiere de lo que corre. */

struct menu {
	const struct romlist *list;
	struct menu_settings set;
	int page;              /* MENU_PAGE_* */
	int sel;               /* juegos: 0 .. count + 1 (count: Opciones; count + 1: Salir) */
	int opt;               /* opciones: MENU_OPT_* */
	/* Lo que corre ahora (lo pone main.c): ancho × alto de la salida y la
	 * resolución y la escala pedidas (las de set.resolution y set.scale
	 * cuando coinciden, no hay nada que aplicar). */
	unsigned out_w, out_h;
	int res_run;           /* enum menu_resolution */
	int scale_run;         /* 0 automática, 1 .. 5 */
	char note[48];         /* resultado del último «Aplicar ahora» */
	bool armed;            /* ya se vieron todos los botones sueltos */
	uint32_t held;         /* botones del llamado anterior */
	uint32_t rep_button;   /* bit de la dirección que repite, o 0 */
	uint64_t rep_since, rep_next;
	char status[MENU_STATUS_MAX];     /* mensaje (p. ej. un error al abrir) */
	char last[ROMLIST_PATH_MAX];      /* última ROM abierta (se marca en el panel) */
	const char *dirs[MENU_DIRS_MAX];  /* dónde se buscó (para la lista vacía) */
	unsigned ndirs;
	const char *loading;   /* título que se está cargando, o NULL */
	/* Animación (menu_frame). */
	float pos, opt_pos;    /* fila mostrada: sigue a sel/opt con suavizado */
	float page_t;          /* 0 = juegos, 1 = opciones */
	float intro;           /* fundido de entrada, 0 .. 1 */
	float t;               /* segundos de animación */
	uint64_t last_us;
};

/* `sel`: fila inicial (la última ROM abierta; fuera de rango = la primera). */
void menu_init(struct menu *m, const struct romlist *list, int sel, const struct menu_settings *set);
/* Botones del DualShock 3 (bits SALSA64_DS3_* de padmap.h; el OR de todos
 * los mandos) y la hora en µs. */
enum menu_action menu_input(struct menu *m, uint32_t buttons, uint64_t now_us);
/* Índice de la ROM elegida en la lista, o -1 si es otra fila. */
int menu_selected(const struct menu *m);
/* Filas de la página de juegos: ROMs + Opciones + Salir. */
static inline int menu_rows(const struct menu *m) { return (int)m->list->count + 2; }
static inline int menu_row_exit(const struct menu *m) { return (int)m->list->count + 1; }
/* Hay un cambio de salida o de escala por aplicar. */
static inline bool menu_video_pending(const struct menu *m)
{
	return m->set.resolution != m->res_run || m->set.scale != m->scale_run;
}
void menu_set_status(struct menu *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Avanza la animación a now_us y dibuja el menú entero con rsxkit_ui_* en
 * un espacio lógico de lw × lh (alto 720; ancho según la tele). */
void menu_frame(struct menu *m, float lw, float lh, uint64_t now_us);

/* Ajustes: por omisión, desde el texto de menu.cfg y de vuelta a texto. */
void menu_settings_default(struct menu_settings *s);
void menu_settings_parse(struct menu_settings *s, const char *text, size_t len);
int menu_settings_format(const struct menu_settings *s, char *buf, size_t size);

/* video.cfg: «salida = 1080|720» y «escala = auto|1..5». También acepta el
 * formato viejo, una sola línea «1080» o «720». Solo toca resolution y
 * scale; lo que no entiende lo deja como estaba. */
void menu_video_parse(struct menu_settings *s, const char *text, size_t len);
int menu_video_format(const struct menu_settings *s, char *buf, size_t size);

/* Para la prueba automática: los botones que acercan la selección a la
 * fila `target` de la página de juegos (0 .. menu_row_exit) y la abren;
 * suelta todo en los llamados impares de `step` para que cada uno cuente
 * como pulsación nueva. */
uint32_t menu_autopilot(const struct menu *m, int target, uint32_t step);

#endif
