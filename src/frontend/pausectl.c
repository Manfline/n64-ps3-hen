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

void pausectl_init(struct pausectl *p, bool confirm)
{
	memset(p, 0, sizeof *p);
	p->confirm = confirm;
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
	bool combo = false;

	p->intercepted = intercepted;
	if (!in_game)
		p->dialog = false;
	for (unsigned i = 0; i < PAUSECTL_PORTS; i++) {
		uint32_t b = sys ? 0 : raw[i];
		p->swallow[i] &= b;  /* lo soltado vuelve a contar */
		uint32_t r = b & ~p->prev[i];
		/* El atajo: los dos apretados y uno recién (mantenerlo no lo
		 * repite; soltar uno y volver a apretarlo, sí). */
		if ((b & QUIT) == QUIT && (r & QUIT))
			combo = true;
		rise |= r & ~p->swallow[i];
		p->prev[i] = b;
		now[i] = b;
	}

	bool was = p->dialog;
	if (!sys && in_game) {
		if (p->dialog) {
			if (rise & BIT(SALSA64_DS3_CROSS)) {
				p->dialog = false;
				act = PAUSECTL_EXIT;
			} else if ((rise & BIT(SALSA64_DS3_CIRCLE)) || combo) {
				p->dialog = false;
			}
		} else if (combo) {
			if (p->confirm)
				p->dialog = true;
			else
				act = PAUSECTL_EXIT;
		}
	}
	if (p->dialog != was || act != PAUSECTL_NONE)
		for (unsigned i = 0; i < PAUSECTL_PORTS; i++)
			p->swallow[i] |= now[i];
	for (unsigned i = 0; i < PAUSECTL_PORTS; i++)
		game[i] = sys || p->dialog ? 0 : now[i] & ~p->swallow[i];
	return act;
}

uint32_t pausectl_reasons(const struct pausectl *p)
{
	return (pausectl_overlay(p) || p->intercepted ? PAUSECTL_SYSTEM : 0) | (p->dialog ? PAUSECTL_CONFIRM : 0);
}

void pausectl_game_end(struct pausectl *p)
{
	p->dialog = false;
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
