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
#include "frontend/cover.h"
#include "test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <rsxkit/text.h>
#include <rsxkit/ui.h>

#include "core/rom.h"
#include "core/save.h"
#include "frontend/i18n.h"
#include "frontend/menu.h"
#include "frontend/padmap.h"
#include "frontend/romlist.h"
#include "frontend/version.h"

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
	e.name = "juego-japones.z64";
	CHECK(!e.title_supported);
	CHECK(!strcmp(romlist_display_title(&e), "juego-japones.z64"));
	CHECK_EQ(e.tv, SALSA64_TV_PAL);
	CHECK(!strcmp(romlist_region_name(e.region), "Europa"));
	CHECK_EQ(e.save, SALSA64_SAVE_EEPROM_4K);
	CHECK(!strcmp(romlist_save_label(e.save), "EEPROM 4K"));

	/* Sin título: se muestra el nombre del archivo. */
	memset(h + 0x20, ' ', 20);
	memset(&e, 0, sizeof e);
	e.name = "homebrew.z64";
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
	const char *base = strrchr(path, '/') + 1;
	char *pool = (char *)(l->e + ROMLIST_MAX);
	e->name = pool + l->strings_used;
	strcpy((char *)e->name, base);
	l->strings_used += (uint32_t)strlen(base) + 1;
	e->source = !strncmp(path, "/usb/", 5) ? 1 : 0;
	strcpy(e->title, title);
	e->kind = ROMLIST_ROM;
	e->title_supported = 1;
	l->rom_count++;
}

static void menu_sort_by_title(void)
{
	struct romlist *l = romlist_create();
	add(l, "/d/zz.z64", "super mario 64");
	add(l, "/d/b.z64", "MARIO KART 64");
	add(l, "/d/a.z64", "");                  /* se ordena como "a.z64" */
	add(l, "/usb/oot.z64", "ZELDA");
	add(l, "/d/oot.z64", "ZELDA");           /* mismo título y nombre: por ruta */
	add(l, "/d/A2.z64", "Mario Kart 64");    /* mismo título: por nombre */
	romlist_sort(l, ROMLIST_ORDER_TITLE);
	static const char *roots[] = { "/d", "/usb" };
	static const char *names[] = { "a.z64", "A2.z64", "b.z64", "zz.z64", "oot.z64", "oot.z64" };
	for (unsigned i = 0; i < 6; i++)
		CHECK(!strcmp(romlist_name(&l->e[i]), names[i]));
	CHECK_EQ(romlist_find(l, "/usb/oot.z64", roots, 2), 5);
	CHECK_EQ(romlist_find(l, "/nada.z64", roots, 2), -1);
	romlist_free(l);
}

static void menu_sort_by_filename(void)
{
	struct romlist *l = romlist_create();
	add(l, "/d/z.z64", "AAA");
	add(l, "/d/a.z64", "ZZZ");
	romlist_sort(l, ROMLIST_ORDER_FILENAME);
	CHECK(!strcmp(romlist_name(&l->e[0]), "a.z64"));
	CHECK(!strcmp(romlist_name(&l->e[1]), "z.z64"));
	romlist_free(l);
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

	struct romlist *l = romlist_create();
	CHECK(romlist_memory_bytes() <= 1572864u);
	CHECK_EQ(romlist_scan_dir(l, dir, dir, NULL, 0), 3);
	CHECK_EQ(l->skipped, 1);  /* solo corta.z64; la carpeta es una fila propia */
	romlist_sort(l, ROMLIST_ORDER_TITLE);
	CHECK_EQ(l->count, 4);
	CHECK_EQ(l->e[0].kind, ROMLIST_DIRECTORY);
	CHECK(!strcmp(romlist_name(&l->e[0]), "carpeta.z64"));
	CHECK(!strcmp(l->e[1].title, "MARIO KART 64"));
	CHECK(!strcmp(romlist_name(&l->e[1]), "kart.n64"));
	CHECK_EQ(l->e[1].format, SALSA64_ROM_N64);
	CHECK(!strcmp(l->e[2].title, "SUPER MARIO 64"));
	CHECK_EQ(l->e[2].format, SALSA64_ROM_V64);
	CHECK_EQ(l->e[2].size, sizeof rom);
	CHECK(!strcmp(l->e[3].title, "ZELDA"));
	snprintf(path, sizeof path, "%s/zelda.z64", dir);
	CHECK(!strcmp(romlist_name(&l->e[3]), "zelda.z64"));
	CHECK_EQ(l->rom_count, 3);
	CHECK_EQ(l->dir_count, 1);
	char usb[256], usbrom[300];
	snprintf(usb, sizeof usb, "%s/usb", t_tmp_dir);
	mkdir(usb, 0777);
	snprintf(usbrom, sizeof usbrom, "%s/zelda.z64", usb);
	snprintf(path, sizeof path, "%s/zelda.z64", dir);
	CHECK_EQ(link(path, usbrom), 0);
	const char *roots[] = { dir, usb };
	CHECK_EQ(romlist_scan_sources(l, roots, 2, "", NULL, 0), 4);
	CHECK_EQ(l->count, 5);
	char rebuilt[ROMLIST_PATH_MAX];
	int usb_at = romlist_find(l, usbrom, roots, 2);
	CHECK(usb_at >= 0);
	CHECK(romlist_build_path(l, &l->e[usb_at], roots, 2, rebuilt, sizeof rebuilt));
	CHECK(!strcmp(rebuilt, usbrom));
	char sub[300];
	snprintf(sub, sizeof sub, "%s/sub", dir);
	mkdir(sub, 0777);
	memset(l, 0, romlist_memory_bytes());
	CHECK_EQ(romlist_scan_dir(l, dir, dir, NULL, 0), 3);
	CHECK_EQ(l->count, 5); /* ROMs, carpeta y subcarpeta */
	CHECK_EQ(l->e[0].kind, ROMLIST_DIRECTORY);
	memset(l, 0, romlist_memory_bytes());
	CHECK_EQ(romlist_scan_dir(l, sub, dir, NULL, 0), 0);
	CHECK_EQ(l->e[0].kind, ROMLIST_PARENT);
	CHECK(!strcmp(romlist_display_title(&l->e[0]), ".."));
	CHECK_EQ(romlist_scan_dir(l, "/no/existe/salsa64", "/no/existe/salsa64", NULL, 0), -1);

	/* Las carpetas de datos propios no salen en la raíz; «juegos» solo si trae ROMs. */
	char own[400];
	static const char *const hidden[] = { "saves", "states", "covers", "cheats", "perfilado", "perfiles", "capturas",
					      "cajanegra-3", "juegos" };
	for (unsigned i = 0; i < sizeof hidden / sizeof hidden[0]; i++) {
		snprintf(own, sizeof own, "%s/%s", sub, hidden[i]);
		mkdir(own, 0777);
	}
	snprintf(own, sizeof own, "%s/juegos/zelda.cfg", sub);
	CHECK(write_file(own, (const uint8_t *)"x", 1));
	snprintf(own, sizeof own, "%s/juegos/zelda.z64", sub);
	CHECK(write_file(own, rom, sizeof rom));
	snprintf(own, sizeof own, "%s/otra", sub);
	mkdir(own, 0777);
	memset(l, 0, romlist_memory_bytes());
	CHECK_EQ(romlist_scan_dir(l, sub, sub, NULL, 0), 0);
	CHECK_EQ(l->dir_count, 2);  /* «otra» y «juegos» (que ahora tiene una ROM) */
	snprintf(own, sizeof own, "%s/juegos/zelda.z64", sub);
	unlink(own);
	memset(l, 0, romlist_memory_bytes());
	CHECK_EQ(romlist_scan_dir(l, sub, sub, NULL, 0), 0);
	CHECK_EQ(l->dir_count, 1);  /* solo «otra» */
	snprintf(own, sizeof own, "%s/otra/saves", sub);
	mkdir(own, 0777);
	memset(l, 0, romlist_memory_bytes());
	snprintf(own, sizeof own, "%s/otra", sub);
	CHECK_EQ(romlist_scan_dir(l, own, sub, NULL, 0), 0);
	CHECK_EQ(l->dir_count, 1);  /* fuera de la raíz sí se ve */

	/* Carpeta grande: llena las 4096 entradas, mide la lectura y rechaza la siguiente. */
	char many[300], source[300], target[320];
	snprintf(many, sizeof many, "%s/many", t_tmp_dir);
	mkdir(many, 0777);
	snprintf(source, sizeof source, "%s/zelda.z64", dir);
	for (unsigned i = 0; i < ROMLIST_MAX + 1; i++) {
		snprintf(target, sizeof target, "%s/rom-%04u.z64", many, i);
		CHECK_EQ(link(source, target), 0);
	}
	memset(l, 0, romlist_memory_bytes());
	clock_t start = clock();
	CHECK_EQ(romlist_scan_dir(l, many, t_tmp_dir, NULL, 0), ROMLIST_MAX - 1);
	unsigned elapsed_ms = (unsigned)((clock() - start) * 1000 / CLOCKS_PER_SEC);
	CHECK_EQ(l->rom_count, ROMLIST_MAX - 1);
	CHECK_EQ(l->count, ROMLIST_MAX);
	CHECK_EQ(l->skipped, 2); /* una fila «..» ocupa una entrada del tope */
	memset(l, 0, romlist_memory_bytes());
	const char *combined[] = { many, usb };
	CHECK_EQ(romlist_scan_sources(l, combined, 2, "", NULL, 0), ROMLIST_MAX);
	CHECK_EQ(l->count, ROMLIST_MAX);
	CHECK_EQ(l->skipped, 2); /* el límite también es común a ambas unidades */
	printf("lista ROM: lectura de %u archivos: %u ms; memoria reservada: %zu bytes\n", ROMLIST_MAX + 1,
	       elapsed_ms, romlist_memory_bytes());
	romlist_free(l);
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

static struct romlist *nav_list;

static void nav_setup(struct menu *m, unsigned n, int sel)
{
	if (!nav_list) nav_list = romlist_create();
	memset(nav_list, 0, sizeof *nav_list);
	for (unsigned i = 0; i < n; i++) {
		char p[32], t[16];
		snprintf(p, sizeof p, "/d/r%02u.z64", i);
		snprintf(t, sizeof t, "ROM %02u", i);
		add(nav_list, p, t);
	}
	menu_init(m, nav_list, sel, NULL);
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

static void menu_refresh_buttons(void)
{
	struct menu m;
	nav_setup(&m, 2, 1);
	menu_input(&m, 0, 0);
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_L3), 1), MENU_REFRESH);
	CHECK_EQ(m.page, MENU_PAGE_GAMES);

	menu_input(&m, 0, 2);
	m.page = MENU_PAGE_OPTIONS;
	m.opt = MENU_OPT_REFRESH;
	CHECK_EQ(menu_input(&m, 0, 3), MENU_NONE);
	CHECK_EQ(menu_input(&m, CROSS, 4), MENU_REFRESH);
	CHECK_EQ(m.page, MENU_PAGE_OPTIONS);
	static const char *const labels[] = { "Actualizar lista", "Refresh list", "Actualiser la liste",
					      "Liste aktualisieren", "Aggiorna elenco", "Atualizar lista" };
	static const char *const notices[] = { "3 ROMs encontradas", "3 ROMs found", "3 ROM trouvées",
					       "3 ROMs gefunden", "3 ROM trovate", "3 ROMs encontradas" };
	for (int lang = 0; lang < MENU_LANGS; lang++) {
		i18n_set(lang);
		CHECK(!strcmp(tr("Actualizar lista"), labels[lang]));
		menu_set_status(&m, "%u ROMs encontradas", 3u);
		CHECK(!strcmp(m.status, notices[lang]));
	}
	i18n_set(MENU_LANG_ES);
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
	/* ○ en la lista pregunta antes de salir: ○ sigue donde estaba (nada se
	 * mueve ni se abre mientras pregunta), × sale. */
	menu_input(&m, 0, 100);
	menu_input(&m, DOWN, 110);
	menu_input(&m, 0, 120);
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_CIRCLE), 130), MENU_NONE);
	CHECK(m.quit_ask);
	menu_input(&m, 0, 140);
	menu_input(&m, DOWN, 150);
	CHECK_EQ(m.sel, 1);
	CHECK(m.quit_ask);
	menu_input(&m, 0, 160);
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_CIRCLE), 170), MENU_NONE);
	CHECK(!m.quit_ask);
	CHECK_EQ(m.sel, 1);
	CHECK_EQ(m.page, MENU_PAGE_GAMES);
	menu_input(&m, 0, 180);
	menu_input(&m, BIT(SALSA64_DS3_CIRCLE), 190);
	menu_input(&m, 0, 200);
	CHECK_EQ(menu_input(&m, CROSS, 210), MENU_QUIT);
	CHECK(!m.quit_ask);
}

