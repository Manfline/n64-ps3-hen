/*
 * Salsa64 — pruebas del menú para elegir ROM: la lista (src/frontend/romlist.c:
 * cabeceras en los tres órdenes de bytes, títulos, orden, directorio y la
 * última ROM), la navegación (src/frontend/menu.c: repetición, vuelta,
 * páginas, botones sueltos) y el texto de lib/rsxkit (src/text.c: glifos,
 * orden de bytes, recorte, UTF-8, «…»).
 *
 * Lo que toca el disco (directorio, última ROM) corre solo si quien llama
 * dio t_tmp_dir (el host y qemu; en la PS3 no).
 *
 * También la geometría del 2D de rsxkit (rsxkit/ui.h: atlas de la fuente con
 * Scale2x, texto, rectángulos, ondas) y la de cada cuadro del menú. Con
 * SALSA64_MENU_PPM=<prefijo> en el host, el menú rasterizado como lo
 * dibujaría el RSX se guarda en imágenes para revisarlo a ojo.
 */
#include "test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rsxkit/text.h>
#include <rsxkit/ui.h>

#include "core/rom.h"
#include "core/save.h"
#include "frontend/menu.h"
#include "frontend/padmap.h"
#include "frontend/romlist.h"

#define BIT(b) (1u << (b))
#define UP     BIT(SALSA64_DS3_UP)
#define DOWN   BIT(SALSA64_DS3_DOWN)
#define LEFT   BIT(SALSA64_DS3_LEFT)
#define RIGHT  BIT(SALSA64_DS3_RIGHT)
#define CROSS  BIT(SALSA64_DS3_CROSS)

const char *t_tmp_dir;

/* Cabecera .z64 con título, código de juego y versión. */
static void make_header(uint8_t h[0x40], const char *title, const char *code, uint8_t version)
{
	memset(h, 0, 0x40);
	h[0] = 0x80, h[1] = 0x37, h[2] = 0x12, h[3] = 0x40;
	memset(h + 0x20, ' ', 20);
	memcpy(h + 0x20, title, strlen(title) < 20 ? strlen(title) : 20);
	memcpy(h + 0x3B, code, 4);
	h[0x3F] = version;
}

/* De .z64 a otro orden: .v64 (pares) o .n64 (palabras invertidas). */
static void to_format(uint8_t *p, size_t n, enum salsa64_rom_format f)
{
	for (size_t i = 0; f == SALSA64_ROM_V64 && i + 1 < n; i += 2) {
		uint8_t t = p[i];
		p[i] = p[i + 1];
		p[i + 1] = t;
	}
	for (size_t i = 0; f == SALSA64_ROM_N64 && i + 3 < n; i += 4) {
		uint8_t a = p[i], b = p[i + 1];
		p[i] = p[i + 3];
		p[i + 1] = p[i + 2];
		p[i + 2] = b;
		p[i + 3] = a;
	}
}

static void menu_header_three_formats(void)
{
	static const enum salsa64_rom_format fmts[] = { SALSA64_ROM_Z64, SALSA64_ROM_V64, SALSA64_ROM_N64 };
	for (unsigned k = 0; k < 3; k++) {
		uint8_t h[0x40];
		struct rom_entry e;
		memset(&e, 0, sizeof e);
		make_header(h, "THE LEGEND OF ZELDA", "NZLE", 2);
		to_format(h, sizeof h, fmts[k]);
		romlist_parse_header(h, NULL, 0, &e);
		CHECK_EQ(e.format, fmts[k]);
		CHECK(!strcmp(e.title, "THE LEGEND OF ZELDA"));
		CHECK(!strcmp(e.code, "NZLE"));
		CHECK_EQ(e.region, 'E');
		CHECK_EQ(e.version, 2);
		CHECK_EQ(e.tv, SALSA64_TV_NTSC);
		CHECK_EQ(e.save, SALSA64_SAVE_SRAM_256K);
	}
}

static void menu_header_title_cleanup(void)
{
	uint8_t h[0x40];
	struct rom_entry e;
	make_header(h, "", "NSMP", 0);
	/* Ceros y espacios a los lados; un byte de katakana en medio. */
	memset(h + 0x20, 0, 20);
	memcpy(h + 0x22, "MARIO", 5);
	h[0x27] = 0xB1;
	h[0x28] = '6';
	romlist_parse_header(h, NULL, 0, &e);
	CHECK(!strcmp(e.title, "MARIO?6"));
	CHECK_EQ(e.tv, SALSA64_TV_PAL);
	CHECK(!strcmp(romlist_region_name(e.region), "Europa"));
	CHECK_EQ(e.save, SALSA64_SAVE_EEPROM_4K);
	CHECK(!strcmp(romlist_save_label(e.save), "EEPROM 4K"));

	/* Sin título: se muestra el nombre del archivo. */
	memset(h + 0x20, ' ', 20);
	memset(&e, 0, sizeof e);
	strcpy(e.path, "/x/homebrew.z64");
	e.name = 3;
	romlist_parse_header(h, NULL, 0, &e);
	CHECK(!strcmp(e.title, ""));
	CHECK(!strcmp(romlist_display_title(&e), "homebrew.z64"));
	CHECK(!strcmp(e.code, "NSMP"));
	memset(h + 0x3B, 0, 4);  /* homebrew sin código */
	romlist_parse_header(h, NULL, 0, &e);
	CHECK(!strcmp(e.code, ""));
	CHECK(!strcmp(romlist_region_name(e.region), "?"));

	/* guardado.cfg manda sobre la tabla. */
	static const char cfg[] = "NSM = eeprom16k\n";
	make_header(h, "SUPER MARIO 64", "NSME", 0);
	romlist_parse_header(h, cfg, sizeof cfg - 1, &e);
	CHECK_EQ(e.save, SALSA64_SAVE_EEPROM_16K);
}

static void menu_rom_extensions(void)
{
	CHECK(romlist_has_rom_ext("oot.z64"));
	CHECK(romlist_has_rom_ext("SM64.V64"));
	CHECK(romlist_has_rom_ext("a.N64"));
	CHECK(!romlist_has_rom_ext(".z64"));
	CHECK(!romlist_has_rom_ext("oot.z64.txt"));
	CHECK(!romlist_has_rom_ext("oot.zip"));
	CHECK(!romlist_has_rom_ext("oot.x64"));
	CHECK(!romlist_has_rom_ext("z64"));
}

