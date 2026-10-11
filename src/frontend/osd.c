/*
 * Salsa64 — aviso y contador sobre el juego (PS3). Ver osd.h.
 */
#include "osd.h"

#include <stdarg.h>
#include <stdio.h>

#include <rsxkit/ui.h>
#include <rsxkit/text.h>

static struct {
	char text[96];
	uint64_t until;
} notice;

void osd_notice(uint64_t now_us, const char *fmt, ...)
{
	va_list ap;
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
