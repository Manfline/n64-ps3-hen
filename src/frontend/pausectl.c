/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — pausa del juego: menú del sistema y cuadro de confirmación.
 * Ver pausectl.h.
 */
#include "pausectl.h"

#include <string.h>

#include <rsxkit/text.h>

#include "padmap.h"

#define BIT(b) (1u << (b))
#define QUIT   (BIT(SALSA64_DS3_START) | BIT(SALSA64_DS3_SELECT))
#define OPEN   (BIT(SALSA64_DS3_SQUARE) | BIT(SALSA64_DS3_SELECT))
#define HOLD_FIRST  18u  /* vueltas antes de que ↑↓ repitan */
#define HOLD_EVERY  3u

void pausectl_init(struct pausectl *p, bool confirm)
{
	memset(p, 0, sizeof *p);
	p->confirm = confirm;
	p->toggle = -1;
	p->menu_do = -1;
	p->image_do = -1;
}

void pausectl_system_menu(struct pausectl *p, bool open)
{
	p->system = open;
}

void pausectl_system_draw(struct pausectl *p, bool on)
{
	p->drawing = on;
}

enum pausectl_action pausectl_pads(struct pausectl *p, const uint32_t raw[PAUSECTL_PORTS], bool intercepted,
				   bool in_game, uint32_t game[PAUSECTL_PORTS])
{
	enum pausectl_action act = PAUSECTL_NONE;
	bool sys = intercepted || pausectl_overlay(p);
	uint32_t now[PAUSECTL_PORTS], rise = 0;
	bool combo = false, open_combo = false;
	uint32_t held = 0;

	p->intercepted = intercepted;
	if (!in_game)
		p->dialog = p->panel = p->menu = p->image = false;
	for (unsigned i = 0; i < PAUSECTL_PORTS; i++) {
		uint32_t b = sys ? 0 : raw[i];
		p->swallow[i] &= b;  /* lo soltado vuelve a contar */
		uint32_t r = b & ~p->prev[i];
		/* El atajo: los dos apretados y uno recién (mantenerlo no lo
		 * repite; soltar uno y volver a apretarlo, sí). */
		if ((b & QUIT) == QUIT && (r & QUIT))
			combo = true;
		if ((b & OPEN) == OPEN && (r & OPEN))
			open_combo = true;
		held |= b & ~p->swallow[i];
		rise |= r & ~p->swallow[i];
		p->prev[i] = b;
		now[i] = b;
	}

	bool was = p->dialog, was_panel = p->panel, was_menu = p->menu, was_image = p->image, denied_now = false;
	if (!sys && in_game) {
		if (p->image) {
			uint32_t up = BIT(SALSA64_DS3_UP), down = BIT(SALSA64_DS3_DOWN);
			bool left = rise & BIT(SALSA64_DS3_LEFT), right = rise & BIT(SALSA64_DS3_RIGHT);
			if ((rise & BIT(SALSA64_DS3_START)) || combo) {
				p->image = false;  /* todo cerrado: el juego sigue */
			} else if ((rise & BIT(SALSA64_DS3_CIRCLE)) ||
				   (p->image_sel == IM_BACK && (rise & BIT(SALSA64_DS3_CROSS)))) {
				p->image = false;
				p->menu = true;
			} else if (rise & (up | down)) {
				int step = (rise & up) ? IM_ROWS - 1 : 1;
				do  /* sin máscara, la fila «Densidad» no está */
					p->image_sel = (p->image_sel + step) % IM_ROWS;
				while (p->image_sel == IM_DENSITY && !p->image_density);
			} else if (p->image_sel <= IM_SCALE && (left || right || (rise & BIT(SALSA64_DS3_CROSS)))) {
				p->image_do = p->image_sel;
				p->image_arg = left ? -1 : 1;
			} else if (p->image_sel == IM_SAVE && (rise & BIT(SALSA64_DS3_CROSS))) {
				p->image_do = IM_SAVE;
			}
		} else if (p->panel) {
			if ((rise & (BIT(SALSA64_DS3_CIRCLE) | BIT(SALSA64_DS3_START))) || open_combo || combo) {
				p->panel = false;
			} else {
				int step = 0;
				uint32_t dir = held & (BIT(SALSA64_DS3_UP) | BIT(SALSA64_DS3_DOWN));
				if (rise & BIT(SALSA64_DS3_RIGHT) || rise & BIT(SALSA64_DS3_R1))
					step = PAUSECTL_PANEL_PAGE;
				else if (rise & BIT(SALSA64_DS3_LEFT) || rise & BIT(SALSA64_DS3_L1))
					step = -PAUSECTL_PANEL_PAGE;
				else if (dir && dir != p->hold_bit) {
					p->hold_bit = dir;
					p->hold_n = 0;
					step = dir == BIT(SALSA64_DS3_DOWN) ? 1 : -1;
				} else if (dir) {
					p->hold_n++;
					if (p->hold_n > HOLD_FIRST && (p->hold_n - HOLD_FIRST) % HOLD_EVERY == 0)
						step = dir == BIT(SALSA64_DS3_DOWN) ? 1 : -1;
				} else {
					p->hold_bit = p->hold_n = 0;
				}
				if (step && p->panel_rows) {
					int n = (int)p->panel_rows, s = p->panel_sel + step;
					/* Un paso suelto da la vuelta; los saltos se paran en los extremos. */
					if (step == 1 || step == -1)
						s = (s % n + n) % n;
					else
						s = s < 0 ? 0 : s >= n ? n - 1 : s;
					p->panel_sel = s;
				}
				if ((rise & BIT(SALSA64_DS3_CROSS)) && p->panel_rows)
					p->toggle = p->panel_sel;
			}
		} else if (p->menu) {
			uint32_t up = BIT(SALSA64_DS3_UP), down = BIT(SALSA64_DS3_DOWN);
			if ((rise & (BIT(SALSA64_DS3_CIRCLE) | BIT(SALSA64_DS3_START))) || combo) {
				p->menu = false;
			} else if (rise & up) {
				p->menu_sel = (p->menu_sel + PM_ROWS - 1) % PM_ROWS;
			} else if (rise & down) {
				p->menu_sel = (p->menu_sel + 1) % PM_ROWS;
			} else if (p->menu_sel == PM_SLOT && (rise & (BIT(SALSA64_DS3_LEFT) | BIT(SALSA64_DS3_RIGHT)))) {
				p->menu_do = PM_SLOT;
				p->menu_arg = rise & BIT(SALSA64_DS3_RIGHT) ? 1 : -1;
			} else if (rise & BIT(SALSA64_DS3_CROSS)) {
				switch (p->menu_sel) {
				case PM_RESUME:
					p->menu = false;
					break;
				case PM_SAVE:
				case PM_LOAD:
					p->menu = false;
					p->menu_do = p->menu_sel;
					break;
				case PM_SLOT:
					p->menu_do = PM_SLOT;
					p->menu_arg = 1;
					break;
				case PM_CHEATS:
					if (p->panel_avail && p->panel_rows) {
						p->menu = false;
						p->panel = true;
						if (p->panel_sel < 0 || p->panel_sel >= (int)p->panel_rows)
							p->panel_sel = 0;
						p->hold_bit = p->hold_n = 0;
					} else {
						p->menu_do = PM_CHEATS;
					}
					break;
				case PM_IMAGE:
					p->menu = false;
					p->image = true;
					p->image_sel = IM_FILTER;
					break;
				case PM_FPS:
					p->menu_do = PM_FPS;
					break;
				case PM_EXIT:
					p->menu = false;
					if (p->confirm)
						p->dialog = true;
					else
						act = PAUSECTL_EXIT;
					break;
				}
			}
		} else if (p->dialog) {
			if (rise & BIT(SALSA64_DS3_CROSS)) {
				p->dialog = false;
				act = PAUSECTL_EXIT;
			} else if ((rise & BIT(SALSA64_DS3_CIRCLE)) || combo) {
				p->dialog = false;
			}
		} else if (combo) {
			p->menu = true;
			p->menu_sel = PM_RESUME;
		} else if (open_combo) {
			if (p->panel_avail && p->panel_rows) {
				p->panel = true;
				if (p->panel_sel < 0 || p->panel_sel >= (int)p->panel_rows)
					p->panel_sel = 0;
				p->hold_bit = p->hold_n = 0;
			} else {
				p->panel_denied = denied_now = true;
			}
		}
	}
	if (p->dialog != was || p->panel != was_panel || p->menu != was_menu || p->image != was_image || denied_now ||
	    act != PAUSECTL_NONE)
		for (unsigned i = 0; i < PAUSECTL_PORTS; i++)
			p->swallow[i] |= now[i];
	for (unsigned i = 0; i < PAUSECTL_PORTS; i++)
		game[i] = sys || p->dialog || p->panel || p->menu || p->image ? 0 : now[i] & ~p->swallow[i];
	return act;
}