static void add(struct romlist *l, const char *path, const char *title)
{
	struct rom_entry *e = &l->e[l->count++];
	memset(e, 0, sizeof *e);
	strcpy(e->path, path);
	e->name = (uint16_t)(strrchr(path, '/') + 1 - path);
	strcpy(e->title, title);
}

static void menu_sort_by_title(void)
{
	static struct romlist l;
	memset(&l, 0, sizeof l);
	add(&l, "/d/zz.z64", "super mario 64");
	add(&l, "/d/b.z64", "MARIO KART 64");
	add(&l, "/d/a.z64", "");                  /* se ordena como "a.z64" */
	add(&l, "/usb/oot.z64", "ZELDA");
	add(&l, "/d/oot.z64", "ZELDA");           /* mismo título y nombre: por ruta */
	add(&l, "/d/A2.z64", "Mario Kart 64");    /* mismo título: por nombre */
	romlist_sort(&l);
	static const char *order[] = { "/d/a.z64", "/d/A2.z64", "/d/b.z64", "/d/zz.z64", "/d/oot.z64", "/usb/oot.z64" };
	for (unsigned i = 0; i < 6; i++)
		CHECK(!strcmp(l.e[i].path, order[i]));
	CHECK_EQ(romlist_find(&l, "/usb/oot.z64"), 5);
	CHECK_EQ(romlist_find(&l, "/nada.z64"), -1);
}

static bool write_file(const char *path, const uint8_t *data, size_t n)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	bool ok = fwrite(data, 1, n, f) == n;
	return fclose(f) == 0 && ok;
}

static void menu_scan_dir(void)
{
	if (!t_tmp_dir)
		return;
	char dir[256], path[300];
	snprintf(dir, sizeof dir, "%s/roms", t_tmp_dir);
	mkdir(dir, 0777);
	static uint8_t rom[0x2000];
	struct { const char *name; const char *title; const char *code; enum salsa64_rom_format f; size_t size; } files[] = {
		{ "zelda.z64", "ZELDA", "NZLE", SALSA64_ROM_Z64, sizeof rom },
		{ "Mario.V64", "SUPER MARIO 64", "NSME", SALSA64_ROM_V64, sizeof rom },
		{ "kart.n64", "MARIO KART 64", "NKTE", SALSA64_ROM_N64, sizeof rom },
		{ "notas.txt", "NO", "NOPE", SALSA64_ROM_Z64, sizeof rom },
		{ "._zelda.z64", "NO", "NOPE", SALSA64_ROM_Z64, sizeof rom },
		{ "corta.z64", "NO", "NOPE", SALSA64_ROM_Z64, 0x800 },
	};
	for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) {
		memset(rom, 0, sizeof rom);
		make_header(rom, files[i].title, files[i].code, 0);
		to_format(rom, files[i].size, files[i].f);
		snprintf(path, sizeof path, "%s/%s", dir, files[i].name);
		CHECK(write_file(path, rom, files[i].size));
	}
	snprintf(path, sizeof path, "%s/carpeta.z64", dir);
	mkdir(path, 0777);

	static struct romlist l;
	memset(&l, 0, sizeof l);
	CHECK_EQ(romlist_scan_dir(&l, dir, NULL, 0), 3);
	CHECK_EQ(l.skipped, 2);  /* corta.z64 y carpeta.z64 */
	romlist_sort(&l);
	CHECK_EQ(l.count, 3);
	CHECK(!strcmp(l.e[0].title, "MARIO KART 64"));
	CHECK(!strcmp(romlist_name(&l.e[0]), "kart.n64"));
	CHECK_EQ(l.e[0].format, SALSA64_ROM_N64);
	CHECK(!strcmp(l.e[1].title, "SUPER MARIO 64"));
	CHECK_EQ(l.e[1].format, SALSA64_ROM_V64);
	CHECK_EQ(l.e[1].size, sizeof rom);
	CHECK(!strcmp(l.e[2].title, "ZELDA"));
	snprintf(path, sizeof path, "%s/zelda.z64", dir);
	CHECK(!strcmp(l.e[2].path, path));
	CHECK_EQ(romlist_scan_dir(&l, "/no/existe/salsa64", NULL, 0), -1);

	/* Lista llena: lo que no cabe cuenta como saltado. */
	l.count = ROMLIST_MAX - 1;
	l.skipped = 0;
	CHECK_EQ(romlist_scan_dir(&l, dir, NULL, 0), 1);
	CHECK_EQ(l.count, ROMLIST_MAX);
	CHECK_EQ(l.skipped, 4);
}

static void menu_last_rom(void)
{
	if (!t_tmp_dir)
		return;
	char file[256], path[ROMLIST_PATH_MAX];
	snprintf(file, sizeof file, "%s/ultima-rom", t_tmp_dir);
	unlink(file);
	CHECK(!romlist_load_last(file, path, sizeof path));
	CHECK(romlist_save_last(file, "/dev_usb000/salsa64/Mario Kart 64.z64"));
	CHECK(romlist_load_last(file, path, sizeof path));
	CHECK(!strcmp(path, "/dev_usb000/salsa64/Mario Kart 64.z64"));
	CHECK(romlist_save_last(file, "/dev_hdd0/salsa64/oot.z64"));
	CHECK(romlist_load_last(file, path, sizeof path));
	CHECK(!strcmp(path, "/dev_hdd0/salsa64/oot.z64"));
	unlink(file);
}

/* ---- Navegación ------------------------------------------------------------- */

static struct romlist nav_list;

static void nav_setup(struct menu *m, unsigned n, int sel)
{
	memset(&nav_list, 0, sizeof nav_list);
	for (unsigned i = 0; i < n; i++) {
		char p[32], t[16];
		snprintf(p, sizeof p, "/d/r%02u.z64", i);
		snprintf(t, sizeof t, "ROM %02u", i);
		add(&nav_list, p, t);
	}
	menu_init(m, &nav_list, sel, NULL);
}

