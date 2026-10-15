/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — menú para elegir ROM, con una disposición inspirada en el XMB
 * (sin ningún recurso de Sony: todo se dibuja con rsxkit/ui.h).
 *
 * Página de juegos: una columna vertical con las carpetas y ROMs de la lista
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
 * página, X abre, L3 vuelve a leer la lista, triángulo va a Opciones (fondo
 * animado y recorte del borde). En Opciones, X o izquierda/derecha cambian
 * el valor y círculo vuelve. Hasta ver todos los botones
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

#include "cheats.h"
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
	MENU_DIRECTORY, /* entrar en una carpeta o volver al directorio padre */
	MENU_QUIT,      /* «Salir al XMB» */
	MENU_SETTINGS,  /* cambió un ajuste: guardarlo */
	MENU_APPLY,     /* «Aplicar ahora»: poner la salida y la escala elegidas */
	MENU_CHEATS,    /* □ sobre una ROM: cargar sus trucos y llamar a menu_cheats_open */
	MENU_CHEATS_CHANGED, /* se activó o desactivó un truco: guardar los activos */
	MENU_GAME,      /* SELECT sobre una ROM: cargar sus ajustes y llamar a menu_game_open */
	MENU_GAME_CHANGED, /* cambió un ajuste de ese juego: guardarlo */
	MENU_DOWNLOAD,  /* «Descargar»: carátulas o trucos (menu.dl_what) */
	MENU_REFRESH,   /* volver a leer la carpeta visible */
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

/* Aspecto de la imagen del juego (menu.cfg, «aspecto = 4:3|16:9»): 4:3 es
 * la original; 16:9 la estira a lo ancho para llenar una tele panorámica. */
enum menu_aspect {
	MENU_ASPECT_4_3,
	MENU_ASPECT_16_9,
};

/* Filtro de la imagen del juego (menu.cfg, «filtro = ...»; clave y nombre en
 * menu.c, filter_info). Los sencillos son de VIDEOGAMES SCZ; los CRT,
 * originales del autor portados de sus versiones de RetroMac
 * (frontend/filters), cada familia con su crédito en el panel. */
enum menu_filter {
	MENU_FILTER_SMOOTH,         /* suavizada (la de siempre) */
	MENU_FILTER_SHARP,          /* nítida: píxeles sin suavizar */
	MENU_FILTER_SCAN_SOFT,      /* líneas de barrido suaves */
	MENU_FILTER_SCAN_STRONG,    /* líneas de barrido marcadas */
	MENU_FILTER_CRT_MASK,       /* CRT de máscara de sombra */
	MENU_FILTER_CRT_RGB,        /* CRT con cable RGB */
	MENU_FILTER_CRT_SVIDEO,     /* CRT con cable S-Video */
	MENU_FILTER_CRT_COMPOSITE,  /* CRT con cable compuesto */
	MENU_FILTER_CRT_TRINITRON,  /* CRT Trinitron de la sala (Sony KV-29), sin cable */
	MENU_FILTERS,
};

/* Idioma del menú (menu.cfg, «idioma = es|en|fr|de|it|pt»), ver i18n.h. */
enum menu_lang { MENU_LANG_ES, MENU_LANG_EN, MENU_LANG_FR, MENU_LANG_DE, MENU_LANG_IT, MENU_LANG_PT, MENU_LANGS };
enum menu_rom_order { MENU_ROM_ORDER_TITLE, MENU_ROM_ORDER_FILENAME };

/* Densidad de los puntos del CRT de máscara de sombra: la del filtro
 * original o la fina (puntos más chicos y más juntos). */
enum menu_density { MENU_DENSITY_NORMAL, MENU_DENSITY_FINE, MENU_DENSITIES };

/* Interpolación de cuadros (menu.cfg y juegos/<ROM>.cfg, «interpolacion =
 * no|geometria»; docs/interpolacion.md): un juego de 20 o 30 cuadros se ve
 * a 60 dibujando en el RSX los campos de en medio con las posiciones de los
 * vértices entre un cuadro y el siguiente (salsa64_rdp_interp). */
enum menu_interp { MENU_INTERP_OFF, MENU_INTERP_GEOMETRY, MENU_INTERPS };

