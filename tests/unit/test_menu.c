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
 * Con SALSA64_MENU_PPM=<archivo> en el host, el menú de prueba se guarda
 * como imagen para revisarlo a ojo.
 */
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rsxkit/text.h>

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
	menu_init(m, &nav_list, sel);
	m->rows = 5;
}

static void menu_needs_release_first(void)
{
	struct menu m;
	nav_setup(&m, 4, 2);
	CHECK_EQ(m.sel, 2);
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

	nav_setup(&m, 4, 99);  /* fuera de rango: la primera */
	CHECK_EQ(m.sel, 0);
}

static void menu_move_and_wrap(void)
{
	struct menu m;
	nav_setup(&m, 3, 0);  /* 3 ROMs + «Salir al XMB» */
	menu_input(&m, 0, 0);
	m.dirty = false;
	menu_input(&m, DOWN, 10);
	CHECK_EQ(m.sel, 1);
	CHECK(m.dirty);
	menu_input(&m, 0, 20);
	menu_input(&m, UP, 30);
	menu_input(&m, 0, 40);
	menu_input(&m, UP, 50);   /* vuelta: de la primera a «Salir» */
	CHECK_EQ(m.sel, 3);
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
	CHECK_EQ(m.sel, 40);
	/* Soltar corta la repetición. */
	menu_input(&m, 0, u + 200 * MENU_REPEAT_FAST_US);
	CHECK_EQ(m.rep_button, 0);
}

static void menu_pages(void)
{
	struct menu m;
	nav_setup(&m, 20, 0);  /* filas de 5: una página avanza 4 */
	menu_input(&m, 0, 0);
	menu_input(&m, RIGHT, 10);
	CHECK_EQ(m.sel, 4);
	menu_input(&m, 0, 20);
	menu_input(&m, LEFT, 30);
	CHECK_EQ(m.sel, 0);
	menu_input(&m, 0, 40);
	menu_input(&m, LEFT, 50);  /* no da la vuelta */
	CHECK_EQ(m.sel, 0);
	m.sel = 18;
	menu_input(&m, 0, 60);
	menu_input(&m, RIGHT, 70);
	CHECK_EQ(m.sel, 20);
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
	a = MENU_NONE;
	for (step = 0; step < 40 && a == MENU_NONE; step++)
		a = menu_input(&m, menu_autopilot(&m, 6, step), 1000000 + step * 20000);
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

static void write_ppm(const char *path, const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%u %u\n255\n", w, h);
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
			fwrite(px + y * pitch + 4 * x + 1, 1, 3, f);  /* X, R, G, B */
	fclose(f);
}

static void menu_draw_canvas(void)
{
	static uint8_t px[360][640 * 4];
	rsxkit_canvas c = { px, 640, 360, 640 * 4, RSXKIT_XRGB8888 };
	static struct romlist l;
	memset(&l, 0, sizeof l);
	static const char *titles[] = { "THE LEGEND OF ZELDA", "SUPER MARIO 64", "GOLDENEYE", "MARIO KART 64",
					"Perfect Dark", "CONKER BFD", "STARFOX64", "" };
	static const char *codes[] = { "NZLE", "NSME", "NGEE", "NKTE", "NPDE", "NFUE", "NFXE", "NXXE" };
	for (unsigned i = 0; i < 8; i++) {
		uint8_t h[0x40];
		struct rom_entry *e = &l.e[l.count++];
		memset(e, 0, sizeof *e);
		snprintf(e->path, sizeof e->path, "/dev_hdd0/salsa64/rom-con-nombre-largo-número-%u.z64", i);
		e->name = (uint16_t)strlen("/dev_hdd0/salsa64/");
		e->size = (i + 1) << 22;
		make_header(h, titles[i], codes[i], 0);
		romlist_parse_header(h, NULL, 0, e);
	}
	romlist_sort(&l);
	struct menu m;
	menu_init(&m, &l, 3);
	menu_draw(&m, &c);
	CHECK(!m.dirty);
	CHECK(m.rows >= 10);
	/* La barra de la selección (fila 3) está pintada a la izquierda. */
	int y = 14 + 13 + 12 + 3 * (13 + 3) + 4;
	const uint8_t *p = px[y] + 4 * 20;
	CHECK_EQ(p[1] << 16 | p[2] << 8 | p[3], 0x24508C);
	const char *out = getenv("SALSA64_MENU_PPM");
	if (out)
		write_ppm(out, &px[0][0], 640, 360, 640 * 4);

	/* Con mensaje, lista vacía y en 640 × 480: no se sale del lienzo (ASan). */
	static uint8_t big[480][640 * 4];
	rsxkit_canvas c43 = { big, 640, 480, 640 * 4, RSXKIT_XRGB8888 };
	menu_set_status(&m, "No se pudo abrir %s", l.e[0].path);
	menu_draw(&m, &c43);
	static struct romlist empty;
	menu_init(&m, &empty, 0);
	m.dirs[m.ndirs++] = "/dev_hdd0/salsa64";
	m.dirs[m.ndirs++] = "/dev_usb000/salsa64";
	menu_draw(&m, &c);
	CHECK_EQ(menu_selected(&m), -1);
	menu_draw_loading(&c, &l.e[0]);
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
	RUN(menu_pages);
	RUN(menu_autopilot_opens_target);
	RUN(text_glyph_bytes);
	RUN(text_clip_utf8_ellipsis);
	RUN(menu_draw_canvas);
}
