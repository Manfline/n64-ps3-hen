/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — aviso y contador sobre el juego (PS3). Ver osd.h.
 */
#include "osd.h"

#include <stdarg.h>
#include <stdio.h>

#include <rsxkit/ui.h>
#include <rsxkit/text.h>

#include "cheats.h"
#include "i18n.h"
#include "pausectl.h"

static struct {
	char text[96];
	uint64_t until;
} notice;

void osd_notice(uint64_t now_us, const char *fmt, ...)
{
	va_list ap;
	fmt = tr(fmt);
	va_start(ap, fmt);
	vsnprintf(notice.text, sizeof notice.text, fmt, ap);
	va_end(ap);
	notice.until = now_us + OSD_NOTICE_US;
}

void osd_notice_clear(void)
{
	notice.until = 0;
}

/* Caracteres de un texto UTF-8 (para medirlo). */
static unsigned cells(const char *s)
{
	unsigned n = 0;
	for (; *s; s++)
		n += ((unsigned char)*s & 0xC0) != 0x80;
	return n;
}

void osd_draw(const struct osd_fps *f, uint64_t now_us, float lw, float lh)
{
	bool show_notice = now_us < notice.until;
	if (!show_notice && !f->on)
		return;
	rsxkit_ui_begin(lw, lh);
	if (f->on) {
		char line[96];
		const float sc = 1.5f, pad = 6.0f;
		osd_fps_text(f, line, sizeof line);
		float w = cells(line) * RSXKIT_FONT_W * sc, h = RSXKIT_FONT_H * sc;
		rsxkit_ui_rect(16.0f, 14.0f, w + 2 * pad, h + 2 * pad, 0x000000A0);
		rsxkit_ui_text(16.0f + pad, 14.0f + pad, line, sc, 0xFFFFFFFF, 0);
	}
	if (show_notice) {
		/* Abajo al centro; se desvanece en el último cuarto de segundo. */
		uint64_t left = notice.until - now_us;
		uint32_t a = left > 250000 ? 255 : (uint32_t)(left * 255 / 250000);
		const float sc = 2.0f, pad = 10.0f;
		float w = cells(notice.text) * RSXKIT_FONT_W * sc, h = RSXKIT_FONT_H * sc;
		float x = (lw - w) / 2 - pad, y = lh - h - 2 * pad - 48.0f;
		rsxkit_ui_rect(x, y, w + 2 * pad, h + 2 * pad, 0x10101000 | (a * 0xC0 / 255));
		rsxkit_ui_text(x + pad, y + pad, notice.text, sc, 0xFFFFFF00 | a, 0);
	}
	rsxkit_ui_end();
}

void osd_confirm_draw(const char *question, const char *answers, float lw, float lh)
{
	struct pausectl_box b;
	pausectl_layout(tr(question), tr(answers), lw, lh, &b);
	rsxkit_ui_begin(lw, lh);
	rsxkit_ui_rect(0.0f, 0.0f, lw, lh, 0x00000070);
	rsxkit_ui_rect(b.x - 2.0f, b.y - 2.0f, b.w + 4.0f, b.h + 4.0f, 0xFFFFFF60);
	rsxkit_ui_rect(b.x, b.y, b.w, b.h, 0x141414F0);
	rsxkit_ui_text(b.qx, b.qy, tr(question), b.qscale, 0xFFFFFFFF, 0);
	rsxkit_ui_text(b.ax, b.ay, tr(answers), b.ascale, 0xC8C8C8FF, 0);
	rsxkit_ui_end();
}

void osd_cheats_draw(const struct cheat_set *cs, int sel, float lw, float lh)
{
	const float sc = 1.6f, row_h = 36.0f, pad = 24.0f;
	unsigned rows = cs->n, vis = rows < PAUSECTL_PANEL_VISIBLE ? rows : PAUSECTL_PANEL_VISIBLE;
	unsigned top = pausectl_panel_top(sel, rows);
	float w = lw * 0.78f > 980.0f ? 980.0f : lw * 0.78f;
	float title_h = 56.0f, foot_h = 52.0f;
	float h = title_h + vis * row_h + foot_h + 2 * pad;
	float x = (lw - w) / 2, y = (lh - h) / 2;
	uint32_t maxc = (uint32_t)((w - 2 * pad - 90.0f) / (RSXKIT_FONT_W * sc));
	char line[CHEAT_DESC_MAX + 8];

	rsxkit_ui_begin(lw, lh);
	rsxkit_ui_rect(0.0f, 0.0f, lw, lh, 0x00000090);
	rsxkit_ui_rect(x - 2.0f, y - 2.0f, w + 4.0f, h + 4.0f, 0xFFFFFF60);
	rsxkit_ui_rect(x, y, w, h, 0x141414F4);
	snprintf(line, sizeof line, tr("Trucos  (%u activos de %u)"), cheats_active(cs), cs->n);
	rsxkit_ui_text(x + pad, y + pad, line, 2.0f, 0xFFFFFFFF, 0);
	for (unsigned i = 0; i < vis; i++) {
		unsigned k = top + i;
		float ry = y + pad + title_h + i * row_h;
		bool cur = (int)k == sel;
		if (cur)
			rsxkit_ui_rect(x + 8.0f, ry - 4.0f, w - 16.0f, row_h - 2.0f, 0x3A5A9AE0);
		snprintf(line, sizeof line, "[%c] ", cs->c[k].on ? 'x' : ' ');
		rsxkit_ui_text(x + pad, ry, line, sc, cs->c[k].on ? 0x9CFFA0FF : 0xB0B0B0FF, 0);
		rsxkit_ui_text(x + pad + 4 * RSXKIT_FONT_W * sc, ry, cs->c[k].desc, sc, cur ? 0xFFFFFFFF : 0xD8D8D8FF, maxc);
	}
	if (rows > vis) {
		/* Barra de posición a la derecha. */
		float th = vis * row_h, bh = th * vis / rows < 20.0f ? 20.0f : th * vis / rows;
		float by = y + pad + title_h + (th - bh) * top / (rows - vis);
		rsxkit_ui_rect(x + w - 14.0f, y + pad + title_h, 4.0f, th, 0xFFFFFF20);
		rsxkit_ui_rect(x + w - 14.0f, by, 4.0f, bh, 0xFFFFFFA0);
		snprintf(line, sizeof line, "%d / %u", sel + 1, rows);
		rsxkit_ui_text(x + w - pad - cells(line) * RSXKIT_FONT_W * 1.5f, y + pad + 6.0f, line, 1.5f, 0xB0B0B0FF, 0);
	}
	rsxkit_ui_text(x + pad, y + h - pad - RSXKIT_FONT_H * 1.5f, tr("×  Activar o desactivar     ↑↓  Elegir     ○  Cerrar"), 1.5f,
		       0xC8C8C8FF, 0);
	rsxkit_ui_end();
}