static void menu_needs_release_first(void)
{
	struct menu m;
	nav_setup(&m, 4, 2);
	CHECK_EQ(m.sel, 2);
	CHECK(m.set.waves);
	/* START + SELECT del juego todavía apretados, y luego X: nada. */
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_START) | BIT(SALSA64_DS3_SELECT), 0), MENU_NONE);
	CHECK_EQ(menu_input(&m, CROSS, 1000), MENU_NONE);
	CHECK_EQ(menu_input(&m, CROSS | DOWN, 2000), MENU_NONE);
	CHECK_EQ(m.sel, 2);
	CHECK_EQ(menu_input(&m, 0, 3000), MENU_NONE);
	CHECK_EQ(menu_input(&m, CROSS, 4000), MENU_OPEN);
	CHECK_EQ(menu_selected(&m), 2);
	/* Mantener X no abre otra vez. */
	CHECK_EQ(menu_input(&m, CROSS, 5000), MENU_NONE);
	/* Mientras carga, nada. */
	m.loading = "ROM 02";
	menu_input(&m, 0, 6000);
	CHECK_EQ(menu_input(&m, CROSS, 7000), MENU_NONE);

	nav_setup(&m, 4, 99);  /* fuera de rango: la primera */
	CHECK_EQ(m.sel, 0);
}

static void menu_move_and_wrap(void)
{
	struct menu m;
	nav_setup(&m, 3, 0);  /* 3 ROMs + «Opciones» + «Salir al XMB» */
	CHECK_EQ(menu_rows(&m), 5);
	menu_input(&m, 0, 0);
	menu_input(&m, DOWN, 10);
	CHECK_EQ(m.sel, 1);
	menu_input(&m, 0, 20);
	menu_input(&m, UP, 30);
	menu_input(&m, 0, 40);
	menu_input(&m, UP, 50);   /* vuelta: de la primera a «Salir» */
	CHECK_EQ(m.sel, menu_row_exit(&m));
	CHECK_EQ(menu_selected(&m), -1);
	menu_input(&m, 0, 60);
	CHECK_EQ(menu_input(&m, CROSS, 70), MENU_QUIT);
	menu_input(&m, 0, 80);
	menu_input(&m, DOWN, 90);  /* vuelta al principio */
	CHECK_EQ(m.sel, 0);
}

static void menu_repeat(void)
{
	struct menu m;
	nav_setup(&m, 40, 0);
	menu_input(&m, 0, 0);
	uint64_t t = 1000000;
	menu_input(&m, DOWN, t);
	CHECK_EQ(m.sel, 1);
	menu_input(&m, DOWN, t + MENU_REPEAT_DELAY_US - 1);
	CHECK_EQ(m.sel, 1);
	menu_input(&m, DOWN, t + MENU_REPEAT_DELAY_US);
	CHECK_EQ(m.sel, 2);
	menu_input(&m, DOWN, t + MENU_REPEAT_DELAY_US + MENU_REPEAT_US - 1);
	CHECK_EQ(m.sel, 2);
	menu_input(&m, DOWN, t + MENU_REPEAT_DELAY_US + MENU_REPEAT_US);
	CHECK_EQ(m.sel, 3);
	/* Pasado el umbral, más rápido. */
	uint64_t u = t + MENU_REPEAT_FAST_AFTER_US;
	menu_input(&m, DOWN, u);
	int s = m.sel;
	menu_input(&m, DOWN, u + MENU_REPEAT_FAST_US);
	CHECK_EQ(m.sel, s + 1);
	/* La repetición no da la vuelta: se queda en «Salir al XMB». */
	for (int i = 0; i < 100; i++)
		menu_input(&m, DOWN, u + (uint64_t)(i + 2) * MENU_REPEAT_FAST_US);
	CHECK_EQ(m.sel, menu_row_exit(&m));
	/* Soltar corta la repetición. */
	menu_input(&m, 0, u + 200 * MENU_REPEAT_FAST_US);
	CHECK_EQ(m.rep_button, 0);
}

static void menu_pages(void)
{
	struct menu m;
	nav_setup(&m, 20, 0);  /* una página avanza 6 */
	menu_input(&m, 0, 0);
	menu_input(&m, RIGHT, 10);
	CHECK_EQ(m.sel, 6);
	menu_input(&m, 0, 20);
	menu_input(&m, LEFT, 30);
	CHECK_EQ(m.sel, 0);
	menu_input(&m, 0, 40);
	menu_input(&m, LEFT, 50);  /* no da la vuelta */
	CHECK_EQ(m.sel, 0);
	m.sel = 18;
	menu_input(&m, 0, 60);
	menu_input(&m, RIGHT, 70);
	CHECK_EQ(m.sel, menu_row_exit(&m));
}