/* Ajustes de un solo juego (juegos/<ROM>.cfg): -1 es «lo global». */
struct menu_game {
	int aspect;    /* -1 o enum menu_aspect */
	int filter;    /* -1 o enum menu_filter */
	int overscan;  /* -1 o MENU_OVERSCAN_OFF / NORMAL / WIDE */
	int scale;     /* -1, 0 (automática) o 1 .. MENU_SCALE_MAX: escala interna */
	int density;   /* -1 o enum menu_density (solo con la máscara) */
	int interp;    /* -1 o enum menu_interp */
};
/* MENU_GAME_DENSITY solo es una fila si el filtro que vale es la máscara. */
enum { MENU_GAME_ASPECT, MENU_GAME_FILTER, MENU_GAME_DENSITY, MENU_GAME_OVERSCAN, MENU_GAME_SCALE, MENU_GAME_INTERP,
       MENU_GAME_BACK, MENU_GAME_ROWS };
#define MENU_SCALE_MAX 5

/* Ajustes del menú: el fondo, el contador y la confirmación al salir van en
 * SALSA64_DATA_DIR/menu.cfg; el recorte
 * es el de overscan.cfg y la resolución la de video.cfg, que leen y
 * escriben en main.c. */
struct menu_settings {
	bool waves;     /* fondo animado («fondo = si|no») */
	int overscan;   /* enum menu_overscan */
	int resolution; /* enum menu_resolution */
	int scale;      /* escala interna del render: 0 automática, 1 .. 5 */
	bool fps;       /* contador de velocidad en el juego («fps = si|no»; SELECT + △) */
	bool confirm;   /* START + SELECT pregunta antes de salir del juego («confirmar = si|no») */
	int aspect;     /* enum menu_aspect («aspecto = 4:3|16:9») */
	int filter;     /* enum menu_filter («filtro = suave|nitido|lineas|lineas+|crt-mascara|crt-rgb|
	                   crt-svideo|crt-compuesto|crt-trinitron») */
	int density;    /* enum menu_density («densidad = normal|fina»), la de la máscara */
	int lang;       /* enum menu_lang */
	int interp;     /* enum menu_interp («interpolacion = no|geometria») */
	int rom_order;  /* enum menu_rom_order («orden = titulo|archivo») */
};

enum { MENU_PAGE_GAMES, MENU_PAGE_OPTIONS, MENU_PAGE_CHEATS, MENU_PAGE_GAME };
enum { MENU_OPT_WAVES, MENU_OPT_OVERSCAN, MENU_OPT_ASPECT, MENU_OPT_FILTER, MENU_OPT_DENSITY, MENU_OPT_RESOLUTION,
       MENU_OPT_SCALE, MENU_OPT_APPLY, MENU_OPT_INTERP, MENU_OPT_FPS, MENU_OPT_CONFIRM, MENU_OPT_LANG,
       MENU_OPT_ROM_ORDER, MENU_OPT_DL_COVERS, MENU_OPT_DL_CHEATS, MENU_OPT_REFRESH, MENU_OPT_BACK, MENU_OPTS };
/* «Aplicar ahora» solo existe mientras lo elegido difiere de lo que corre;
 * «Densidad», solo con el CRT de máscara de sombra. */