static void menu_directory_navigation(void)
{
	struct romlist *l = romlist_create();
	struct menu m;
	strcpy(l->directory, "");
	l->count = 1;
	l->e[0].kind = ROMLIST_DIRECTORY;
	l->e[0].name = "juegos";
	menu_init(&m, l, 0, NULL);
	menu_input(&m, 0, 0);
	CHECK_EQ(menu_input(&m, CROSS, 1), MENU_DIRECTORY);
	CHECK_EQ(menu_selected(&m), -1);
	strcpy(l->directory, "juegos");
	l->count = 1;
	l->e[0].kind = ROMLIST_PARENT;
	l->e[0].name = "..";
	menu_init(&m, l, 0, NULL);
	menu_input(&m, 0, 2);
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_CIRCLE), 3), MENU_DIRECTORY);
	CHECK_EQ(m.sel, 0);
	/* Al regresar de «sin-hle», el cursor queda en esa carpeta aunque la
	 * última ROM abierta siga en la lista y aparezca después. */
	strcpy(l->directory, "");
	memset(l->e, 0, 2 * sizeof l->e[0]);
	l->count = 2;
	l->e[0].kind = ROMLIST_DIRECTORY;
	l->e[0].name = "sin-hle";
	l->e[1].kind = ROMLIST_ROM;
	l->e[1].name = "ultima.z64";
	int at = menu_find_directory(l, "sin-hle");
	CHECK_EQ(at, 0);
	menu_init(&m, l, at, NULL);
	CHECK_EQ(m.sel, 0);
	CHECK_EQ(menu_selected(&m), -1);
	CHECK_EQ(l->e[m.sel].kind, ROMLIST_DIRECTORY);
	romlist_free(l);
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

/* «Densidad»: solo con el CRT de máscara (Opciones y la página del juego);
 * se guarda en menu.cfg y en juegos/. */
static void menu_density(void)
{
	struct menu m;
	nav_setup(&m, 3, 0);
	menu_input(&m, 0, 0);
	m.page = MENU_PAGE_OPTIONS;
	m.opt = MENU_OPT_FILTER;
	m.set.filter = MENU_FILTER_CRT_MASK;
	CHECK_EQ(m.set.density, MENU_DENSITY_NORMAL);
	menu_input(&m, 0, 10);
	menu_input(&m, DOWN, 20);
	CHECK_EQ(m.opt, MENU_OPT_DENSITY);
	menu_input(&m, 0, 30);
	CHECK_EQ(menu_input(&m, RIGHT, 40), MENU_SETTINGS);
	CHECK_EQ(m.set.density, MENU_DENSITY_FINE);
	/* ×, ← y → la cambian igual (y cada una pide guardar). */
	menu_input(&m, 0, 41);
	CHECK_EQ(menu_input(&m, CROSS, 42), MENU_SETTINGS);
	CHECK_EQ(m.set.density, MENU_DENSITY_NORMAL);
	menu_input(&m, 0, 43);
	CHECK_EQ(menu_input(&m, LEFT, 44), MENU_SETTINGS);
	CHECK_EQ(m.set.density, MENU_DENSITY_FINE);
	menu_input(&m, 0, 50);
	menu_input(&m, DOWN, 60);
	CHECK_EQ(m.opt, MENU_OPT_RESOLUTION);
	/* Sin la máscara no hay fila: de Filtro a Resolución. */
	m.set.filter = MENU_FILTER_SMOOTH;
	m.opt = MENU_OPT_FILTER;
	menu_input(&m, 0, 70);
	menu_input(&m, DOWN, 80);
	CHECK_EQ(m.opt, MENU_OPT_RESOLUTION);
	/* menu.cfg: ida y vuelta. */
	char buf[512];
	struct menu_settings back;
	int n = menu_settings_format(&m.set, buf, sizeof buf);
	CHECK(n > 0 && n < (int)sizeof buf);
	menu_settings_default(&back);
	menu_settings_parse(&back, buf, (size_t)n);
	CHECK_EQ(back.density, MENU_DENSITY_FINE);
	CHECK_EQ(back.filter, MENU_FILTER_SMOOTH);
	/* La página del juego: la fila sigue al filtro que vale para él. */
	struct menu_game g;
	menu_game_default(&g);
	CHECK_EQ(g.density, -1);
	m.set.filter = MENU_FILTER_CRT_MASK;
	menu_game_open(&m, &g, "Juego");
	menu_input(&m, 0, 90);
	menu_input(&m, DOWN, 100);
	CHECK_EQ(m.game_sel, MENU_GAME_FILTER);
	menu_input(&m, 0, 110);
	menu_input(&m, DOWN, 120);
	CHECK_EQ(m.game_sel, MENU_GAME_DENSITY);
	CHECK_EQ(menu_game_density(&m.set, &m.game), MENU_DENSITY_FINE);  /* global: la de Opciones */
	menu_input(&m, 0, 130);
	CHECK_EQ(menu_input(&m, RIGHT, 140), MENU_GAME_CHANGED);
	CHECK_EQ(m.game.density, MENU_DENSITY_NORMAL);
	CHECK_EQ(menu_game_density(&m.set, &m.game), MENU_DENSITY_NORMAL);
	/* Global → Normal → Fina → Global con × o →; ← al revés. */
	menu_input(&m, 0, 141);
	CHECK_EQ(menu_input(&m, CROSS, 142), MENU_GAME_CHANGED);
	CHECK_EQ(m.game.density, MENU_DENSITY_FINE);
	menu_input(&m, 0, 143);
	CHECK_EQ(menu_input(&m, LEFT, 144), MENU_GAME_CHANGED);
	CHECK_EQ(m.game.density, MENU_DENSITY_NORMAL);
	n = menu_game_format(&m.game, buf, sizeof buf);
	struct menu_game gb;
	menu_game_default(&gb);
	menu_game_parse(&gb, buf, (size_t)n);
	CHECK_EQ(gb.density, MENU_DENSITY_NORMAL);
	const char *junk = "densidad = media\n";
	menu_game_parse(&gb, junk, strlen(junk));
	CHECK_EQ(gb.density, -1);
	/* Con un filtro sin densidad, la fila desaparece. */
	m.game.filter = MENU_FILTER_SHARP;
	m.game_sel = MENU_GAME_FILTER;
	menu_input(&m, 0, 150);
	menu_input(&m, DOWN, 160);
	CHECK_EQ(m.game_sel, MENU_GAME_OVERSCAN);
	menu_input(&m, 0, 170);
	menu_input(&m, UP, 180);
	CHECK_EQ(m.game_sel, MENU_GAME_FILTER);
}