static void menu_options_page(void)
{
	struct menu m;
	nav_setup(&m, 2, 0);
	menu_input(&m, 0, 0);
	/* X en «Opciones» abre la página; X cambia el fondo. */
	m.sel = 2;
	CHECK_EQ(menu_input(&m, CROSS, 10), MENU_NONE);
	CHECK_EQ(m.page, MENU_PAGE_OPTIONS);
	CHECK_EQ(m.opt, MENU_OPT_WAVES);
	CHECK_EQ(menu_selected(&m), -1);
	menu_input(&m, 0, 20);
	CHECK_EQ(menu_input(&m, CROSS, 30), MENU_SETTINGS);
	CHECK(!m.set.waves);
	menu_input(&m, 0, 40);
	CHECK_EQ(menu_input(&m, RIGHT, 50), MENU_SETTINGS);
	CHECK(m.set.waves);
	/* Recorte del borde: derecha avanza, izquierda vuelve, con vuelta. */
	menu_input(&m, 0, 60);
	menu_input(&m, DOWN, 61);
	CHECK_EQ(m.opt, MENU_OPT_OVERSCAN);
	CHECK_EQ(m.set.overscan, MENU_OVERSCAN_NORMAL);
	menu_input(&m, 0, 62);
	CHECK_EQ(menu_input(&m, RIGHT, 63), MENU_SETTINGS);
	CHECK_EQ(m.set.overscan, MENU_OVERSCAN_WIDE);
	menu_input(&m, 0, 64);
	menu_input(&m, CROSS, 65);
	CHECK_EQ(m.set.overscan, MENU_OVERSCAN_OFF);
	menu_input(&m, 0, 66);
	menu_input(&m, LEFT, 67);
	CHECK_EQ(m.set.overscan, MENU_OVERSCAN_WIDE);
	m.set.overscan = MENU_OVERSCAN_CUSTOM;  /* números propios en overscan.cfg */
	menu_input(&m, 0, 68);
	menu_input(&m, LEFT, 69);
	CHECK_EQ(m.set.overscan, MENU_OVERSCAN_NORMAL);
	CHECK(m.set.waves);
	/* Resolución: X o los lados alternan 720p / 1080p y piden guardar. */
	menu_input(&m, 0, 69);
	menu_input(&m, DOWN, 70);
	CHECK_EQ(m.opt, MENU_OPT_RESOLUTION);
	CHECK_EQ(m.set.resolution, MENU_RES_720P);
	menu_input(&m, 0, 71);
	CHECK_EQ(menu_input(&m, CROSS, 72), MENU_SETTINGS);
	CHECK_EQ(m.set.resolution, MENU_RES_1080P);
	menu_input(&m, 0, 73);
	CHECK_EQ(menu_input(&m, LEFT, 74), MENU_SETTINGS);
	CHECK_EQ(m.set.resolution, MENU_RES_720P);
	menu_input(&m, 0, 75);
	CHECK_EQ(menu_input(&m, RIGHT, 76), MENU_SETTINGS);
	CHECK_EQ(m.set.resolution, MENU_RES_1080P);
	CHECK_EQ(m.set.overscan, MENU_OVERSCAN_NORMAL);
	CHECK(m.set.waves);
	/* Escala interna: Auto → 1× … 5× → Auto (izquierda, al revés). */
	menu_input(&m, 0, 77);
	menu_input(&m, DOWN, 77);
	CHECK_EQ(m.opt, MENU_OPT_SCALE);
	CHECK_EQ(m.set.scale, 0);
	uint64_t t = 200;
	for (int want = 1; want <= 5; want++) {
		menu_input(&m, 0, t++);
		CHECK_EQ(menu_input(&m, RIGHT, t++), MENU_SETTINGS);
		CHECK_EQ(m.set.scale, want);
	}
	menu_input(&m, 0, t++);
	CHECK_EQ(menu_input(&m, RIGHT, t++), MENU_SETTINGS);
	CHECK_EQ(m.set.scale, 0);
	menu_input(&m, 0, t++);
	CHECK_EQ(menu_input(&m, LEFT, t++), MENU_SETTINGS);
	CHECK_EQ(m.set.scale, 5);
	menu_input(&m, 0, t++);
	CHECK_EQ(menu_input(&m, CROSS, t++), MENU_SETTINGS);
	CHECK_EQ(m.set.scale, 0);
	CHECK_EQ(m.set.resolution, MENU_RES_1080P);
	/* Lo elegido (1080p) difiere de lo que corre (720p): aparece «Aplicar ahora». */
	CHECK(menu_video_pending(&m));
	menu_input(&m, 0, 77);
	menu_input(&m, DOWN, 78);
	CHECK_EQ(m.opt, MENU_OPT_APPLY);
	menu_input(&m, 0, 79);
	CHECK_EQ(menu_input(&m, RIGHT, 79), MENU_NONE);  /* sin valor que cambiar */
	menu_input(&m, 0, 79);
	CHECK_EQ(menu_input(&m, CROSS, 79), MENU_APPLY);
	m.res_run = m.set.resolution;  /* main.c lo pone tras aplicar */
	m.scale_run = m.set.scale;
	CHECK(!menu_video_pending(&m));
	menu_input(&m, 0, 79);
	CHECK_EQ(menu_input(&m, 0, 79), MENU_NONE);
	CHECK_EQ(m.opt, MENU_OPT_BACK);  /* la fila desapareció: el cursor pasa a «Volver» */
	menu_input(&m, UP, 79);          /* arriba, «Mostrar FPS» y luego «Escala interna» */
	CHECK_EQ(m.opt, MENU_OPT_FPS);
	menu_input(&m, 0, 79);
	menu_input(&m, UP, 79);
	CHECK_EQ(m.opt, MENU_OPT_SCALE);
	menu_input(&m, 0, 79);
	menu_input(&m, DOWN, 79);
	CHECK_EQ(m.opt, MENU_OPT_FPS);   /* sin cambios pendientes se salta «Aplicar ahora» */
	/* Mostrar FPS: alterna sí / no y pide guardar. */
	CHECK(!m.set.fps);
	menu_input(&m, 0, 79);
	CHECK_EQ(menu_input(&m, CROSS, 79), MENU_SETTINGS);
	CHECK(m.set.fps);
	menu_input(&m, 0, 79);
	CHECK_EQ(menu_input(&m, LEFT, 79), MENU_SETTINGS);
	CHECK(!m.set.fps);
	menu_input(&m, 0, 79);
	menu_input(&m, DOWN, 79);
	CHECK_EQ(m.opt, MENU_OPT_BACK);
	menu_input(&m, 0, 80);
	CHECK_EQ(menu_input(&m, RIGHT, 90), MENU_NONE);  /* «Volver» no tiene valor */
	menu_input(&m, 0, 100);
	CHECK_EQ(menu_input(&m, CROSS, 110), MENU_NONE);
	CHECK_EQ(m.page, MENU_PAGE_GAMES);
	CHECK_EQ(m.sel, 2);
	/* Triángulo abre y círculo vuelve. */
	menu_input(&m, 0, 120);
	menu_input(&m, BIT(SALSA64_DS3_TRIANGLE), 130);
	CHECK_EQ(m.page, MENU_PAGE_OPTIONS);
	menu_input(&m, 0, 140);
	menu_input(&m, BIT(SALSA64_DS3_CIRCLE), 150);
	CHECK_EQ(m.page, MENU_PAGE_GAMES);
}

static void menu_settings_file(void)
{
	struct menu_settings s;
	menu_settings_default(&s);
	CHECK(s.waves);
	CHECK_EQ(s.overscan, MENU_OVERSCAN_NORMAL);
	CHECK_EQ(s.resolution, MENU_RES_720P);
	static const char off[] = "# comentario\n  FONDO = No\notra = cosa\n";
	menu_settings_parse(&s, off, sizeof off - 1);
	CHECK(!s.waves);
	static const char bad[] = "fondo = quizá\n";
	menu_settings_parse(&s, bad, sizeof bad - 1);
	CHECK(!s.waves);
	static const char on[] = "fondo=sí";
	menu_settings_parse(&s, on, sizeof on - 1);
	CHECK(s.waves);
	CHECK(!s.fps);
	static const char fps[] = "fps = si\n";
	menu_settings_parse(&s, fps, sizeof fps - 1);
	CHECK(s.fps);
	char text[128];
	s.waves = false;
	int n = menu_settings_format(&s, text, sizeof text);
	struct menu_settings t;
	menu_settings_default(&t);
	menu_settings_parse(&t, text, (size_t)n);
	CHECK(!t.waves);
	CHECK(t.fps);
}

