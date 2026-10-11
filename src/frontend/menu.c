/*
 * Salsa64 — menú para elegir ROM. Ver menu.h.
 */
#include "menu.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/rom.h"
#include "core/save.h"
#include "padmap.h"

#define BIT(b) (1u << (b))
#define DIRS   (BIT(SALSA64_DS3_UP) | BIT(SALSA64_DS3_DOWN) | BIT(SALSA64_DS3_LEFT) | BIT(SALSA64_DS3_RIGHT))

/* Colores 0xRRGGBB. */
#define C_BACK    MENU_BACK_RGB
#define C_TEXT    0xE8ECF0u
#define C_DIM     0x8494A4u
#define C_ACCENT  0x7FB2FFu
#define C_BAR     0x24508Cu
#define C_RULE    0x34404Cu
#define C_WARN    0xE0A050u
#define C_EXIT    0xF0C070u

/* Márgenes del lienzo (de sobra para la zona segura de una tele). */
#define MX 24
#define MY 14
#define ROW_H (RSXKIT_FONT_H + 3)
#define CW RSXKIT_FONT_W

static int entries(const struct menu *m)
{
	return (int)m->list->count + 1;  /* + «Salir al XMB» */
}

void menu_init(struct menu *m, const struct romlist *list, int sel)
{
	memset(m, 0, sizeof *m);
	m->list = list;
	m->sel = sel >= 0 && sel < (int)list->count ? sel : 0;
	m->rows = 10;
	m->dirty = true;
}

int menu_selected(const struct menu *m)
{
	return m->sel < (int)m->list->count ? m->sel : -1;
}

void menu_set_status(struct menu *m, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(m->status, sizeof m->status, fmt, ap);
	va_end(ap);
	m->dirty = true;
}

static void move(struct menu *m, uint32_t dir, bool wrap)
{
	int n = entries(m), page = m->rows > 1 ? m->rows - 1 : 1, s = m->sel;
	if (dir == BIT(SALSA64_DS3_UP))
		s = s > 0 ? s - 1 : wrap ? n - 1 : 0;
	else if (dir == BIT(SALSA64_DS3_DOWN))
		s = s < n - 1 ? s + 1 : wrap ? 0 : n - 1;
	else if (dir == BIT(SALSA64_DS3_LEFT))
		s = s - page < 0 ? 0 : s - page;
	else if (dir == BIT(SALSA64_DS3_RIGHT))
		s = s + page > n - 1 ? n - 1 : s + page;
	if (s != m->sel) {
		m->sel = s;
		m->dirty = true;
	}
}

enum menu_action menu_input(struct menu *m, uint32_t buttons, uint64_t now_us)
{
	uint32_t pressed = buttons & ~m->held;
	m->held = buttons;
	if (!m->armed) {
		m->armed = buttons == 0;
		return MENU_NONE;
	}
	if (pressed & BIT(SALSA64_DS3_CROSS))
		return menu_selected(m) >= 0 ? MENU_OPEN : MENU_QUIT;

	/* Una sola dirección a la vez: la recién pulsada manda. */
	uint32_t dir = pressed & DIRS;
	if (dir) {
		dir &= ~dir + 1;
		move(m, dir, true);
		m->rep_button = dir;
		m->rep_since = now_us;
		m->rep_next = now_us + MENU_REPEAT_DELAY_US;
	} else if (m->rep_button && (buttons & m->rep_button)) {
		if (now_us >= m->rep_next) {
			move(m, m->rep_button, false);
			m->rep_next = now_us + (now_us - m->rep_since >= MENU_REPEAT_FAST_AFTER_US ? MENU_REPEAT_FAST_US
												   : MENU_REPEAT_US);
		}
	} else {
		m->rep_button = 0;
	}
	return MENU_NONE;
}

uint32_t menu_autopilot(const struct menu *m, int target, uint32_t step)
{
	if (step & 1)
		return 0;
	if (m->sel < target)
		return BIT(SALSA64_DS3_DOWN);
	if (m->sel > target)
		return BIT(SALSA64_DS3_UP);
	return BIT(SALSA64_DS3_CROSS);
}

/* ---- Dibujo ------------------------------------------------------------------- */

static void text(const rsxkit_canvas *c, int x, int y, const char *s, int cells, uint32_t rgb)
{
	if (cells > 0)
		rsxkit_text_draw(c, x, y, s, (uint32_t)cells, rgb, 1);
}

static void rule(const rsxkit_canvas *c, int y)
{
	rsxkit_fill(c, MX, y, (int32_t)c->width - 2 * MX, 1, C_RULE);
}

static void size_text(uint32_t bytes, char *buf, size_t n)
{
	if (bytes >= (1u << 20) && bytes % (1u << 20) == 0)
		snprintf(buf, n, "%u MB", bytes >> 20);
	else if (bytes >= (1u << 20))
		snprintf(buf, n, "%.1f MB", bytes / 1048576.0);
	else
		snprintf(buf, n, "%u KB", (bytes + 1023) >> 10);
}

static void header(const rsxkit_canvas *c, const struct menu *m)
{
	int w = (int)c->width;
	text(c, MX, MY, "Salsa64", 7, C_ACCENT);
	text(c, MX + 9 * CW, MY, "Elige un juego", 30, C_TEXT);
	char count[32];
	unsigned n = m->list->count;
	snprintf(count, sizeof count, n == 1 ? "%u juego" : "%u juegos", n);
	text(c, w - MX - (int)rsxkit_text_cells(count) * CW, MY, count, 16, C_DIM);
	rule(c, MY + RSXKIT_FONT_H + 4);
}

/* Columnas de una fila: título, archivo, guardado y región. */
struct cols {
	int title, name, name_cells, save, region;
};