void osd_scanlines_draw(int level, float x, float y, float w, float h, unsigned lines, float lw, float lh)
{
	if (!lines || h / (float)lines < 1.5f)
		return;
	float pitch = h / (float)lines;
	uint32_t color = level >= 2 ? 0x000000A8u : 0x00000052u;
	rsxkit_ui_begin(lw, lh);
	for (unsigned i = 0; i < lines; i++)
		rsxkit_ui_rect(x, y + (float)i * pitch + pitch * 0.55f, w, pitch * 0.45f, color);
	rsxkit_ui_end();
}

void osd_pause_menu_draw(const struct osd_pause_menu *m, float lw, float lh)
{
	const float sc = 2.0f, row_h = 46.0f, pad = 26.0f, w = lw * 0.5f < 560.0f ? 560.0f : lw * 0.5f;
	float h = 60.0f + PM_ROWS * row_h + 50.0f + 2 * pad;
	float x = (lw - w) / 2, y = (lh - h) / 2;
	char line[96], val[40];
	rsxkit_ui_begin(lw, lh);
	rsxkit_ui_rect(0.0f, 0.0f, lw, lh, 0x00000090);
	rsxkit_ui_rect(x - 2.0f, y - 2.0f, w + 4.0f, h + 4.0f, 0xFFFFFF60);
	rsxkit_ui_rect(x, y, w, h, 0x141414F4);
	rsxkit_ui_text(x + pad, y + pad, tr("Pausa"), 2.5f, 0xFFFFFFFF, 0);
	for (int i = 0; i < PM_ROWS; i++) {
		float ry = y + pad + 60.0f + i * row_h;
		bool cur = i == m->sel, dim = false;
		const char *label = "";
		val[0] = '\0';
		switch (i) {
		case PM_RESUME: label = tr("Continuar"); break;
		case PM_SAVE:   label = tr("Guardar estado"); break;
		case PM_LOAD:   label = tr("Cargar estado"); dim = !m->slot_has; break;
		case PM_SLOT:
			snprintf(line, sizeof line, tr(m->slot_has ? "Ranura %u" : "Ranura %u (vacía)"), m->slot);
			snprintf(val, sizeof val, "◀  %s  ▶", line);
			label = "";
			break;
		case PM_CHEATS:
			label = tr("Trucos");
			dim = !m->cheats_total;
			if (m->cheats_total)
				snprintf(val, sizeof val, "%u / %u", m->cheats_on, m->cheats_total);
			break;
		case PM_FPS:
			label = tr("Mostrar FPS");
			snprintf(val, sizeof val, "%s", tr(m->fps_on ? "Sí" : "No"));
			break;
		case PM_EXIT: label = tr(m->in_menu ? "Volver al menú" : "Salir al XMB"); break;
		}
		if (cur)
			rsxkit_ui_rect(x + 8.0f, ry - 6.0f, w - 16.0f, row_h - 2.0f, 0x3A5A9AE0);
		uint32_t col = cur ? 0xFFFFFFFF : dim ? 0x808080FF : 0xD8D8D8FF;
		rsxkit_ui_text(x + pad, ry, label, sc, col, 0);
		if (val[0]) {
			float vx = i == PM_SLOT ? x + pad : x + w - pad - cells(val) * RSXKIT_FONT_W * sc;
			rsxkit_ui_text(vx, ry, val, sc, cur ? 0x9CC8FFFF : 0xA8B4C4FF, 0);
		}
	}
	rsxkit_ui_text(x + pad, y + h - pad - RSXKIT_FONT_H * 1.5f, tr("×  Aceptar     ↑↓  Elegir     ○  Continuar"), 1.5f, 0xC8C8C8FF, 0);
	rsxkit_ui_end();
}