static void menu_controls(void)
{
	struct menu m;
	nav_setup(&m, 2, 0);
	menu_input(&m, 0, 0);
	menu_controls_open(&m, false);
	CHECK_EQ(m.controls.source[SALSA64_IN_A], SALSA64_DS3_CROSS);
	CHECK_EQ(menu_input(&m, CROSS, 10), MENU_NONE);
	CHECK(m.control_capture);
	menu_input(&m, 0, 11);
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_R2), 12), MENU_NONE);  /* se asigna al soltar */
	CHECK(m.control_capture);
	CHECK_EQ(menu_input(&m, 0, 13), MENU_CONTROLS_CHANGED);
	CHECK(!m.control_capture);
	CHECK_EQ(m.global_controls.source[SALSA64_IN_A], SALSA64_DS3_R2);
	CHECK_EQ(m.global_controls.source2[SALSA64_IN_A], SALSA64_DS3_NONE);

	/* Dos botones juntos (L2 y luego R2 sin soltar L2): los dos. */
	m.control_sel = SALSA64_IN_Z;
	menu_input(&m, CROSS, 13);
	menu_input(&m, 0, 13);
	menu_input(&m, BIT(SALSA64_DS3_L2), 13);
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_L2) | BIT(SALSA64_DS3_R2), 13), MENU_NONE);
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_R2), 13), MENU_NONE);  /* falta soltar R2 */
	CHECK_EQ(menu_input(&m, 0, 13), MENU_CONTROLS_CHANGED);
	CHECK_EQ(m.global_controls.source[SALSA64_IN_Z], SALSA64_DS3_L2);
	CHECK_EQ(m.global_controls.source2[SALSA64_IN_Z], SALSA64_DS3_R2);
	m.control_sel = SALSA64_IN_A;

	menu_input(&m, 0, 13);
	m.control_sel = MENU_CONTROL_RESET;
	CHECK_EQ(menu_input(&m, CROSS, 14), MENU_CONTROLS_CHANGED);
	CHECK_EQ(m.global_controls.source[SALSA64_IN_A], SALSA64_DS3_CROSS);
	menu_input(&m, 0, 15);
	/* La cruceta arriba se captura para D↑ sin mover la selección. */
	m.global_controls.source[SALSA64_IN_D_UP] = SALSA64_DS3_CROSS;
	m.controls = m.global_controls;
	m.control_sel = SALSA64_IN_D_UP;
	menu_input(&m, CROSS, 16);
	CHECK(m.control_capture);
	menu_input(&m, 0, 17);
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_UP), 18), MENU_NONE);
	CHECK_EQ(menu_input(&m, 0, 18), MENU_CONTROLS_CHANGED);
	CHECK_EQ(m.control_sel, SALSA64_IN_D_UP);
	CHECK_EQ(m.global_controls.source[SALSA64_IN_D_UP], SALSA64_DS3_UP);

	/* START cancela sin cambiar; SELECT limpia el destino y no se asigna. */
	menu_input(&m, 0, 19);
	m.global_controls.source[SALSA64_IN_A] = SALSA64_DS3_TRIANGLE;
	m.controls = m.global_controls;
	m.control_sel = SALSA64_IN_A;
	menu_input(&m, CROSS, 20);
	menu_input(&m, 0, 21);
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_START), 22), MENU_NONE);
	CHECK(!m.control_capture);
	CHECK_EQ(m.global_controls.source[SALSA64_IN_A], SALSA64_DS3_TRIANGLE);
	menu_input(&m, 0, 23);
	menu_input(&m, CROSS, 24);
	menu_input(&m, 0, 25);
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_SELECT), 26), MENU_CONTROLS_CHANGED);
	CHECK_EQ(m.global_controls.source[SALSA64_IN_A], SALSA64_DS3_NONE);
	menu_input(&m, 0, 27);
	m.control_sel = MENU_CONTROL_RESET;
	CHECK_EQ(menu_input(&m, CROSS, 28), MENU_CONTROLS_CHANGED);
	CHECK_EQ(m.global_controls.source[SALSA64_IN_A], SALSA64_DS3_CROSS);

	menu_input(&m, 0, 29);

	menu_game_default(&m.game);
	m.game.controls.source[SALSA64_IN_A] = SALSA64_DS3_R2;
	m.game.controls.source2[SALSA64_IN_A] = SALSA64_DS3_NONE;
	menu_controls_open(&m, true);
	CHECK_EQ(m.controls.source[SALSA64_IN_A], SALSA64_DS3_R2);
	CHECK_EQ(m.controls.source[SALSA64_IN_B], m.global_controls.source[SALSA64_IN_B]);
	CHECK_EQ(m.game.controls.source[SALSA64_IN_B], SALSA64_DS3_INHERIT);

	char buf[1024];
	int n = salsa64_padmap_format(&m.global_controls, buf, sizeof buf, 0);
	CHECK(n > 0);
	struct salsa64_padmap back;
	salsa64_padmap_default(&back);
	CHECK_EQ(salsa64_padmap_parse(&back, buf, (size_t)n, NULL, 0), 0);
	CHECK_EQ(back.source[SALSA64_IN_A], SALSA64_DS3_CROSS);
	n = menu_game_format(&m.game, buf, sizeof buf);
	CHECK(n > 0 && n < (int)sizeof buf);
	struct menu_game game_back;
	menu_game_default(&game_back);
	menu_game_parse(&game_back, buf, (size_t)n);
	struct salsa64_padmap effective = back;
	salsa64_padmap_overlay(&effective, &game_back.controls);
	CHECK_EQ(effective.source[SALSA64_IN_A], SALSA64_DS3_R2);
	m.page = MENU_PAGE_OPTIONS;
	menu_controls_open(&m, false);
	menu_input(&m, 0, 15);
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_CIRCLE), 16), MENU_NONE);
	CHECK_EQ(m.page, MENU_PAGE_OPTIONS);
}