static void menu_autopilot_opens_target(void)
{
	struct menu m;
	nav_setup(&m, 6, 4);
	enum menu_action a = MENU_NONE;
	uint32_t step = 0;
	for (; step < 40 && a == MENU_NONE; step++)
		a = menu_input(&m, menu_autopilot(&m, 1, step), step * 20000);
	CHECK_EQ(a, MENU_OPEN);
	CHECK_EQ(menu_selected(&m), 1);
	/* Desde la página de opciones, primero vuelve. */
	m.page = MENU_PAGE_OPTIONS;
	a = MENU_NONE;
	for (step = 0; step < 40 && a == MENU_NONE; step++)
		a = menu_input(&m, menu_autopilot(&m, menu_row_exit(&m), step), 1000000 + step * 20000);
	CHECK_EQ(a, MENU_QUIT);
}

/* ---- Texto ---------------------------------------------------------------- */

static void text_glyph_bytes(void)
{
	/* "A" de la fuente 8x13: filas 2–10, 0x18 0x24 0x42 0x42 0x42 0x7E 0x42 0x42 0x42. */
	static const uint8_t rows_a[13] = { 0, 0, 0x18, 0x24, 0x42, 0x42, 0x42, 0x7E, 0x42, 0x42, 0x42, 0, 0 };
	static uint8_t px[13][8 * 4];
	rsxkit_canvas c = { px, 8, 13, 8 * 4, RSXKIT_XRGB8888 };
	memset(px, 0, sizeof px);
	CHECK_EQ(rsxkit_text_draw(&c, 0, 0, "A", 0, 0x123456, 1), 1);
	for (int y = 0; y < 13; y++)
		for (int x = 0; x < 8; x++) {
			const uint8_t *p = px[y] + 4 * x;
			bool on = rows_a[y] & (0x80 >> x);
			CHECK_EQ(p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24, on ? 0x563412FFu : 0);
		}

	/* RGBA5551 en big-endian: blanco = 0xFFFF; rojo puro = 0xF801. */
	static uint8_t px16[13][16 * 2];
	rsxkit_canvas c16 = { px16, 16, 13, 16 * 2, RSXKIT_RGBA5551 };
	memset(px16, 0, sizeof px16);
	rsxkit_text_draw(&c16, 8, 0, "A", 0, 0xFF0000, 1);
	CHECK_EQ(px16[7][2 * (8 + 1)], 0xF8);   /* fila de la barra: 0x7E */
	CHECK_EQ(px16[7][2 * (8 + 1) + 1], 0x01);
	CHECK_EQ(px16[7][2 * 8], 0);
	CHECK_EQ(rsxkit_color(RSXKIT_RGB565, 0xFFFFFF), 0xFFFF);
	CHECK_EQ(rsxkit_color(RSXKIT_XRGB1555, 0x0000FF), 0x801F);
	CHECK_EQ(rsxkit_color(RSXKIT_RGBA8888, 0x102030), 0x102030FFu);
}

static void text_clip_utf8_ellipsis(void)
{
	static uint8_t px[20][40 * 4];
	rsxkit_canvas c = { px, 40, 20, 40 * 4, RSXKIT_XRGB8888 };
	memset(px, 0, sizeof px);
	/* Fuera por los cuatro lados y a escala 3: sin escribir fuera (ASan). */
	rsxkit_text_draw(&c, -10, -10, "WWWW", 0, 0xFFFFFF, 3);
	rsxkit_text_draw(&c, 30, 15, "WWWW", 0, 0xFFFFFF, 3);
	rsxkit_fill(&c, -5, -5, 100, 100, 0x000000);
	rsxkit_fill(&c, 35, 18, 10, 10, 0x0000FF);
	CHECK_EQ(px[19][39 * 4 + 3], 0xFF);
	CHECK_EQ(px[17][39 * 4 + 3], 0);

	CHECK_EQ(rsxkit_text_cells("año ñ…"), 6);
	CHECK_EQ(rsxkit_text_cells("\xff\xc3"), 2);  /* inválidos: un «?» por byte */
	CHECK(rsxkit_font_has(0x00F1));
	CHECK(rsxkit_font_has(0x25B6));
	CHECK(!rsxkit_font_has(0x3042));

	/* "ñ" se dibuja con su glifo y no como «?». */
	static uint8_t a[13][8 * 4], b[13][8 * 4];
	rsxkit_canvas ca = { a, 8, 13, 32, RSXKIT_XRGB8888 }, cb = { b, 8, 13, 32, RSXKIT_XRGB8888 };
	memset(a, 0, sizeof a);
	memset(b, 0, sizeof b);
	rsxkit_text_draw(&ca, 0, 0, "ñ", 0, 0xFFFFFF, 1);
	rsxkit_text_draw(&cb, 0, 0, "?", 0, 0xFFFFFF, 1);
	CHECK(memcmp(a, b, sizeof a) != 0);
	/* Lo que la fuente no tiene: «?»; un byte inválido: «�». */
	memset(a, 0, sizeof a);
	rsxkit_text_draw(&ca, 0, 0, "\xe3\x81\x82", 0, 0xFFFFFF, 1);
	CHECK(memcmp(a, b, sizeof a) == 0);
	memset(a, 0, sizeof a);
	memset(b, 0, sizeof b);
	rsxkit_text_draw(&ca, 0, 0, "\xff", 0, 0xFFFFFF, 1);
	rsxkit_text_draw(&cb, 0, 0, "\xef\xbf\xbd", 0, 0xFFFFFF, 1);
	CHECK(memcmp(a, b, sizeof a) == 0);

	/* Más celdas que el límite: max − 1 y «…». */
	CHECK_EQ(rsxkit_text_draw(&c, 0, 0, "abcdefgh", 4, 0xFFFFFF, 1), 4);
	CHECK_EQ(rsxkit_text_draw(&c, 0, 0, "abcd", 4, 0xFFFFFF, 1), 4);
	CHECK_EQ(rsxkit_text_draw(&c, 0, 0, "ab", 4, 0xFFFFFF, 1), 2);
}

/* ---- Geometría del menú y vista previa ---------------------------------------- */

static uint8_t atlas[RSXKIT_UI_ATLAS_H][RSXKIT_UI_ATLAS_W * 4];
static rsxkit_vertex verts[RSXKIT_UI_MAX_QUADS * 6];