uint32_t pausectl_reasons(const struct pausectl *p)
{
	return (pausectl_overlay(p) || p->intercepted ? PAUSECTL_SYSTEM : 0) | (p->dialog ? PAUSECTL_CONFIRM : 0) |
	       (p->panel ? PAUSECTL_PANEL : 0) | (p->menu || p->image ? PAUSECTL_MENU : 0);
}

void pausectl_game_end(struct pausectl *p)
{
	p->dialog = p->panel = p->menu = p->image = false;
	p->menu_do = -1;
	p->image_do = -1;
	p->panel_avail = false;
	p->panel_rows = 0;
	p->panel_sel = 0;
	p->toggle = -1;
}

unsigned pausectl_panel_top(int sel, unsigned rows)
{
	if (rows <= PAUSECTL_PANEL_VISIBLE)
		return 0;
	int top = sel - PAUSECTL_PANEL_VISIBLE / 2;
	int max = (int)rows - PAUSECTL_PANEL_VISIBLE;
	return (unsigned)(top < 0 ? 0 : top > max ? max : top);
}

/* Caracteres de un texto UTF-8. */
static unsigned cells(const char *s)
{
	unsigned n = 0;
	for (; *s; s++)
		n += ((unsigned char)*s & 0xC0) != 0x80;
	return n;
}

void pausectl_layout(const char *question, const char *answers, float lw, float lh, struct pausectl_box *b)
{
	const float pad = 28.0f, gap = 22.0f;
	b->qscale = 2.5f;
	b->ascale = 2.0f;
	float qw = cells(question) * RSXKIT_FONT_W * b->qscale, qh = RSXKIT_FONT_H * b->qscale;
	float aw = cells(answers) * RSXKIT_FONT_W * b->ascale, ah = RSXKIT_FONT_H * b->ascale;
	float inner = qw > aw ? qw : aw;
	b->w = inner + 2 * pad;
	b->h = qh + gap + ah + 2 * pad;
	b->x = (lw - b->w) / 2;
	b->y = (lh - b->h) / 2;
	b->qx = b->x + (b->w - qw) / 2;
	b->qy = b->y + pad;
	b->ax = b->x + (b->w - aw) / 2;
	b->ay = b->qy + qh + gap;
}