static void menu_game_page(void)
{
	struct menu m;
	nav_setup(&m, 3, 0);
	menu_input(&m, 0, 0);
	m.sel = 1;
	/* SELECT sobre una ROM pide sus ajustes. */
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_SELECT), 10), MENU_GAME);
	CHECK_EQ(m.game_rom, 1);
	struct menu_game g;
	menu_game_default(&g);
	CHECK_EQ(g.aspect, -1);
	menu_game_open(&m, &g, "Juego");
	CHECK_EQ(m.page, MENU_PAGE_GAME);
	menu_input(&m, 0, 20);
	/* Aspecto: Global → 4:3 → 16:9 → Global. */
	CHECK_EQ(menu_input(&m, RIGHT, 30), MENU_GAME_CHANGED);
	CHECK_EQ(m.game.aspect, MENU_ASPECT_4_3);
	menu_input(&m, 0, 40);
	CHECK_EQ(menu_input(&m, CROSS, 50), MENU_GAME_CHANGED);
	CHECK_EQ(m.game.aspect, MENU_ASPECT_16_9);
	menu_input(&m, 0, 60);
	CHECK_EQ(menu_input(&m, CROSS, 70), MENU_GAME_CHANGED);
	CHECK_EQ(m.game.aspect, -1);
	menu_input(&m, 0, 80);
	CHECK_EQ(menu_input(&m, LEFT, 90), MENU_GAME_CHANGED);
	CHECK_EQ(m.game.aspect, MENU_ASPECT_16_9);
	/* Filtro y recorte. */
	menu_input(&m, 0, 100);
	menu_input(&m, DOWN, 110);
	CHECK_EQ(m.game_sel, MENU_GAME_FILTER);
	menu_input(&m, 0, 120);
	menu_input(&m, LEFT, 130);
	CHECK_EQ(m.game.filter, MENU_FILTERS - 1);  /* de «global» al último */
	menu_input(&m, 0, 140);
	menu_input(&m, DOWN, 150);
	CHECK_EQ(m.game_sel, MENU_GAME_OVERSCAN);
	menu_input(&m, 0, 160);
	menu_input(&m, RIGHT, 170);
	CHECK_EQ(m.game.overscan, MENU_OVERSCAN_OFF);
	/* Escala interna: Global → Auto → 1× … 5× → Global; izquierda al revés. */
	menu_input(&m, 0, 172);
	menu_input(&m, DOWN, 174);
	CHECK_EQ(m.game_sel, MENU_GAME_SCALE);
	CHECK_EQ(m.game.scale, -1);
	menu_input(&m, 0, 175);
	CHECK_EQ(menu_input(&m, RIGHT, 176), MENU_GAME_CHANGED);
	CHECK_EQ(m.game.scale, 0);
	menu_input(&m, 0, 177);
	menu_input(&m, LEFT, 178);
	CHECK_EQ(m.game.scale, -1);
	menu_input(&m, 0, 179);
	menu_input(&m, LEFT, 179);
	CHECK_EQ(m.game.scale, MENU_SCALE_MAX);
	m.game.scale = 2;
	/* Lo efectivo: lo del juego o, si es global, lo de Opciones. */
	m.set.aspect = MENU_ASPECT_4_3;
	m.set.filter = MENU_FILTER_SHARP;
	m.set.overscan = MENU_OVERSCAN_WIDE;
	m.set.scale = 0;
	int a, f, o, sc;
	menu_game_effective(&m.set, &m.game, &a, &f, &o, &sc);
	CHECK_EQ(a, MENU_ASPECT_16_9);
	CHECK_EQ(f, MENU_FILTERS - 1);
	CHECK_EQ(o, MENU_OVERSCAN_OFF);
	CHECK_EQ(sc, 2);
	m.game.filter = -1;
	menu_game_effective(&m.set, &m.game, NULL, &f, NULL, NULL);
	CHECK_EQ(f, MENU_FILTER_SHARP);
	char sbuf[24];
	CHECK(!strcmp(menu_scale_name(0, MENU_RES_1080P, sbuf, sizeof sbuf), "Auto (4×)"));
	CHECK(!strcmp(menu_scale_name(3, MENU_RES_720P, sbuf, sizeof sbuf), "3×"));
	CHECK(!strcmp(menu_filter_credit(MENU_FILTER_SCAN_SOFT), "Filtro de VIDEOGAMES SCZ"));
	/* Texto del archivo: ida y vuelta, y global ante lo que no entiende. */
	char buf[512];
	int n = menu_game_format(&m.game, buf, sizeof buf);
	CHECK(n > 0 && n < (int)sizeof buf);
	struct menu_game back;
	menu_game_default(&back);
	menu_game_parse(&back, buf, (size_t)n);
	CHECK_EQ(back.aspect, MENU_ASPECT_16_9);
	CHECK_EQ(back.filter, -1);
	CHECK_EQ(back.overscan, MENU_OVERSCAN_OFF);
	CHECK_EQ(back.scale, 2);
	CHECK_EQ(back.interp, -1);
	m.game.scale = 0;
	m.game.interp = MENU_INTERP_GEOMETRY;
	n = menu_game_format(&m.game, buf, sizeof buf);
	menu_game_parse(&back, buf, (size_t)n);
	CHECK_EQ(back.scale, 0);
	CHECK_EQ(back.interp, MENU_INTERP_GEOMETRY);
	CHECK_EQ(menu_game_interp(&m.set, &back), MENU_INTERP_GEOMETRY);
	back.interp = -1;
	CHECK_EQ(menu_game_interp(&m.set, &back), MENU_INTERP_OFF);  /* la de Opciones */
	/* Accesorio: Controller Pak por omisión, «ninguno» se guarda y se lee. */
	CHECK_EQ(back.accessory, MENU_ACC_PAK);
	m.game.accessory = MENU_ACC_NONE;
	n = menu_game_format(&m.game, buf, sizeof buf);
	CHECK(strstr(buf, "accesorio = ninguno") != NULL);
	menu_game_parse(&back, buf, (size_t)n);
	CHECK_EQ(back.accessory, MENU_ACC_NONE);
	m.game.accessory = MENU_ACC_PAK;
	n = menu_game_format(&m.game, buf, sizeof buf);
	CHECK(strstr(buf, "accesorio = controller-pak") != NULL);
	menu_game_parse(&back, buf, (size_t)n);
	CHECK_EQ(back.accessory, MENU_ACC_PAK);
	const char *junk = "aspecto = 5:4\nfiltro = raro\nrecorte = x\nescala = 9\n";
	menu_game_parse(&back, junk, strlen(junk));
	CHECK_EQ(back.aspect, -1);
	CHECK_EQ(back.filter, -1);
	CHECK_EQ(back.overscan, -1);
	CHECK_EQ(back.scale, -1);
	/* ○ vuelve a la lista. */
	menu_input(&m, 0, 180);
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_CIRCLE), 190), MENU_NONE);
	CHECK_EQ(m.page, MENU_PAGE_GAMES);
	/* menu.cfg: filtro e idioma van y vienen. */
	struct menu_settings st;
	menu_settings_default(&st);
	st.filter = MENU_FILTER_SCAN_SOFT;
	st.lang = MENU_LANG_DE;
	st.interp = MENU_INTERP_GEOMETRY;
	n = menu_settings_format(&st, buf, sizeof buf);
	CHECK(n > 0 && n < (int)sizeof buf);
	struct menu_settings st2;
	menu_settings_default(&st2);
	CHECK_EQ(st2.interp, MENU_INTERP_OFF);
	menu_settings_parse(&st2, buf, (size_t)n);
	CHECK_EQ(st2.filter, MENU_FILTER_SCAN_SOFT);
	CHECK_EQ(st2.lang, MENU_LANG_DE);
	CHECK_EQ(st2.interp, MENU_INTERP_GEOMETRY);
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
	/* Aspecto: X o los lados alternan 4:3 / 16:9 y piden guardar. */
	menu_input(&m, 0, 69);
	menu_input(&m, DOWN, 69);
	CHECK_EQ(m.opt, MENU_OPT_ASPECT);
	CHECK_EQ(m.set.aspect, MENU_ASPECT_4_3);
	menu_input(&m, 0, 69);
	CHECK_EQ(menu_input(&m, CROSS, 69), MENU_SETTINGS);
	CHECK_EQ(m.set.aspect, MENU_ASPECT_16_9);
	menu_input(&m, 0, 69);
	CHECK_EQ(menu_input(&m, LEFT, 69), MENU_SETTINGS);
	CHECK_EQ(m.set.aspect, MENU_ASPECT_4_3);
	/* Filtro: derecha avanza (Suave → Nítida → Líneas suaves → Líneas fuertes → los CRT → Suave), izquierda
	 * vuelve. */
	menu_input(&m, 0, 69);
	menu_input(&m, DOWN, 69);
	CHECK_EQ(m.opt, MENU_OPT_FILTER);
	CHECK_EQ(m.set.filter, MENU_FILTER_SMOOTH);
	for (int want = 1; want < MENU_FILTERS; want++) {
		menu_input(&m, 0, 69);
		CHECK_EQ(menu_input(&m, RIGHT, 69), MENU_SETTINGS);
		CHECK_EQ(m.set.filter, want);
	}
	menu_input(&m, 0, 69);
	menu_input(&m, CROSS, 69);
	CHECK_EQ(m.set.filter, MENU_FILTER_SMOOTH);
	menu_input(&m, 0, 69);
	CHECK_EQ(menu_input(&m, LEFT, 69), MENU_SETTINGS);
	CHECK_EQ(m.set.filter, MENU_FILTER_CRT_TRINITRON);
	m.set.filter = MENU_FILTER_SMOOTH;
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
	/* Sin descargas.cfg, la opción de controles queda entre la lista y volver. */
	CHECK(!m.dl_enabled);
	menu_input(&m, UP, 79);
	CHECK_EQ(m.opt, MENU_OPT_CONTROLS);
	menu_input(&m, 0, 79);
	menu_input(&m, UP, 79);
	CHECK_EQ(m.opt, MENU_OPT_REFRESH);
	menu_input(&m, 0, 79);
	menu_input(&m, UP, 79);
	CHECK_EQ(m.opt, MENU_OPT_ROM_ORDER);
	menu_input(&m, 0, 79);
	menu_input(&m, UP, 79);
	CHECK_EQ(m.opt, MENU_OPT_LANG);
	menu_input(&m, 0, 79);
	menu_input(&m, DOWN, 79);
	CHECK_EQ(m.opt, MENU_OPT_ROM_ORDER);
	menu_input(&m, 0, 79);
	menu_input(&m, DOWN, 79);
	CHECK_EQ(m.opt, MENU_OPT_REFRESH);
	menu_input(&m, 0, 79);
	CHECK_EQ(menu_input(&m, CROSS, 79), MENU_REFRESH);
	menu_input(&m, 0, 79);
	menu_input(&m, DOWN, 79);
	CHECK_EQ(m.opt, MENU_OPT_CONTROLS);
	CHECK_EQ(menu_input(&m, CROSS, 79), MENU_CONTROLS);
	menu_input(&m, 0, 79);
	menu_input(&m, DOWN, 79);
	CHECK_EQ(m.opt, MENU_OPT_BACK);
	menu_input(&m, 0, 79);
	m.dl_enabled = true;             /* encendidas (main.c, dl_enabled) */
	menu_input(&m, UP, 79);          /* encima de «Volver» está «Controles» */
	CHECK_EQ(m.opt, MENU_OPT_CONTROLS);
	menu_input(&m, 0, 79);
	menu_input(&m, UP, 79);
	CHECK_EQ(m.opt, MENU_OPT_REFRESH);
	menu_input(&m, 0, 79);
	menu_input(&m, UP, 79);
	CHECK_EQ(m.opt, MENU_OPT_DL_CHEATS);
	menu_input(&m, 0, 79);
	menu_input(&m, UP, 79);
	CHECK_EQ(m.opt, MENU_OPT_DL_COVERS);
	menu_input(&m, 0, 79);
	menu_input(&m, UP, 79);
	CHECK_EQ(m.opt, MENU_OPT_ROM_ORDER);
	menu_input(&m, 0, 79);
	menu_input(&m, UP, 79);
	CHECK_EQ(m.opt, MENU_OPT_LANG);
	menu_input(&m, 0, 79);
	menu_input(&m, UP, 79);
	CHECK_EQ(m.opt, MENU_OPT_CONFIRM);
	menu_input(&m, 0, 79);
	menu_input(&m, UP, 79);
	CHECK_EQ(m.opt, MENU_OPT_FPS);
	menu_input(&m, 0, 79);
	menu_input(&m, UP, 79);
	CHECK_EQ(m.opt, MENU_OPT_INTERP);
	menu_input(&m, 0, 79);
	menu_input(&m, UP, 79);
	CHECK_EQ(m.opt, MENU_OPT_SCALE);
	menu_input(&m, 0, 79);
	menu_input(&m, DOWN, 79);
	CHECK_EQ(m.opt, MENU_OPT_INTERP);   /* sin cambios pendientes se salta «Aplicar ahora» */
	/* Interpolación: apagada por omisión; alterna y pide guardar. */
	CHECK_EQ(m.set.interp, MENU_INTERP_OFF);
	menu_input(&m, 0, 79);
	CHECK_EQ(menu_input(&m, CROSS, 79), MENU_SETTINGS);
	CHECK_EQ(m.set.interp, MENU_INTERP_GEOMETRY);
	menu_input(&m, 0, 79);
	CHECK_EQ(menu_input(&m, LEFT, 79), MENU_SETTINGS);
	CHECK_EQ(m.set.interp, MENU_INTERP_OFF);
	menu_input(&m, 0, 79);
	menu_input(&m, DOWN, 79);
	CHECK_EQ(m.opt, MENU_OPT_FPS);
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
	CHECK_EQ(m.opt, MENU_OPT_CONFIRM);
	/* Confirmar al salir: sí por omisión; alterna y pide guardar. */
	CHECK(m.set.confirm);
	menu_input(&m, 0, 79);
	CHECK_EQ(menu_input(&m, CROSS, 79), MENU_SETTINGS);
	CHECK(!m.set.confirm);
	menu_input(&m, 0, 79);
	CHECK_EQ(menu_input(&m, RIGHT, 79), MENU_SETTINGS);
	CHECK(m.set.confirm);
	/* Idioma: derecha avanza con vuelta; izquierda al revés. */
	menu_input(&m, 0, 79);
	menu_input(&m, DOWN, 79);
	CHECK_EQ(m.opt, MENU_OPT_LANG);
	CHECK_EQ(m.set.lang, MENU_LANG_ES);
	menu_input(&m, 0, 79);
	CHECK_EQ(menu_input(&m, RIGHT, 79), MENU_SETTINGS);
	CHECK_EQ(m.set.lang, MENU_LANG_EN);
	menu_input(&m, 0, 79);
	CHECK_EQ(menu_input(&m, LEFT, 79), MENU_SETTINGS);
	menu_input(&m, 0, 79);
	CHECK_EQ(menu_input(&m, LEFT, 79), MENU_SETTINGS);
	CHECK_EQ(m.set.lang, MENU_LANG_PT);
	m.set.lang = MENU_LANG_ES;
	/* Orden y descargas: cambia y guarda la forma de ordenar; × pide bajar. */
	menu_input(&m, 0, 79);
	menu_input(&m, DOWN, 79);
	CHECK_EQ(m.opt, MENU_OPT_ROM_ORDER);
	CHECK_EQ(menu_input(&m, RIGHT, 79), MENU_SETTINGS);
	CHECK_EQ(m.set.rom_order, MENU_ROM_ORDER_FILENAME);
	menu_input(&m, 0, 79);
	menu_input(&m, DOWN, 79);
	CHECK_EQ(m.opt, MENU_OPT_DL_COVERS);
	menu_input(&m, 0, 79);
	CHECK_EQ(menu_input(&m, RIGHT, 79), MENU_NONE);
	menu_input(&m, 0, 79);
	CHECK_EQ(menu_input(&m, CROSS, 79), MENU_DOWNLOAD);
	CHECK_EQ(m.dl_what, 0);
	menu_input(&m, 0, 79);
	menu_input(&m, DOWN, 79);
	CHECK_EQ(m.opt, MENU_OPT_DL_CHEATS);
	menu_input(&m, 0, 79);
	CHECK_EQ(menu_input(&m, CROSS, 79), MENU_DOWNLOAD);
	CHECK_EQ(m.dl_what, 1);
	menu_input(&m, 0, 79);
	menu_input(&m, DOWN, 79);
	CHECK_EQ(m.opt, MENU_OPT_REFRESH);
	menu_input(&m, 0, 79);
	menu_input(&m, DOWN, 79);
	CHECK_EQ(m.opt, MENU_OPT_CONTROLS);
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
	CHECK(s.confirm);  /* sin la línea: pregunta antes de salir */
	static const char order[] = "orden = archivo\n";
	menu_settings_parse(&s, order, sizeof order - 1);
	CHECK_EQ(s.rom_order, MENU_ROM_ORDER_FILENAME);
	CHECK_EQ(s.aspect, MENU_ASPECT_4_3);
	static const char wide[] = "aspecto = 16:9\n";
	menu_settings_parse(&s, wide, sizeof wide - 1);
	CHECK_EQ(s.aspect, MENU_ASPECT_16_9);
	static const char junk[] = "aspecto = raro\n";
	menu_settings_parse(&s, junk, sizeof junk - 1);
	CHECK_EQ(s.aspect, MENU_ASPECT_16_9);  /* un valor que no entiende no lo cambia */
	static const char direct[] = "confirmar = no\n";
	menu_settings_parse(&s, direct, sizeof direct - 1);
	CHECK(!s.confirm);
	char text[512];
	s.waves = false;
	int n = menu_settings_format(&s, text, sizeof text);
	CHECK(n > 0 && (size_t)n < sizeof text);
	struct menu_settings t;
	menu_settings_default(&t);
	menu_settings_parse(&t, text, (size_t)n);
	CHECK(!t.waves);
	CHECK(t.fps);
	CHECK(!t.confirm);
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
static const uint8_t *img_px[RSXKIT_UI_IMAGES];  /* carátulas de la vista previa */
static uint32_t img_w[RSXKIT_UI_IMAGES], img_h[RSXKIT_UI_IMAGES];