static void ui_atlas_scale2x(void)
{
	rsxkit_ui_atlas(&atlas[0][0], RSXKIT_UI_ATLAS_W * 4);
	/* Todo blanco; el alfa es la forma. Borde de cada celda transparente. */
	unsigned opaque = 0;
	for (int y = 0; y < RSXKIT_UI_ATLAS_H; y++)
		for (int x = 0; x < RSXKIT_UI_ATLAS_W; x++) {
			const uint8_t *p = &atlas[y][4 * x];
			CHECK(p[0] == 0xFF && p[1] == 0xFF && p[2] == 0xFF);
			if (x % 18 == 0 || y % 28 == 0)
				CHECK_EQ(p[3], 0);
			opaque += p[3] != 0;
		}
	CHECK(opaque > 0);
	/* «A» (U+0041, índice 33): la barra de la fila 7 se vuelve las filas
	 * 14 y 15 del glifo al doble; arriba de la letra (filas 0–3), nada. */
	int ox = (33 % 16) * 18 + 1, oy = (33 / 16) * 28 + 1;
	CHECK_EQ(atlas[oy + 14][4 * (ox + 4) + 3], 0xFF);
	CHECK_EQ(atlas[oy + 15][4 * (ox + 11) + 3], 0xFF);
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 16; x++)
			CHECK_EQ(atlas[oy + y][4 * (ox + x) + 3], 0);
	/* El bloque blanco (después del último glifo) es opaco. */
	int wx = (217 % 16) * 18 + 1, wy = (217 / 16) * 28 + 1;
	CHECK_EQ(atlas[wy + 13][4 * (wx + 8) + 3], 0xFF);
}

static void ui_geometry(void)
{
	uint32_t dropped = rsxkit_ui_dropped();
	rsxkit_ui_batch(verts, 12, 1280, 720);
	rsxkit_ui_rect(10, 20, 30, 40, 0x11223344);
	CHECK_EQ(rsxkit_ui_count(), 6);
	CHECK(verts[0].pos[0] == 10 && verts[0].pos[1] == 20 && verts[0].pos[3] == 1);
	CHECK_EQ(verts[0].rgba[0], 0x11);
	CHECK_EQ(verts[0].rgba[3], 0x44);
	rsxkit_ui_rect(0, 0, 10, 10, 0x11223300);  /* transparente: nada */
	CHECK_EQ(rsxkit_ui_count(), 6);
	/* Espacios sin quads; el ancho cuenta todas las celdas. «a» llena el
	 * lote y «b» ya no cabe. */
	float w = rsxkit_ui_text(0, 0, "a b", 2.0f, 0xFFFFFFFF, 0);
	CHECK(w == 48.0f);
	CHECK_EQ(rsxkit_ui_count(), 12);
	CHECK_EQ(rsxkit_ui_dropped(), dropped + 1);
	CHECK(verts[6].pos[0] == 0 && verts[7].pos[0] == 16 && verts[8].pos[1] == 26);
	rsxkit_ui_batch(verts, sizeof verts / sizeof verts[0], 1280, 720);
	CHECK(rsxkit_ui_text(0, 0, "abcdefgh", 1.0f, 0xFFFFFFFF, 4) == 32.0f);
	CHECK_EQ(rsxkit_ui_count(), 4 * 6);
	/* Una onda: 64 tramos de lámina y dos de línea. */
	rsxkit_wave wv = { 400, 200, { 30, 10 }, { 1, 2 }, { 0.1f, -0.2f }, 0, 2, 0xFFFFFF80, 0xFFFFFF20 };
	uint32_t n0 = rsxkit_ui_count();
	rsxkit_ui_wave(&wv, 1.5f);
	CHECK_EQ(rsxkit_ui_count() - n0, 64 * 3 * 6);
	/* Grupos: el texto, la lámina y la línea (cada uno, una llamada al RSX). */
	const uint32_t *first;
	CHECK_EQ(rsxkit_ui_groups(&first), 3);
	CHECK_EQ(first[0], 0);
	CHECK_EQ(first[1], n0);
	CHECK_EQ(first[2], n0 + 64 * 6);
	for (uint32_t i = n0; i < rsxkit_ui_count(); i++)
		CHECK(verts[i].pos[0] >= 0 && verts[i].pos[0] <= 1280 && verts[i].pos[1] > 150 && verts[i].pos[1] < 700);
	rsxkit_ui_batch(NULL, 0, 0, 0);
	rsxkit_ui_rect(0, 0, 1, 1, 0xFFFFFFFF);
	CHECK_EQ(rsxkit_ui_count(), 0);
}

/* Rasteriza el lote como lo haría el RSX (téxeles con filtro bilineal,
 * textura × color, mezcla alfa) en una imagen RGB de w × h. */
static void raster(uint8_t *rgb, int w, int h, float lw, float lh, uint32_t n)
{
	float sx = w / lw, sy = h / lh;
	for (uint32_t t = 0; t + 2 < n; t += 3) {
		const rsxkit_vertex *v[3] = { &verts[t], &verts[t + 1], &verts[t + 2] };
		float x[3], y[3];
		for (int k = 0; k < 3; k++)
			x[k] = v[k]->pos[0] * sx, y[k] = v[k]->pos[1] * sy;
		float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
		if (fabsf(area) < 1e-6f)
			continue;
		int x0 = (int)floorf(fminf(x[0], fminf(x[1], x[2]))), x1 = (int)ceilf(fmaxf(x[0], fmaxf(x[1], x[2])));
		int y0 = (int)floorf(fminf(y[0], fminf(y[1], y[2]))), y1 = (int)ceilf(fmaxf(y[0], fmaxf(y[1], y[2])));
		x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > w ? w : x1, y1 = y1 > h ? h : y1;
		for (int py = y0; py < y1; py++)
			for (int px = x0; px < x1; px++) {
				float cx = px + 0.5f, cy = py + 0.5f;
				float b0 = ((x[1] - cx) * (y[2] - cy) - (x[2] - cx) * (y[1] - cy)) / area;
				float b1 = ((x[2] - cx) * (y[0] - cy) - (x[0] - cx) * (y[2] - cy)) / area;
				float b2 = 1.0f - b0 - b1;
				if (b0 < 0 || b1 < 0 || b2 < 0)
					continue;
				float s = b0 * v[0]->st[0] + b1 * v[1]->st[0] + b2 * v[2]->st[0] - 0.5f;
				float tt = b0 * v[0]->st[1] + b1 * v[1]->st[1] + b2 * v[2]->st[1] - 0.5f;
				int is = (int)floorf(s), it = (int)floorf(tt);
				float fs = s - is, ft = tt - it, ta = 0;
				for (int k = 0; k < 4; k++) {
					int ax = is + (k & 1), ay = it + (k >> 1);
					ax = ax < 0 ? 0 : ax >= RSXKIT_UI_ATLAS_W ? RSXKIT_UI_ATLAS_W - 1 : ax;
					ay = ay < 0 ? 0 : ay >= RSXKIT_UI_ATLAS_H ? RSXKIT_UI_ATLAS_H - 1 : ay;
					float wgt = ((k & 1) ? fs : 1 - fs) * ((k >> 1) ? ft : 1 - ft);
					ta += wgt * atlas[ay][4 * ax + 3] / 255.0f;
				}
				float a = ta * (b0 * v[0]->rgba[3] + b1 * v[1]->rgba[3] + b2 * v[2]->rgba[3]) / 255.0f;
				uint8_t *o = rgb + 3 * ((size_t)py * w + px);
				for (int c = 0; c < 3; c++) {
					float col = b0 * v[0]->rgba[c] + b1 * v[1]->rgba[c] + b2 * v[2]->rgba[c];
					o[c] = (uint8_t)(col * a + o[c] * (1 - a) + 0.5f);
				}
			}
	}
}

