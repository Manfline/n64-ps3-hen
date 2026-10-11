/*
 * Salsa64 — destinos de render del RDP. Ver targets.h.
 */
#include "rdp/targets.h"

#include <string.h>

/* G_IM_SIZ_* de G_SETCIMG (gbi): 4, 8, 16 y 32 bits. */
#define IM_SIZ_8b  1u
#define IM_SIZ_32b 3u

void rdp_targets_init(struct rdp_targets *s)
{
	memset(s, 0, sizeof *s);
	s->shown = -1;
	s->vi_width = 320;
}

static bool stale(const struct rdp_targets *s, const struct rdp_target *t)
{
	return s->field - t->last_field > RDP_TARGETS_STALE;
}

/* Orden de reciclado: menor es más reciclable. */
static int evict_class(const struct rdp_targets *s, const struct rdp_target *t)
{
	if (stale(s, t))
		return 0;
	if (t->width != s->vi_width)
		return 1;
	return 2;
}

int rdp_targets_get(struct rdp_targets *s, uint32_t addr, uint32_t width, uint32_t siz, uint32_t min_lh,
		    bool *fresh)
{
	int free_i = -1, victim = -1;
	uint32_t lw = width ? width : 320;
	uint32_t lh = lw * 3 / 4;
	if (min_lh > lh)
		lh = min_lh;
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		struct rdp_target *t = &s->t[i];
		if (!t->used) {
			if (free_i < 0)
				free_i = i;
			continue;
		}
		if (t->addr == addr && t->width == width) {
			if (t->lh >= lh) {
				*fresh = false;
				return i;
			}
			/* Crece: la superficie se vuelve a crear (lo dibujado hasta
			 * ahora se pierde; pasa una vez). */
			t->lh = lh;
			t->drawn = false;
			s->created++;
			*fresh = true;
			return i;
		}
		if (i == s->shown)
			continue;
		if (victim < 0) {
			victim = i;
			continue;
		}
		const struct rdp_target *v = &s->t[victim];
		int ct = evict_class(s, t), cv = evict_class(s, v);
		if (ct < cv || (ct == cv && (int32_t)(t->last_field - v->last_field) < 0))
			victim = i;
	}
	int i = free_i >= 0 ? free_i : victim;
	if (i < 0)
		i = s->shown >= 0 ? s->shown : 0;  /* no pasa: hay más de una ranura */
	struct rdp_target *t = &s->t[i];
	t->used = true;
	t->drawn = false;
	t->addr = addr;
	t->width = width;
	t->bpp = siz == IM_SIZ_32b ? 4 : siz == IM_SIZ_8b ? 1 : 2;
	t->lw = lw;
	t->lh = lh;
	t->last_field = s->field;
	s->created++;
	*fresh = true;
	return i;
}

void rdp_targets_drawn(struct rdp_targets *s, int slot)
{
	if (slot < 0 || slot >= RDP_TARGETS_MAX)
		return;
	s->t[slot].last_field = s->field;
	s->t[slot].drawn = true;
}

/* ¿El destino cubre el origen del VI? Da la línea lógica donde cae. */
static bool covers(const struct rdp_target *t, uint32_t origin, uint32_t *y0)
{
	uint32_t line = t->width * t->bpp;
	if (!line || origin < t->addr || origin >= t->addr + line * t->lh)
		return false;
	*y0 = (origin - t->addr) / line;
	return true;
}

void rdp_targets_present(struct rdp_targets *s, const struct salsa64_vi_frame *f, uint32_t field,
			 struct rdp_present *out)
{
	s->field = field;
	/* El campo solo avanza cuando el VI muestra algo: lo dibujado mientras
	 * estuvo en negro (SM64 lo apaga 26 campos al cambiar de escena) queda
	 * con el campo de ahora, no con el de antes del apagado. */
	for (int i = 0; i < RDP_TARGETS_MAX; i++)
		if (s->t[i].drawn) {
			s->t[i].last_field = field;
			s->t[i].drawn = false;
		}
	if (f->width)
		s->vi_width = f->width;

	int best = -1;
	uint32_t best_y0 = 0;
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		const struct rdp_target *t = &s->t[i];
		uint32_t y0;
		if (!t->used || stale(s, t) || !covers(t, f->origin, &y0))
			continue;
		if (best >= 0) {
			const struct rdp_target *b = &s->t[best];
			bool tw = t->width == f->width, bw = b->width == f->width;
			if (bw != tw) {
				if (bw)
					continue;
			} else if (y0 != best_y0) {
				if (y0 > best_y0)
					continue;
			} else if ((int32_t)(t->last_field - b->last_field) <= 0) {
				continue;
			}
		}
		best = i;
		best_y0 = y0;
	}
	if (best < 0 && s->shown >= 0 && s->t[s->shown].used && f->origin == s->shown_origin &&
	    field - s->shown_field <= 2 && covers(&s->t[s->shown], f->origin, &best_y0))
		best = s->shown;

	s->shown = best;
	s->shown_origin = f->origin;
	s->shown_field = field;
	out->slot = best;
	out->y0 = out->w = out->h = 0;
	if (best < 0)
		return;
	const struct rdp_target *t = &s->t[best];
	out->y0 = best_y0;
	out->w = f->width < t->lw ? f->width : t->lw;
	out->h = f->height < t->lh - best_y0 ? f->height : t->lh - best_y0;
}