static void raster(uint8_t *rgb, int w, int h, float lw, float lh, uint32_t n)
{
	float sx = w / lw, sy = h / lh;
	const uint32_t *gfirst;
	uint32_t ng = rsxkit_ui_groups(&gfirst), g = 0;
	for (uint32_t t = 0; t + 2 < n; t += 3) {
		while (g + 1 < ng && gfirst[g + 1] <= t)
			g++;
		uint32_t gi = ng ? rsxkit_ui_group_image(g) : 0;
		const uint8_t *im = gi ? img_px[gi - 1] : NULL;
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
				float fs = s - is, ft = tt - it, ta = 0, tc[3] = { 1, 1, 1 };
				if (im) {
					uint32_t iw = img_w[gi - 1], ih = img_h[gi - 1];
					ta = 0;
					tc[0] = tc[1] = tc[2] = 0;
					for (int k = 0; k < 4; k++) {
						int ax = is + (k & 1), ay = it + (k >> 1);
						ax = ax < 0 ? 0 : ax >= (int)iw ? (int)iw - 1 : ax;
						ay = ay < 0 ? 0 : ay >= (int)ih ? (int)ih - 1 : ay;
						float wgt = ((k & 1) ? fs : 1 - fs) * ((k >> 1) ? ft : 1 - ft);
						const uint8_t *tp = im + 4 * ((size_t)ay * iw + ax);
						ta += wgt * tp[3] / 255.0f;
						for (int c = 0; c < 3; c++)
							tc[c] += wgt * tp[c] / 255.0f;
					}
				} else
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
					o[c] = (uint8_t)(col * tc[c] * a + o[c] * (1 - a) + 0.5f);
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

static struct romlist *frame_list;

static void frame_list_setup(void)
{
	if (!frame_list) frame_list = romlist_create();
	memset(frame_list, 0, sizeof *frame_list);
	static const char *titles[] = { "THE LEGEND OF ZELDA", "SUPER MARIO 64", "GOLDENEYE", "MARIO KART 64",
					"Perfect Dark", "CONKER BFD", "STARFOX64", "" };
	static const char *codes[] = { "NZLE", "NSME", "NGEE", "NKTE", "NPDE", "NFUE", "NFXE", "NXXE" };
	static const char *names[] = { "oot.z64", "sm64.z64", "goldeneye.z64", "mk64.z64", "perfectdark.z64",
				       "conker.z64", "sf64.v64", "homebrew-sin-título.n64" };
	for (unsigned i = 0; i < 8; i++) {
		uint8_t h[0x40];
		struct rom_entry *e = &frame_list->e[frame_list->count++];
		memset(e, 0, sizeof *e);
		e->name = names[i];
		e->source = i == 3 ? 1 : 0;
		e->size = (i + 1) << 22;
		make_header(h, titles[i], codes[i], 0);
		romlist_parse_header(h, NULL, 0, e);
	}
	romlist_sort(frame_list, ROMLIST_ORDER_TITLE);
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
	menu_init(&m, frame_list, 3, NULL);
	snprintf(m.last, sizeof m.last, "%s", romlist_name(&frame_list->e[3]));
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
	struct romlist *empty = romlist_create();
	menu_init(&m, empty, 0, NULL);
	m.dirs[m.ndirs++] = "/dev_hdd0/salsa64";
	frames(&m, 0, 500000, 1280);
	CHECK_EQ(rsxkit_ui_dropped(), dropped0);
	rsxkit_ui_batch(NULL, 0, 0, 0);
	romlist_free(empty);
}

/* Qué cambios de Opciones reescriben menu.cfg: todo lo que va en él (la
 * densidad no se guardaba: el juego abría con la normal), nada de lo de
 * overscan.cfg y video.cfg. */
static void menu_cfg_differs(void)
{
	struct menu_settings a, b;
	menu_settings_default(&a);
	b = a;
	CHECK(!menu_settings_cfg_differs(&a, &b));
	b.overscan = MENU_OVERSCAN_WIDE;
	b.resolution = MENU_RES_1080P;
	b.scale = 5;
	CHECK(!menu_settings_cfg_differs(&a, &b));
	for (int k = 0; k < 7; k++) {
		b = a;
		switch (k) {
		case 0: b.waves = !a.waves; break;
		case 1: b.fps = !a.fps; break;
		case 2: b.confirm = !a.confirm; break;
		case 3: b.aspect = MENU_ASPECT_16_9; break;
		case 4: b.filter = MENU_FILTER_CRT_TRINITRON; break;
		case 5: b.density = MENU_DENSITY_FINE; break;
		default: b.lang = 1; break;
		}
		CHECK(menu_settings_cfg_differs(&a, &b));
	}
}

/* El menú quieto `ms` milisegundos desde `t`, dibujando cada cuadro. */
static uint64_t settle_draw(struct menu *m, uint64_t t, uint32_t ms)
{
	for (uint64_t end = t + ms * 1000ull; t <= end; t += 16683) {
		menu_input(m, 0, t);
		rsxkit_ui_batch(verts, sizeof verts / sizeof verts[0], 1280, 720);
		menu_frame(m, 1280, 720, t);
	}
	return t;
}

/* Opciones: la ayuda de cada fila, de cada filtro y de cada densidad, con
 * el panel lleno. Cada texto termina en dos NULL; si a uno le falta, el
 * bucle del panel sigue leyendo lo que haya después en la memoria (con
 * ASan, desborde de global). En la PS3, «CRT Trinitron» en Opciones colgaba
 * el menú: leía como texto los arreglos de al lado hasta una dirección
 * mala. */
static void menu_options_help(void)
{
	frame_list_setup();
	uint32_t dropped0 = rsxkit_ui_dropped();
	struct menu m;
	menu_init(&m, frame_list, 3, NULL);
	m.dl_enabled = true;
	uint64_t t = settle_draw(&m, 0, 1500);
	menu_input(&m, BIT(SALSA64_DS3_TRIANGLE), t);
	t = settle_draw(&m, t + 16683, 800);
	CHECK_EQ(m.page, MENU_PAGE_OPTIONS);
	uint32_t rows = 0, filters = 0, densities = 0;
	for (int i = 0; i < 2 * MENU_OPTS; i++) {
		rows |= 1u << m.opt;
		if (m.opt == MENU_OPT_FILTER) {
			/* Todos los filtros (la vuelta entera) y luego la máscara,
			 * para que aparezca la fila de la densidad. */
			for (int k = 0; k < MENU_FILTERS || m.set.filter != MENU_FILTER_CRT_MASK; k++) {
				CHECK_EQ(menu_input(&m, RIGHT, t), MENU_SETTINGS);
				t = settle_draw(&m, t + 16683, 300);
				filters |= 1u << m.set.filter;
			}
		}
		if (m.opt == MENU_OPT_DENSITY)
			for (int k = 0; k < MENU_DENSITIES; k++) {
				CHECK_EQ(menu_input(&m, RIGHT, t), MENU_SETTINGS);
				t = settle_draw(&m, t + 16683, 300);
				densities |= 1u << m.set.density;
			}
		if (m.opt == MENU_OPT_SCALE) {  /* con un cambio pendiente aparece «Aplicar ahora» */
			CHECK_EQ(menu_input(&m, RIGHT, t), MENU_SETTINGS);
			t = settle_draw(&m, t + 16683, 300);
		}
		if (m.opt == MENU_OPT_BACK)
			break;
		menu_input(&m, DOWN, t);
		t = settle_draw(&m, t + 16683, 300);
	}
	CHECK_EQ(rows, (1u << MENU_OPTS) - 1);
	CHECK_EQ(filters, (1u << MENU_FILTERS) - 1);
	CHECK_EQ(densities, (1u << MENU_DENSITIES) - 1);
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

static void check_label(const char *describe, const char *commit, const char *want)
{
	char buf[96];
	version_label(describe, commit, buf, sizeof buf);
	t_checks++;
	if (strcmp(buf, want))
		t_fail(__FILE__, __LINE__, "version_label(«%s», «%s») = «%s», se esperaba «%s»", describe, commit, buf,
		       want);
}

/* La versión corta de cada forma de git describe, el paquete de prueba y la
 * primera línea del log (aquí sin los -D del Makefile: la del publicado y
 * «desconocida»). */
static void version_texts(void)
{
	check_label("v0.2.5", "fb12aeb", "v0.2.5 · fb12aeb");
	check_label("v0.2.5", "", "v0.2.5");
	check_label("v0.2.5-dirty", "fb12aeb", "v0.2.5 · fb12aeb-dirty");
	check_label("v0.2.5-3-gabc1234", "abc1234", "v0.2.5+3 · abc1234");
	check_label("v0.2.5-12-gabc1234-dirty", "abc1234", "v0.2.5+12 · abc1234-dirty");
	check_label("v0.3.0-rc1-2-g0123abcd", "0123abcd", "v0.3.0-rc1+2 · 0123abcd");
	check_label("abc1234", "abc1234", "abc1234");
	check_label("abc1234-dirty", "abc1234", "abc1234-dirty");
	check_label("desconocida", "", "versión desconocida");
	check_label("", "", "versión desconocida");
	char buf[160];
	CHECK(!version_build(VERSION_TITLE_RELEASE, VERSION_APPID_RELEASE, buf, sizeof buf));
	CHECK(version_build("Salsa64 gráficos 3", "SLSA00114", buf, sizeof buf));
	CHECK(!strcmp(buf, "Salsa64 gráficos 3 · SLSA00114"));
	CHECK(version_build(VERSION_TITLE_RELEASE, "SLSA00099", buf, sizeof buf));
	CHECK(!strcmp(buf, "Salsa64 · SLSA00099"));
	version_banner(buf, sizeof buf);
	CHECK(!strcmp(buf, "Salsa64 versión desconocida (Salsa64, SLSA00064)"));
	char small[12];  /* se corta sin pasarse */
	version_label("v0.2.5-3-gabc1234", "abc1234", small, sizeof small);
	CHECK_EQ(strlen(small), sizeof small - 1);
}

/* La versión y el paquete van arriba a la derecha, entre el contador y el
 * panel y sin pasar sobre la columna, lejos del pie: en los 6 idiomas, con
 * tele 16:9 y 4:3 (el espacio lógico es el mismo a 720p y a 1080p) y con un
 * título que no cabe (se recorta). Se dibuja el mismo cuadro sin ellos y con
 * ellos, y se mira dónde cambió. */
static void menu_version_place(void)
{
	static uint8_t without[720 * 1280 * 3], with[720 * 1280 * 3];
	static const char *const builds[] = {
		NULL, "Salsa64 gráficos 3 · SLSA00114",
		"Salsa64 prueba con un título larguísimo que no cabe en la esquina · SLSA00999",
	};
	rsxkit_ui_atlas(&atlas[0][0], RSXKIT_UI_ATLAS_W * 4);
	frame_list_setup();
	uint32_t dropped0 = rsxkit_ui_dropped();
	for (int lang = 0; lang < MENU_LANGS; lang++)
		for (int wide = 0; wide < 2; wide++)
			for (int b = 0; b < 3; b++) {
				float lw = wide ? 1280.0f : 960.0f;
				int w = (int)lw;
				i18n_set(lang);
				struct menu m;
				menu_init(&m, frame_list, 4, NULL);
				m.set.lang = lang;
				m.set.waves = false;  /* fondo quieto: los dos cuadros, iguales salvo la versión */
				frames(&m, 1000000, 2500000, lw);
				memset(without, 0, sizeof without);
				raster(without, w, 720, lw, 720, frames(&m, 2600000, 2600000, lw));
				m.version = b == 2 ? "versión desconocida" : "v0.2.5+12 · abc1234-dirty";
				m.build = builds[b];
				memset(with, 0, sizeof with);
				raster(with, w, 720, lw, 720, frames(&m, 2616683, 2616683, lw));
				int x0 = w, y0 = 720, x1 = -1, y1 = -1;
				for (int y = 0; y < 720; y++)
					for (int x = 0; x < w; x++)
						if (memcmp(with + 3 * (y * w + x), without + 3 * (y * w + x), 3)) {
							x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1;
							y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
						}
				/* El contador termina a 0,055 × 720 + 10 + 1,75 × 13 ≈ 72
				 * (más la sombra); el panel empieza a 0,17 × 720 ≈ 122 y en
				 * 0,555 del ancho (la columna queda a la izquierda); el pie,
				 * a 0,815 × 720 ≈ 587. */
				CHECK(x1 >= 0);
				CHECK(y0 >= 75);
				CHECK(y1 < (b ? 120 : 101));
				CHECK(x0 >= (int)(lw * 0.555f));
				CHECK(x1 <= (int)(lw * 0.94f) + 3);
				CHECK_EQ(rsxkit_ui_dropped(), dropped0);
			}
	i18n_set(MENU_LANG_ES);
	rsxkit_ui_batch(NULL, 0, 0, 0);
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
	menu_init(&m, frame_list, 4, NULL);
	m.out_w = 1280, m.out_h = 720;
	m.res_run = MENU_RES_1080P;  /* pidió 1080p y quedó 720p: sale la pista */
	m.version = "v0.2.5+3 · abc1234";
	m.build = "Salsa64 gráficos 3 · SLSA00114";
	snprintf(m.last, sizeof m.last, "%s", romlist_name(&frame_list->e[4]));
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
	menu_init(&m, frame_list, frame_list->count, NULL);
	m.set.waves = false;
	menu_set_status(&m, "No se pudo leer /dev_usb000/salsa64/mk64.z64");
	n = frames(&m, 1000000, 2000000, 960);
	raster(rgb, 960, 720, 960, 720, n);
	snprintf(path, sizeof path, "%s-43.ppm", out);
	write_ppm(path, rgb, 960, 720);
	/* Ajustes por juego, filtro e idiomas. */
	{
		struct menu_game g;
		menu_game_default(&g);
		g.aspect = MENU_ASPECT_16_9;
		g.overscan = MENU_OVERSCAN_OFF;
		for (int lang = 0; lang <= 5; lang += 1) {
			if (lang == 2 || lang == 4)
				continue;  /* en, de y pt bastan para el aspecto; fr e it se ven en las pruebas de ancho */
			i18n_set(lang);
			menu_init(&m, frame_list, 4, NULL);
			m.set.lang = lang;
			m.set.filter = MENU_FILTER_SCAN_SOFT;
			m.version = "v0.2.5+3 · abc1234";
			m.build = "Salsa64 gráficos 3 · SLSA00114";
			snprintf(m.last, sizeof m.last, "%s", romlist_name(&frame_list->e[4]));
			n = frames(&m, 1000000, 13000000, 960);
			raster(rgb, 960, 720, 960, 720, n);
			snprintf(path, sizeof path, "%s-juegos43-l%d.ppm", out, lang);
			write_ppm(path, rgb, 960, 720);
			menu_game_open(&m, &g, "Perfect Dark");
			m.game_sel = 1;
			n = frames(&m, 13000000, 14000000, 1280);
			raster(rgb, 1280, 720, 1280, 720, n);
			snprintf(path, sizeof path, "%s-ajustes-l%d.ppm", out, lang);
			write_ppm(path, rgb, 1280, 720);
			n = frames(&m, 14000000, 14100000, 960);
			raster(rgb, 960, 720, 960, 720, n);
			snprintf(path, sizeof path, "%s-ajustes43-l%d.ppm", out, lang);
			write_ppm(path, rgb, 960, 720);
			menu_init(&m, frame_list, 4, NULL);
			m.set.lang = lang;
			m.set.filter = MENU_FILTER_SCAN_STRONG;
			m.page = MENU_PAGE_OPTIONS;
			m.opt = MENU_OPT_FILTER;
			n = frames(&m, 1000000, 13000000, 1280);
			raster(rgb, 1280, 720, 1280, 720, n);
			snprintf(path, sizeof path, "%s-filtro-l%d.ppm", out, lang);
			write_ppm(path, rgb, 1280, 720);
			m.dl_enabled = true;  /* experimentales: solo con descargas.cfg */
			m.opt = MENU_OPT_DL_COVERS;
			n = frames(&m, 13000000, 14000000, 960);
			raster(rgb, 960, 720, 960, 720, n);
			snprintf(path, sizeof path, "%s-descargar43-l%d.ppm", out, lang);
			write_ppm(path, rgb, 960, 720);
			menu_set_status(&m, "%s: %u nuevas, %u ya estaban, %u sin coincidencia", tr("Carátulas"), 12u, 40u, 3u);
			n = frames(&m, 14000000, 14100000, 1280);
			raster(rgb, 1280, 720, 1280, 720, n);
			snprintf(path, sizeof path, "%s-estado-l%d.ppm", out, lang);
			write_ppm(path, rgb, 1280, 720);
		}
		i18n_set(0);
	}
	/* La página de trucos. */
	{
		static struct cheat_set cs;
		static const char text[] =
			"cheat0_desc = \"Vidas infinitas\"\ncheat0_code = \"8107A6C4 0063\"\ncheat0_enable = true\n"
			"cheat1_desc = \"Salud y munición al máximo (todas las armas)\"\ncheat1_code = \"8100FFFF 0001+8100FFFE 0002\"\n"
			"cheat2_desc = \"Saltar de nivel\"\ncheat2_code = \"89001000 0001\"\n"
			"cheat3_desc = \"Tiempo infinito\"\ncheat3_code = \"81001000 0001\"\n";
		cheats_parse(&cs, text, sizeof text - 1);
		menu_init(&m, frame_list, 4, NULL);
		menu_cheats_open(&m, &cs, romlist_name(&frame_list->e[4]));
		m.cht_title = "Perfect Dark";
		m.cht_sel = 1;
		n = frames(&m, 1000000, 2000000, 1280);
		raster(rgb, 1280, 720, 1280, 720, n);
		snprintf(path, sizeof path, "%s-trucos.ppm", out);
		write_ppm(path, rgb, 1280, 720);
		n = frames(&m, 2000000, 2100000, 960);
		raster(rgb, 960, 720, 960, 720, n);
		snprintf(path, sizeof path, "%s-trucos43.ppm", out);
		write_ppm(path, rgb, 960, 720);
	}
	/* Con carátula: una de 320×448 hecha aquí (degradado y un marco). */
	{
		static uint8_t cov[512 * 448 * 4];
		for (int y = 0; y < 448; y++)
			for (int x = 0; x < 320; x++) {
				uint8_t *p = cov + 4 * (y * 320 + x);
				bool edge = x < 8 || y < 8 || x >= 312 || y >= 440;
				p[0] = edge ? 240 : (uint8_t)(40 + y / 3);
				p[1] = edge ? 200 : (uint8_t)(60 + x / 3);
				p[2] = edge ? 40 : 160;
				p[3] = 255;
			}
		img_px[0] = cov, img_w[0] = 320, img_h[0] = 448;
		rsxkit_ui_image_size(0, 320, 448);
		menu_init(&m, frame_list, 4, NULL);
		m.cover_rom = 4, m.cover_w = 320, m.cover_h = 448;
		snprintf(m.last, sizeof m.last, "%s", romlist_name(&frame_list->e[4]));
		n = frames(&m, 1000000, 13000000, 1280);
		raster(rgb, 1280, 720, 1280, 720, n);
		snprintf(path, sizeof path, "%s-caratula.ppm", out);
		write_ppm(path, rgb, 1280, 720);
		n = frames(&m, 13000000, 13100000, 960);
		raster(rgb, 960, 720, 960, 720, n);
		snprintf(path, sizeof path, "%s-caratula43.ppm", out);
		write_ppm(path, rgb, 960, 720);
		/* Y una apaisada, como las de RetroArch (512×372). */
		for (int y = 0; y < 372; y++)
			for (int x = 0; x < 512; x++) {
				uint8_t *p = cov + 4 * (y * 512 + x);
				p[0] = (uint8_t)(x / 2), p[1] = (uint8_t)(y / 2), p[2] = 90, p[3] = 255;
			}
		img_w[0] = 512, img_h[0] = 372;
		rsxkit_ui_image_size(0, 512, 372);
		m.cover_w = 512, m.cover_h = 372;
		n = frames(&m, 13100000, 13200000, 1280);
		raster(rgb, 1280, 720, 1280, 720, n);
		snprintf(path, sizeof path, "%s-caratula-apaisada.ppm", out);
		write_ppm(path, rgb, 1280, 720);
		n = frames(&m, 13200000, 13300000, 960);
		raster(rgb, 960, 720, 960, 720, n);
		snprintf(path, sizeof path, "%s-caratula-apaisada43.ppm", out);
		write_ppm(path, rgb, 960, 720);
		img_px[0] = NULL;
	}
	rsxkit_ui_batch(NULL, 0, 0, 0);
}


static void menu_cover_state(void)
{
	frame_list_setup();
	struct menu m;
	menu_init(&m, frame_list, 2, NULL);
	CHECK_EQ(m.cover_rom, -1);
	frames(&m, 1000000, 2000000, 1280);
	CHECK(menu_settled(&m));
	/* Sin carátula cargada no se muestra; con la de otra ROM tampoco. */
	CHECK(m.cover_t == 0.0f);
	m.cover_rom = 3, m.cover_w = 320, m.cover_h = 448;
	frames(&m, 2000000, 3000000, 1280);
	CHECK(m.cover_t == 0.0f);
	/* La de la ROM elegida aparece. */
	m.cover_rom = 2;
	frames(&m, 3000000, 4000000, 1280);
	CHECK(m.cover_t > 0.99f);
	/* Moviendo la selección ya no está quieta, y la carátula se va. */
	menu_input(&m, 0, 4000000);
	menu_input(&m, BIT(SALSA64_DS3_DOWN), 4010000);
	frames(&m, 4010000, 4020000, 1280);
	CHECK(!menu_settled(&m));
	frames(&m, 4020000, 5000000, 1280);
	CHECK(menu_settled(&m));
	CHECK(m.cover_t < 0.01f);
	rsxkit_ui_batch(NULL, 0, 0, 0);
}

static void menu_cheats_page(void)
{
	struct menu m;
	nav_setup(&m, 3, 1);
	menu_input(&m, 0, 0);
	/* □ sobre una ROM pide los trucos; sobre «Opciones» no. */
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_SQUARE), 10), MENU_CHEATS);
	CHECK_EQ(m.page, MENU_PAGE_GAMES);  /* la página la abre main.c cuando los tiene */
	menu_input(&m, 0, 20);
	m.sel = 3;
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_SQUARE), 30), MENU_NONE);
	m.sel = 1;
	menu_input(&m, 0, 40);

	static struct cheat_set cs;
	static const char text[] = "cheat0_desc = \"A\"\ncheat0_code = \"80000000 0001\"\n"
				   "cheat1_desc = \"B\"\ncheat1_code = \"80000001 0001\"\n"
				   "cheat2_desc = \"C\"\ncheat2_code = \"80000002 0001\"\n";
	cheats_parse(&cs, text, sizeof text - 1);
	menu_set_status(&m, "No hay trucos");
	menu_cheats_open(&m, &cs, "ROM 01");
	CHECK_EQ(m.page, MENU_PAGE_CHEATS);
	CHECK_EQ(m.cht_sel, 0);
	CHECK(!m.status[0]);
	CHECK_EQ(menu_cheat_rows(&m), 4);  /* tres trucos y «Volver» */
	CHECK_EQ(menu_selected(&m), -1);   /* la ROM no cuenta como elegida en esta página */
	/* × cambia el truco elegido y pide guardar; ◂▸ no cambian nada. */
	menu_input(&m, 0, 50);
	CHECK_EQ(menu_input(&m, CROSS, 60), MENU_CHEATS_CHANGED);
	CHECK(cs.c[0].on);
	menu_input(&m, 0, 70);
	CHECK_EQ(menu_input(&m, CROSS, 80), MENU_CHEATS_CHANGED);
	CHECK(!cs.c[0].on);
	/* Abajo baja; en el último («Volver») × vuelve a la lista. */
	menu_input(&m, 0, 90);
	menu_input(&m, DOWN, 100);
	CHECK_EQ(m.cht_sel, 1);
	menu_input(&m, 0, 110);
	menu_input(&m, DOWN, 120);
	menu_input(&m, 0, 130);
	menu_input(&m, DOWN, 140);
	CHECK_EQ(m.cht_sel, 3);
	menu_input(&m, 0, 150);
	menu_input(&m, DOWN, 160);
	CHECK_EQ(m.cht_sel, 0);  /* da la vuelta */
	menu_input(&m, 0, 170);
	menu_input(&m, UP, 180);
	CHECK_EQ(m.cht_sel, 3);
	menu_input(&m, 0, 190);
	CHECK_EQ(menu_input(&m, CROSS, 200), MENU_NONE);
	CHECK_EQ(m.page, MENU_PAGE_GAMES);
	CHECK_EQ(m.sel, 1);  /* la misma ROM */
	/* ○ también vuelve, y □ cierra lo que □ abrió. */
	menu_cheats_open(&m, &cs, "ROM 01");
	menu_input(&m, 0, 210);
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_CIRCLE), 220), MENU_NONE);
	CHECK_EQ(m.page, MENU_PAGE_GAMES);
	menu_cheats_open(&m, &cs, "ROM 01");
	menu_input(&m, 0, 230);
	CHECK_EQ(menu_input(&m, BIT(SALSA64_DS3_SQUARE), 240), MENU_NONE);
	CHECK_EQ(m.page, MENU_PAGE_GAMES);

	/* Dibujo de la página (con y sin tele ancha) sin descartar nada. */
	menu_cheats_open(&m, &cs, "ROM 01 con un título bastante largo para que no quepa");
	uint32_t dropped = rsxkit_ui_dropped();  /* acumulado: otras pruebas apuran el lote a propósito */
	for (int k = 0; k < 2; k++) {
		float lw = k ? 960.0f : 1280.0f;
		m.cht_sel = 2;
		uint32_t n = frames(&m, 1000000, 1400000, lw);
		CHECK(n > 0);
		CHECK_EQ(rsxkit_ui_dropped(), dropped);
	}
	rsxkit_ui_batch(NULL, 0, 0, 0);
}