static void write_ppm(const char *path, const uint8_t *rgb, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	fwrite(rgb, 3, (size_t)w * h, f);
	fclose(f);
}

static struct romlist frame_list;

static void frame_list_setup(void)
{
	memset(&frame_list, 0, sizeof frame_list);
	static const char *titles[] = { "THE LEGEND OF ZELDA", "SUPER MARIO 64", "GOLDENEYE", "MARIO KART 64",
					"Perfect Dark", "CONKER BFD", "STARFOX64", "" };
	static const char *codes[] = { "NZLE", "NSME", "NGEE", "NKTE", "NPDE", "NFUE", "NFXE", "NXXE" };
	static const char *names[] = { "oot.z64", "sm64.z64", "goldeneye.z64", "mk64.z64", "perfectdark.z64",
				       "conker.z64", "sf64.v64", "homebrew-sin-título.n64" };
	for (unsigned i = 0; i < 8; i++) {
		uint8_t h[0x40];
		struct rom_entry *e = &frame_list.e[frame_list.count++];
		memset(e, 0, sizeof *e);
		snprintf(e->path, sizeof e->path, "%s/%s", i == 3 ? "/dev_usb000/salsa64" : "/dev_hdd0/salsa64", names[i]);
		e->name = (uint16_t)(strrchr(e->path, '/') + 1 - e->path);
		e->size = (i + 1) << 22;
		make_header(h, titles[i], codes[i], 0);
		romlist_parse_header(h, NULL, 0, e);
	}
	romlist_sort(&frame_list);
}

/* Cuadros del menú en el lote del host; devuelve los vértices del último. */
static uint32_t frames(struct menu *m, uint64_t from_us, uint64_t to_us, float lw)
{
	uint32_t n = 0;
	for (uint64_t t = from_us; t <= to_us; t += 16683) {
		rsxkit_ui_batch(verts, sizeof verts / sizeof verts[0], lw, 720);
		menu_frame(m, lw, 720, t);
		n = rsxkit_ui_count();
	}
	return n;
}

static void menu_frame_geometry(void)
{
	frame_list_setup();
	uint32_t dropped0 = rsxkit_ui_dropped();
	struct menu m;
	menu_init(&m, &frame_list, 3, NULL);
	snprintf(m.last, sizeof m.last, "%s", frame_list.e[3].path);
	uint32_t n_on = frames(&m, 1000000, 2000000, 1280);
	CHECK(m.intro == 1.0f);
	CHECK(n_on > 2000 && n_on < sizeof verts / sizeof verts[0]);
	CHECK_EQ(rsxkit_ui_dropped(), dropped0);

	/* Sin ondas: 64 tramos de lámina en tres ondas y dos de línea en dos. */
	m.set.waves = false;
	uint32_t n_off = frames(&m, 2000000, 2000000, 1280);
	CHECK_EQ(n_on - n_off, (3 * 64 + 2 * 2 * 64) * 6);

	/* Justo después de moverse, el panel está vacío; al detenerse, se llena. */
	uint32_t still = frames(&m, 2100000, 2100000, 1280);
	menu_input(&m, 0, 2100000);
	menu_input(&m, DOWN, 2100000);
	uint32_t moving = frames(&m, 2116000, 2116000, 1280);
	CHECK(moving < still);
	uint32_t settled = frames(&m, 2132000, 3200000, 1280);
	CHECK(m.pos == (float)m.sel);
	CHECK(settled > moving + 6 * 30);

	/* Opciones, 4:3 y la pantalla de carga: sin salirse del lote. */
	menu_input(&m, 0, 3300000);
	menu_input(&m, BIT(SALSA64_DS3_TRIANGLE), 3300000);
	frames(&m, 3316000, 4000000, 960);
	CHECK(m.page_t == 1.0f);
	m.loading = "SUPER MARIO 64";
	frames(&m, 4016000, 4016000, 960);
	static struct romlist empty;
	menu_init(&m, &empty, 0, NULL);
	m.dirs[m.ndirs++] = "/dev_hdd0/salsa64";
	frames(&m, 0, 500000, 1280);
	CHECK_EQ(rsxkit_ui_dropped(), dropped0);
	rsxkit_ui_batch(NULL, 0, 0, 0);
}