struct menu {
	const struct romlist *list;
	struct menu_settings set;
	int page;              /* MENU_PAGE_* */
	int sel;               /* juegos: entradas, Opciones y Salir al XMB */
	int opt;               /* opciones: MENU_OPT_* */
	/* Trucos de la ROM elegida (los carga main.c): una fila por truco y «Volver». */
	struct cheat_set *cheats;
	const char *cht_title; /* título de esa ROM */
	int cht_sel;
	float cht_pos, cht_t;  /* fila mostrada y entrada de la página (0 .. 1) */
	/* Ajustes de la ROM elegida (los carga main.c). */
	struct menu_game game;
	const char *game_title;
	int game_sel;
	int game_rom;          /* la ROM de esa página (índice en la lista) */
	float game_pos, game_t;
	int dl_what;           /* con MENU_DOWNLOAD: 0 carátulas, 1 trucos */
	bool dl_enabled;       /* filas «Descargar …» (experimental: download.h, dl_enabled) */
	/* Carátula de la ROM cover_rom, ya subida a la ranura 0 de rsxkit_ui por
	 * main.c (-1: ninguna), de cover_w × cover_h puntos. cover_t es su
	 * aparición, 0 .. 1. */
	int cover_rom;
	unsigned cover_w, cover_h;
	float cover_t;
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
	bool quit_ask;         /* ○ en la lista: «¿Salir al XMB?» a la vista (× sale, ○ sigue) */
	const char *dirs[MENU_DIRS_MAX];  /* dónde se buscó (para la lista vacía) */
	unsigned ndirs;
	const char *loading;   /* título que se está cargando, o NULL */
	/* Arriba a la derecha, chico, bajo el contador (los pone main.c): la
	 * versión corta (version.h) y, en un paquete de prueba, «título ·
	 * APPID»; NULL, nada. */
	const char *version, *build;
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
/* Fila de una carpeta por nombre en la lista reconstruida, o -1. */
int menu_find_directory(const struct romlist *list, const char *name);
/* Filas de la página de juegos: ROMs + Opciones + Salir. */
static inline int menu_rows(const struct menu *m) { return (int)m->list->count + 2; }
static inline int menu_row_exit(const struct menu *m) { return (int)m->list->count + 1; }
/* Hay un cambio de salida o de escala por aplicar. */
static inline bool menu_video_pending(const struct menu *m)
{
	return m->set.resolution != m->res_run || m->set.scale != m->scale_run;
}
/* La selección lleva quieta lo suficiente para cargar su carátula: página de
 * juegos, sobre una ROM, con la columna ya detenida. */
static inline bool menu_settled(const struct menu *m)
{
	float d = m->pos - (float)m->sel;
	return menu_selected(m) >= 0 && m->page_t < 0.01f && d > -0.02f && d < 0.02f;
}
void menu_set_status(struct menu *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Abre la página de trucos de `title` con `cs` (que debe seguir vivo mientras
 * esté abierta; X cambia sus `on`). */
void menu_cheats_open(struct menu *m, struct cheat_set *cs, const char *title);
static inline int menu_cheat_rows(const struct menu *m) { return (m->cheats ? (int)m->cheats->n : 0) + 1; }

/* Abre la página de ajustes de `title` con `g` (se copia). */
void menu_game_open(struct menu *m, const struct menu_game *g, const char *title);
void menu_game_default(struct menu_game *g);
/* juegos/<ROM>.cfg: «aspecto|filtro|recorte|escala = global|...». */
void menu_game_parse(struct menu_game *g, const char *text, size_t len);
int menu_game_format(const struct menu_game *g, char *buf, size_t size);
/* Lo que vale para un juego: lo suyo, o si es -1 lo global (la escala
 * global es s->scale, la de video.cfg). Cualquiera puede ser NULL. */
void menu_game_effective(const struct menu_settings *s, const struct menu_game *g, int *aspect, int *filter,
			 int *overscan, int *scale);
/* La densidad que vale para el juego (la suya o la de Opciones). */
int menu_game_density(const struct menu_settings *s, const struct menu_game *g);
/* Si el filtro tiene densidad (el CRT de máscara de sombra) y el nombre de
 * una densidad (en español: se traduce con tr() al dibujar). */
bool menu_filter_has_density(int filter);
const char *menu_density_name(int density);
/* La interpolación que vale para el juego (la suya o la de Opciones) y el
 * nombre de un valor (en español: se traduce con tr() al dibujar). */
int menu_game_interp(const struct menu_settings *s, const struct menu_game *g);
const char *menu_interp_name(int interp);

/* Nombres de los valores (en español: se traducen con tr() al dibujar),
 * para el menú de pausa. La escala automática dice cuál es según la
 * resolución (enum menu_resolution). */
const char *menu_aspect_name(int aspect);
const char *menu_filter_name(int filter);
const char *menu_filter_credit(int filter);
const char *menu_overscan_name(int overscan);
const char *menu_scale_name(int scale, int resolution, char *buf, size_t size);

/* Avanza la animación a now_us y dibuja el menú entero con rsxkit_ui_* en
 * un espacio lógico de lw × lh (alto 720; ancho según la tele). */
void menu_frame(struct menu *m, float lw, float lh, uint64_t now_us);

/* Ajustes: por omisión, desde el texto de menu.cfg y de vuelta a texto. */
void menu_settings_default(struct menu_settings *s);
void menu_settings_parse(struct menu_settings *s, const char *text, size_t len);
int menu_settings_format(const struct menu_settings *s, char *buf, size_t size);
/* Si algo de lo que va en menu.cfg difiere entre a y b (lo de overscan.cfg y
 * video.cfg no cuenta): compara los textos de menu_settings_format, así una
 * clave nueva de menu.cfg no se queda sin guardar. */
bool menu_settings_cfg_differs(const struct menu_settings *a, const struct menu_settings *b);

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