void t_suite_menu(void)
{
	RUN(menu_header_three_formats);
	RUN(menu_header_title_cleanup);
	RUN(menu_rom_extensions);
	RUN(menu_sort_by_title);
	RUN(menu_sort_by_filename);
	RUN(menu_scan_dir);
	RUN(menu_last_rom);
	RUN(menu_needs_release_first);
	RUN(menu_refresh_buttons);
	RUN(menu_move_and_wrap);
	RUN(menu_directory_navigation);
	RUN(menu_repeat);
	RUN(menu_video_file);
	RUN(menu_pages);
	RUN(menu_options_page);
	RUN(menu_game_page);
	RUN(menu_density);
	RUN(menu_controls);
	RUN(menu_settings_file);
	RUN(menu_autopilot_opens_target);
	RUN(text_glyph_bytes);
	RUN(text_clip_utf8_ellipsis);
	RUN(ui_atlas_scale2x);
	RUN(ui_geometry);
	RUN(menu_frame_geometry);
	RUN(menu_options_help);
	RUN(menu_cfg_differs);
	RUN(menu_cheats_page);
	RUN(menu_cover_state);
	RUN(version_texts);
	RUN(menu_version_place);
	RUN(menu_preview);
	romlist_free(nav_list);
	romlist_free(frame_list);
}