/* video.cfg: formato nuevo, formato viejo («1080» solo) y valores raros. */
static void menu_video_file(void)
{
	struct menu_settings s;
	menu_settings_default(&s);
	CHECK_EQ(s.resolution, MENU_RES_720P);
	CHECK_EQ(s.scale, 0);
	const char old[] = "1080\n";
	menu_video_parse(&s, old, sizeof old - 1);
	CHECK_EQ(s.resolution, MENU_RES_1080P);
	CHECK_EQ(s.scale, 0);
	const char old720[] = "720\n";
	menu_video_parse(&s, old720, sizeof old720 - 1);
	CHECK_EQ(s.resolution, MENU_RES_720P);
	const char nuevo[] = "# x\nSalida = 1080\nescala = 5\n";
	menu_video_parse(&s, nuevo, sizeof nuevo - 1);
	CHECK_EQ(s.resolution, MENU_RES_1080P);
	CHECK_EQ(s.scale, 5);
	const char bad[] = "escala = 9\nsalida = 4k\n";
	menu_video_parse(&s, bad, sizeof bad - 1);
	CHECK_EQ(s.scale, 5);
	CHECK_EQ(s.resolution, MENU_RES_1080P);
	const char autoc[] = "escala = auto\n";
	menu_video_parse(&s, autoc, sizeof autoc - 1);
	CHECK_EQ(s.scale, 0);
	char text[256];
	s.scale = 2;
	s.resolution = MENU_RES_720P;
	int n = menu_video_format(&s, text, sizeof text);
	struct menu_settings t;
	menu_settings_default(&t);
	t.resolution = MENU_RES_1080P;
	menu_video_parse(&t, text, (size_t)n);
	CHECK_EQ(t.scale, 2);
	CHECK_EQ(t.resolution, MENU_RES_720P);
}

/* SALSA64_MENU_PPM=<prefijo>: el menú como lo dibujaría el RSX en
 * <prefijo>-juegos.ppm, -opciones.ppm y -43.ppm, para verlo a ojo. */
static void menu_preview(void)
{
	const char *out = getenv("SALSA64_MENU_PPM");
	if (!out)
		return;
	static uint8_t rgb[720 * 1280 * 3];
	char path[256];
	rsxkit_ui_atlas(&atlas[0][0], RSXKIT_UI_ATLAS_W * 4);
	frame_list_setup();
	struct menu m;
	menu_init(&m, &frame_list, 4, NULL);
	m.out_w = 1280, m.out_h = 720;
	m.res_run = MENU_RES_1080P;  /* pidió 1080p y quedó 720p: sale la pista */
	snprintf(m.last, sizeof m.last, "%s", frame_list.e[4].path);
	uint32_t n = frames(&m, 1000000, 13000000, 1280);
	raster(rgb, 1280, 720, 1280, 720, n);
	snprintf(path, sizeof path, "%s-juegos.ppm", out);
	write_ppm(path, rgb, 1280, 720);
	menu_input(&m, 0, 13100000);
	menu_input(&m, BIT(SALSA64_DS3_TRIANGLE), 13100000);
	menu_input(&m, 0, 13110000);
	menu_input(&m, DOWN, 13111000);
	menu_input(&m, 0, 13112000);
	menu_input(&m, DOWN, 13113000);  /* la fila «Resolución» */
	n = frames(&m, 13116000, 14500000, 1280);
	raster(rgb, 1280, 720, 1280, 720, n);
	snprintf(path, sizeof path, "%s-resolucion.ppm", out);
	write_ppm(path, rgb, 1280, 720);
	menu_input(&m, 0, 14510000);
	menu_input(&m, DOWN, 14511000);  /* «Escala interna» */
	menu_input(&m, 0, 14512000);
	menu_input(&m, RIGHT, 14513000);
	menu_input(&m, 0, 14514000);
	menu_input(&m, RIGHT, 14515000);
	menu_input(&m, 0, 14516000);
	menu_input(&m, RIGHT, 14517000);
	menu_input(&m, 0, 14518000);
	menu_input(&m, RIGHT, 14519000);
	menu_input(&m, 0, 14519500);
	menu_input(&m, RIGHT, 14519900);  /* 5× */
	n = frames(&m, 14520000, 15500000, 1280);
	raster(rgb, 1280, 720, 1280, 720, n);
	snprintf(path, sizeof path, "%s-opciones.ppm", out);
	write_ppm(path, rgb, 1280, 720);
	n = frames(&m, 15500000, 15600000, 960);
	raster(rgb, 960, 720, 960, 720, n);
	snprintf(path, sizeof path, "%s-opciones43.ppm", out);
	write_ppm(path, rgb, 960, 720);
	menu_input(&m, 0, 15600500);
	menu_input(&m, DOWN, 15601000);  /* «Aplicar ahora» */
	snprintf(m.note, sizeof m.note, "No se pudo: sigue 1280x720");
	n = frames(&m, 15601500, 16600000, 960);
	raster(rgb, 960, 720, 960, 720, n);
	snprintf(path, sizeof path, "%s-aplicar43.ppm", out);
	write_ppm(path, rgb, 960, 720);
	n = frames(&m, 16600000, 16700000, 1280);
	raster(rgb, 1280, 720, 1280, 720, n);
	snprintf(path, sizeof path, "%s-aplicar.ppm", out);
	write_ppm(path, rgb, 1280, 720);
	menu_input(&m, 0, 16701000);
	menu_input(&m, UP, 16702000);  /* «Escala interna» con la nota */
	menu_input(&m, 0, 16703000);
	menu_input(&m, UP, 16704000);  /* «Resolución» con la nota */
	n = frames(&m, 16705000, 17700000, 960);
	raster(rgb, 960, 720, 960, 720, n);
	snprintf(path, sizeof path, "%s-resolucion43.ppm", out);
	write_ppm(path, rgb, 960, 720);
	menu_init(&m, &frame_list, frame_list.count, NULL);
	m.set.waves = false;
	menu_set_status(&m, "No se pudo leer /dev_usb000/salsa64/mk64.z64");
	n = frames(&m, 1000000, 2000000, 960);
	raster(rgb, 960, 720, 960, 720, n);
	snprintf(path, sizeof path, "%s-43.ppm", out);
	write_ppm(path, rgb, 960, 720);
	rsxkit_ui_batch(NULL, 0, 0, 0);
}

void t_suite_menu(void)
{
	RUN(menu_header_three_formats);
	RUN(menu_header_title_cleanup);
	RUN(menu_rom_extensions);
	RUN(menu_sort_by_title);
	RUN(menu_scan_dir);
	RUN(menu_last_rom);
	RUN(menu_needs_release_first);
	RUN(menu_move_and_wrap);
	RUN(menu_repeat);
	RUN(menu_video_file);
	RUN(menu_pages);
	RUN(menu_options_page);
	RUN(menu_settings_file);
	RUN(menu_autopilot_opens_target);
	RUN(text_glyph_bytes);
	RUN(text_clip_utf8_ellipsis);
	RUN(ui_atlas_scale2x);
	RUN(ui_geometry);
	RUN(menu_frame_geometry);
	RUN(menu_preview);
}