static struct cols columns(int w)
{
	struct cols k;
	k.region = w - MX - 12 * CW;
	k.save = k.region - 2 * CW - 10 * CW;
	k.title = MX + 2 * CW;
	k.name = k.title + (ROMLIST_TITLE_MAX + 2) * CW;
	k.name_cells = (k.save - 2 * CW - k.name) / CW;
	return k;
}

static void row(const rsxkit_canvas *c, const struct menu *m, int i, int y, const struct cols *k)
{
	bool sel = i == m->sel;
	if (sel)
		rsxkit_fill(c, MX - 6, y - 2, (int32_t)c->width - 2 * MX + 12, ROW_H, C_BAR);
	if (sel)
		text(c, MX, y, "▶", 1, C_TEXT);
	if (i == (int)m->list->count) {
		text(c, k->title, y, "Salir al XMB", 40, sel ? C_TEXT : C_EXIT);
		return;
	}
	const struct rom_entry *e = &m->list->e[i];
	text(c, k->title, y, romlist_display_title(e), ROMLIST_TITLE_MAX, C_TEXT);
	text(c, k->name, y, romlist_name(e), k->name_cells, sel ? C_TEXT : C_DIM);
	bool emulated = e->save == SALSA64_SAVE_NONE || salsa64_save_emulated((enum salsa64_save_type)e->save);
	text(c, k->save, y, romlist_save_label(e->save), 10, emulated ? (sel ? C_TEXT : C_DIM) : C_WARN);
	text(c, k->region, y, romlist_region_name(e->region), 12, sel ? C_TEXT : C_DIM);
}

static void footer(const rsxkit_canvas *c, const struct menu *m)
{
	int w = (int)c->width, h = (int)c->height, cells = (w - 2 * MX) / CW;
	int y = h - MY - 2 * ROW_H;
	rule(c, y - 5);
	char line[ROMLIST_PATH_MAX + 64];
	int s = menu_selected(m);
	if (s >= 0) {
		const struct rom_entry *e = &m->list->e[s];
		char size[16];
		size_text(e->size, size, sizeof size);
		char code[16] = "";
		if (e->code[0])
			snprintf(code, sizeof code, "%s v%u · ", e->code, e->version);
		snprintf(line, sizeof line, "%s%s · %s · guardado %s%s · %s", code, size,
			 e->tv == SALSA64_TV_PAL ? "PAL" : e->tv == SALSA64_TV_MPAL ? "PAL-M" : "NTSC",
			 romlist_save_label(e->save),
			 e->save == SALSA64_SAVE_NONE || salsa64_save_emulated((enum salsa64_save_type)e->save)
				 ? "" : " (sin emular)", e->path);
	} else {
		snprintf(line, sizeof line, "Cierra Salsa64 y vuelve al menú de la consola.");
	}
	text(c, MX, y, line, cells, C_DIM);
	y += ROW_H;
	if (m->status[0])
		text(c, MX, y, m->status, cells, C_WARN);
	else
		text(c, MX, y, "× abrir   ↑↓ elegir   ←→ página   En el juego, START + SELECT vuelve aquí", cells, C_DIM);
}

void menu_draw(struct menu *m, const rsxkit_canvas *c)
{
	int w = (int)c->width, h = (int)c->height;
	rsxkit_fill(c, 0, 0, w, h, C_BACK);
	header(c, m);

	int y0 = MY + RSXKIT_FONT_H + 12;
	int y1 = h - MY - 2 * ROW_H - 10;
	m->rows = (y1 - y0) / ROW_H;
	if (m->rows < 1)
		m->rows = 1;
	int n = entries(m);
	int first = 0;
	if (m->list->count == 0) {
		/* Lista vacía: dónde se buscó, y «Salir al XMB» abajo. */
		text(c, MX + 2 * CW, y0, "No encontré ROMs (.z64, .v64, .n64) en:", 60, C_TEXT);
		for (unsigned i = 0; i < m->ndirs && i < MENU_DIRS_MAX; i++)
			text(c, MX + 4 * CW, y0 + (int)(i + 1) * ROW_H, m->dirs[i], 60, C_DIM);
		first = (int)m->ndirs + 2;
	}
	int rows = m->rows - first;
	if (rows < 1)
		rows = 1;
	if (m->sel < m->top)
		m->top = m->sel;
	if (m->sel >= m->top + rows)
		m->top = m->sel - rows + 1;
	if (m->top > n - rows)
		m->top = n - rows > 0 ? n - rows : 0;

	struct cols k = columns(w);
	for (int i = 0; i < rows && m->top + i < n; i++)
		row(c, m, m->top + i, y0 + (first + i) * ROW_H, &k);
	if (m->top > 0)
		text(c, w - MX - CW, y0, "▲", 1, C_ACCENT);
	if (m->top + rows < n)
		text(c, w - MX - CW, y0 + (m->rows - 1) * ROW_H, "▼", 1, C_ACCENT);

	footer(c, m);
	m->dirty = false;
}

void menu_draw_loading(const rsxkit_canvas *c, const struct rom_entry *e)
{
	int w = (int)c->width, h = (int)c->height;
	rsxkit_fill(c, 0, 0, w, h, C_BACK);
	char line[ROMLIST_PATH_MAX + 32];
	snprintf(line, sizeof line, "Cargando %s…", romlist_display_title(e));
	int cells = (int)rsxkit_text_cells(line), max = (w - 2 * MX) / CW;
	if (cells > max)
		cells = max;
	text(c, (w - cells * CW) / 2, h / 2 - ROW_H, line, max, C_TEXT);
	cells = (int)rsxkit_text_cells(e->path);
	if (cells > max)
		cells = max;
	text(c, (w - cells * CW) / 2, h / 2 + 4, e->path, max, C_DIM);
}
